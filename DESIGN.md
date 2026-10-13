# MnemonicDB — design

A DuckDB storage extension providing an immutable, Datomic-style database
backed by RocksDB. Tables are attribute namespaces, every row is an entity
in one shared ID space, and DuckDB's `AT` clause selects the point in time
being queried.

Extension name: `mnemonicdb`. Storage type: `TYPE mnemonicdb`.

```sql
ATTACH 'data/app.db' AS d (TYPE mnemonicdb);

CREATE TABLE d.person (name VARCHAR, email VARCHAR PRIMARY KEY, tags LIST(VARCHAR));
CREATE TABLE d.account (owner REF, balance DECIMAL(18,2));

BEGIN;
INSERT INTO d.person (id, name, email) VALUES (tempid('alice'), 'Alice', 'a@x.com');
INSERT INTO d.account (owner, balance) VALUES (tempid('alice'), 100);
COMMIT;

SELECT p.name, a.balance
FROM d.person p JOIN d.account a ON a.owner = p.id;

SELECT * FROM d.person AT (VERSION => 1042);
SELECT * FROM d.person AT (TIMESTAMP => TIMESTAMPTZ '2026-10-01 00:00:00');
```

Target: DuckDB `main` (pinned commit), C++ storage-extension API. Single
process, single writer, any number of readers.

---

## 1. Data model

| Datomic            | Here                                                       |
|--------------------|------------------------------------------------------------|
| attribute `:person/name` | column `name` of table `person`                      |
| namespace          | table                                                      |
| entity             | a row's `id`; shared across all tables in one database     |
| transaction        | one DuckDB transaction that wrote at least one datom       |
| `:db/cardinality :many` | a top-level `LIST(T)` column                          |
| `:db.type/ref`     | the `REF` logical type (untyped, like Datomic)             |
| `:db.unique/value` | `UNIQUE`                                                   |
| `:db.unique/identity` | `PRIMARY KEY`, and `UNIQUE` used as an `ON CONFLICT` target |
| `:db/index`        | `CREATE INDEX` on the column (enables AVET)                |
| schema             | entities in the `sys` schema                               |

- **Membership.** An entity is "in" a table iff it currently has at least one
  attribute of that table. There is no marker datom.
- **`id` column.** Every table's first column is `id` (type `REF`), added
  automatically. It's a real column so INSERT can set it; DuckDB can't
  insert into virtual columns. A hidden virtual `rowid` column carries the
  same value and is the row identifier for UPDATE and DELETE. The names `id`
  and `rowid` are reserved.
- **NULL means absent.** A NULL column is no datom. Setting a column to NULL
  retracts it.
- **Value types.** Nearly every DuckDB type, nested arbitrarily. A top-level
  `LIST(T)` is cardinality-many; each element is its own datom, so values
  come back deduplicated and ordered by sort key (it behaves as a set). A
  nested list (e.g. inside a STRUCT, or the inner list of `LIST(LIST(T))`)
  is an ordinary single value.
- **Everything is an entity.** Attributes, table definitions,
  transactions and user entities all live in the same `u64` ID space,
  separated by partition.

## 2. Entity IDs and partitions

In memory an ID is a `u64`:

```
 63      56 55            40 39                                  0
+----------+----------------+-------------------------------------+
| part (8) |  zero (16)     |            local (40)               |
+----------+----------------+-------------------------------------+
```

Because the middle 16 bits are always zero, the canonical **packed** form is
6 bytes big-endian: `part (1) | local (5)`. Packed order equals numeric
order, so packed IDs can be used directly in keys.

Narrower encodings are used where the partition is implied by position:

| Partition | Num   | Holds                 | Local width in keys |
|-----------|-------|-----------------------|---------------------|
| `attr`    | 0     | attributes            | `u16` (2 bytes)     |
| `table`   | 1     | table definitions     | `u16` (2 bytes)     |
| `tx`      | 2     | transactions          | 40 bits (5 bytes)   |
| `db`      | 3     | default user entities | full packed (6)     |
| reserved  | 4–15  | future system use     |                     |
| user      | 16–254| user partitions, chosen by number in `tempid()` | full packed |
| `temp`    | 255   | tempid placeholders; never persisted |       |

Partitions are not created or registered: a user partition is just a number
in 16–254 passed to `tempid()`. Each partition in use has its own next-local
counter, stored in `meta` and advanced in the commit batch.

A REF column and the `id` column can hold an entity from any partition,
so in E and ref-V positions IDs are always the 6-byte packed form. The `A`
position is always a 2-byte attribute local, and `T` is always a 5-byte tx
local.

## 3. Value encoding

Values are encoded with DuckDB's memcmp-comparable sort keys
(`CreateSortKeyHelpers::CreateSortKey` / `DecodeSortKey` in
`duckdb/function/create_sort_key.hpp`), wrapped by `ValueCodec`
(`src/codec/value_codec.cpp`). The attribute determines the type, so no type
tag is stored.

- **Modifiers:** ascending, NULLS LAST. Inside nested values, NULL children
  sort last.
- **No top-level validity byte.** A sort key starts with a validity byte,
  but stored values are never NULL, so the codec strips it on encode and puts
  it back on decode. That saves one byte per datom per index.
- **Prefix-free.** Fixed-width types have a fixed length. VARCHAR and BLOB
  escape `0x00`/`0x01` and end with a `0x00` delimiter. Lists end with a
  delimiter, and structs are a concatenation of their fields. No encoding is
  a proper prefix of another, so an AVET prefix scan on `A + V` matches
  exactly V.
- **Decodable.** `DecodeSortKey` reports how many bytes it consumed, so a
  value can be decoded from the front of any buffer.
- **Supported types** (allowlist in `ValueCodec::CheckSupported`): booleans,
  every integer width including HUGEINT/UHUGEINT and BIGNUM, DECIMAL,
  FLOAT/DOUBLE, every DATE/TIME/TIMESTAMP variant, INTERVAL, VARCHAR, BLOB,
  BIT, UUID, ENUM, and STRUCT/UNION/LIST/MAP/ARRAY nested to any depth.
  VARIANT, GEOMETRY and anything else not on the list is rejected until
  tested.
- **Known caveats:** these come from DuckDB's sort-key format, not from SQL
  semantics. They affect AVET range scans, uniqueness and redundancy
  elimination, all of which compare encoded bytes.
  - VARCHAR orders by bytes; collations are ignored.
  - INTERVAL is encoded as raw (months, days, micros). `'1 month'` and
    `'30 days'` are distinct values, and `'40 days'` sorts before
    `'1 month'`.
  - TIME_TZ sorts by its packed bits, not by UTC instant.
  - BIT sorts by its padding byte first.
  - `-0.0` is stored as `0.0`, and all NaNs are stored as one value.
  - ENUM encodes the dictionary index, so changing an ENUM's values is a type
    change, which means a rewrite.
  - The empty STRUCT is rejected: its encoding would be zero bytes.
- **Trust boundary.** `ValueCodec::Decode` trusts its input, because store
  bytes come from this encoder and RocksDB checksums its blocks.
  `ValueCodec::Validate` / `EncodedLength` walk the bytes with bounds checks
  (including ENUM range, UTF-8 and canonical booleans), and anything from
  outside goes through them first.
- **Stability.** DuckDB does not promise a stable sort-key format across
  versions. The database records a format version in `meta`, and all
  encoding goes through `ValueCodec`. If upstream changes the encoding, we
  vendor the old encoder behind the recorded version.

`REF` values are encoded as the 6-byte packed ID, not as a generic integer
sort key, so VAET and E positions share an encoding.

### Key splitting

Every index key has at most one variable-length segment (V), and the other
segments have fixed widths. So V is found by position (for example
`key[8 : len-5]` for EAVT) and never needs to be decoded just to split a key.
Key encode/decode lives in `src/codec/keys.cpp`.

## 4. RocksDB layout

One column family per index:

| CF            | Key                         | Value              | Contains            |
|---------------|-----------------------------|--------------------|---------------------|
| `eavt`        | `E6 A2 V T5`                | empty              | current datoms      |
| `eavt_hist`   | `E6 A2 V T5`                | `Tretract5`        | retracted datoms    |
| `aevt`        | `A2 E6 V T5`                | empty              |                     |
| `aevt_hist`   | `A2 E6 V T5`                | `Tretract5`        |                     |
| `avet`        | `A2 V E6 T5`                | empty              | indexed / unique attrs only |
| `avet_hist`   | `A2 V E6 T5`                | `Tretract5`        |                     |
| `vaet`        | `V6 A2 E6 T5`               | empty              | REF attrs only      |
| `vaet_hist`   | `V6 A2 E6 T5`               | `Tretract5`        |                     |
| `txlog`       | `T5 E6 A2 V`                | op byte (assert/retract) | every change  |
| `meta`        | named keys                  | varies             | format version, counters, last T |

- `T` in an index key is the **asserting** transaction.
- **Retraction** (overwrite, set to NULL, DELETE, set element removed):
  delete the current key and put the same key into the `_hist` CF with the
  retracting T as value. This happens in every index the attribute
  participates in.
- **Redundant assertions** (the value is already current) are skipped.
- Separate CFs allow per-index bloom filters, prefix extractors and
  compaction settings.
- Excision and pruning later on are plain deletes of `_hist` and `txlog`
  ranges.

## 5. Reading

### As-of rule

For a basis `t`, a datom is visible iff:

```
(current  and  Tassert <= t)
or
(history  and  Tassert <= t < Tretract)
```

With no `AT` clause, `t` is the transaction's snapshot basis and only current
CFs are read (`Tassert <= t` still applies, for snapshot consistency).

History CFs are sorted by assertion T, so an as-of scan reads the whole
history range for the attribute and filters on the value. That's acceptable
to start with.

### `AT` clause

- `AT (VERSION => n)`: `n` is the tx local number, or the full `id` of a
  `sys.tx` row. A version later than the reading transaction's basis is an
  error.
- `AT (TIMESTAMP => ts)`: the latest tx whose `sys.tx.instant <= ts`. Tx
  instants are monotonic, so this is a seek on `sys.tx/instant` (always
  AVET-indexed).

The basis is resolved when the catalog binds the table, so different tables
in one query can be read at different points in time.

### Table scan

`SELECT c1, c2 FROM t` runs a k-way merge over the AEVT ranges of **all**
attributes of `t`, keyed on E (all of them, because membership is "any
attribute"). Only projected columns are decoded. Cardinality-many columns
collect consecutive V's for the same E into a LIST. Output is sorted by E.

Filter pushdown: the scan accepts every filter DuckDB pushes and applies it
exactly to each row, in batches. Two kinds of filter also choose the
driving mode:
- equality or IN on `id`, which becomes a set of candidate entities;
- comparisons, BETWEEN, IN, AND (ranges intersected) or OR on an
  AVET-indexed cardinality-one column, which becomes encoded value ranges,
  then candidate entities from AVET (plus `avet_hist` under `AT`).

Candidates are filled in by EAVT point lookups. Every other query merges the
AEVT ranges. (Declining filters through `supports_pushdown_type` breaks
DuckDB's plan when the filtered column isn't projected, so the scan accepts
them all.)

Joins between tables are ordinary DuckDB joins for now. Because every scan
comes out sorted by E, a later optimizer pass can fuse scans of the same
database into a single merge join.

## 6. Writing and transactions

### Lifecycle

1. **Begin.** Take a RocksDB snapshot, record basis `t0` = last committed T.
2. **First write.** Acquire the process-wide writer mutex. A second writer
   waits, and the wait can be interrupted. Then:
   - If the transaction hasn't read any data yet (an autocommit
     `INSERT ... VALUES`, or a statement whose scans all start after this
     point) and the schema is unchanged, it moves its snapshot to the latest
     commit.
   - Otherwise, if a transaction committed after `t0`, it fails with a
     conflict error. The caller must retry; the system never re-runs a
     transaction on its own.

   Basis, snapshot and catalog generation are taken under the same lock that
   commit holds while publishing, so they always belong together. Allocate this transaction's ID in the `tx`
   partition; `current_tx()` returns it from here on.
3. **Statements.** Writes go into a `WriteBatchWithIndex`. Reads in the same
   transaction iterate the batch over the snapshot (read-your-writes).
4. **Commit.** Add the tx entity's datoms (`sys.tx.instant`, plus any user
   columns written to `sys.tx`) and the `txlog` entries, update counters and
   last T in `meta`, write the batch atomically with WAL sync, release the
   mutex.
5. **Rollback.** Drop the batch and release the mutex. The tx ID and any
   allocated entity IDs are wasted.

A transaction whose writes cancel out (assert then retract, retract then
re-assert, DELETE then re-INSERT of the same values) changes no datom. It
commits nothing and gets no `sys.tx` row. "Changed" means at least one
`txlog` entry for the transaction remains in the batch.

### Tempids

- `tempid('label')` returns a placeholder in partition `temp`. Placeholders
  are deterministic: the middle 16 bits (unused by real IDs) hold the target
  partition, and the low 40 bits a hash of the label. So the function is
  pure, and the transaction maps placeholders to real IDs as rows are
  written.
  `tempid('label', p)` with `p` in 16–254 allocates the real ID in user
  partition `p`. `tempid()` is the only way to mint an ID in a chosen
  partition.
- Placeholders are resolved by the insert operator when a row is written, in
  `id` and in REF columns:
  - already-resolved label → that ID;
  - row matches an existing entity on a unique-identity column → that
    entity;
  - otherwise → a fresh ID in the requested partition (default `db`).
- Statement order matters: insert the row carrying the unique key before
  rows that only reference the label. If a label resolves fresh and is later
  matched to a different existing entity by unique identity, that's an
  error.
- Omitting `id` on INSERT allocates a fresh ID.

### Statement semantics

- **Statements don't see their own writes.** INSERT/UPDATE/DELETE sinks
  buffer their input and apply it in Finalize, after the statement's scans
  are done (this is the Halloween problem). Writers within one transaction
  are serialized by a per-transaction mutex, because the actions of a MERGE
  run as separate pipelines.
- **Uniqueness is checked per statement.** Unique values asserted by a
  statement are verified when it finishes, so a statement may move or swap
  unique values between entities.
- **Explicit ids must exist.** An explicit `id`, a REF value, or the entity
  given to `mnemonicdb_add` must name an allocated entity. New entities come
  from `tempid()` or an omitted `id`. Explicit ids therefore never collide
  with later allocations and never move the counters. Partitions 4–15 and
  255 are rejected.

- **ON CONFLICT** is rewritten by DuckDB into MERGE INTO, which plans
  through our INSERT/UPDATE/DELETE operators. It finds conflict targets
  through the table's storage info (unique and identity columns).
- **INSERT** asserts every non-NULL column for the row's entity. Inserting
  with the `id` of an existing entity is legal: that's how an entity gets
  attributes from another table. Cardinality-one columns retract any
  different current value. A unique-value conflict is an error. A
  unique-identity conflict is an error unless `ON CONFLICT (col) DO UPDATE`,
  in which case it upserts into the existing entity.
- **UPDATE** compares old and new values. Cardinality-many columns compare
  as sets, so a reordered or duplicated list equal to the current set is a
  no-op, and `[]` retracts every element (reads back as NULL). Cardinality-one: retract and
  assert, or skip if unchanged. Cardinality-many: retract the removed
  elements and assert the added ones, so `SET tags = list_append(tags, 'x')`
  works.
- **DELETE** retracts every attribute of that table from the matched
  entities. They leave this table but remain in any others.
- **Single-datom add / retract.** Blind assertion or retraction of one
  datom, mainly for cardinality-many elements, without reading the current
  set: `CALL mnemonicdb_add('d.person', id, 'tags', 'x')` and
  `CALL mnemonicdb_retract('d.person', id, 'tags', 'x')`. Same redundancy and
  uniqueness rules as INSERT/UPDATE. They return whether anything changed.

## 7. Schema: the `sys` schema

The schema is data, stored as entities with the same indexes and history as
everything else. Each attached database has a `sys` schema:

| Table            | Columns (besides `id`)                                         |
|------------------|----------------------------------------------------------------|
| `sys.tables`     | `schema`, `name`, `doc`                                        |
| `sys.attributes` | `table_id REF`, `name`, `type` (DuckDB type string; a top-level LIST means cardinality-many), `uniqueness` (`value`/`identity`, NULL for none), `indexed`, `index` (index name), `doc` |
| `sys.tx`         | `instant TIMESTAMPTZ`; users may `ALTER TABLE sys.tx ADD COLUMN` |

(`table` and `unique` are reserved words in DuckDB, hence `table_id` and
`uniqueness`.)

- **Bootstrap.** The `sys` tables and their attributes have fixed IDs.
  Table locals 1–15 and attribute locals 1–63 are reserved for built-ins, so
  user tables start at local 16 and user attributes at 64. Transaction 1 of
  a new database writes the datoms describing them, so `sys.tables` and
  `sys.attributes` describe themselves.
- **Two front doors.** `CREATE TABLE` / `ALTER TABLE` / `CREATE INDEX`
  translate into writes against `sys.*`, and INSERT/UPDATE on `sys.tables`
  and `sys.attributes` is equally valid. After each such statement the
  transaction re-reads the schema from its own view and reconciles it with
  the previous one:
  - AVET entries are built or dropped when an index or uniqueness changes;
  - making a column unique checks the existing values;
  - type changes, attribute removal and DELETE are refused, and must go
    through ALTER TABLE / DROP.

  The new catalog generation is visible to the transaction immediately and
  to everyone else on commit. Built-in entities can't be changed.
- **Transaction metadata.** `ALTER TABLE sys.tx ADD COLUMN note VARCHAR`,
  then `INSERT INTO sys.tx (id, note) VALUES (current_tx('db'), '...')`
  inside the writing transaction. `current_tx('db')` is volatile and only
  valid once the transaction has written. Rows for other transactions can't
  be written.
- **History.** Schema changes can be queried historically, but data is
  always read through the **current** schema. An `AT` query before a type
  change sees old data in the new type.
- **ALTER TABLE**
  - `ADD COLUMN` adds an attribute.
  - `RENAME COLUMN` and `RENAME TABLE` change only the name datom, so the
    data and history keep their entity IDs.
  - `DROP COLUMN` retracts the column's values (they stay in history) and
    detaches the attribute. Adding a column with the same name later creates
    a new, empty attribute.
  - `ALTER COLUMN ... TYPE` is a bulk rewrite: every datom of the attribute
    is re-encoded in all indexes, history and `txlog` included. Allowed:
    DuckDB implicit (widening) casts, any type to VARCHAR, and the same rules
    applied to list elements. Not allowed: cardinality changes, REF changes
    and `USING`. The rewrite also checks that no two current values merge and
    that unique values stay unique.
- **`CREATE INDEX name ON t (col)` / `DROP INDEX name`** build or drop AVET
  and `avet_hist` entries for the attribute, history included. The index
  name is stored on the attribute entity (`sys.attributes.index`), and
  `duckdb_indexes()` lists them. Unique attributes always have AVET, with or
  without an index. Indexes cover exactly one column and take no options.

## 8. Code layout (planned)

```
CMakeLists.txt            extension build (DuckDB extension template)
duckdb/                   submodule, pinned commit on main
third_party/rocksdb/      submodule (or vcpkg)
src/
  datom_extension.cpp     registration: storage type, REF type, tempid(), current_tx()
  codec/                  EntityId packing, ValueCodec (sort keys), key builders
  store/                  RocksDB open/CFs, meta, counters, as-of iterators
  catalog/                Catalog / Schema / TableEntry, sys bootstrap, AT binding
  txn/                    TransactionManager, writer mutex, WBWI, tempid resolution
  exec/                   scan (k-way merge), insert / update / delete sinks
test/sql/                 sqllogictest files
test/unit/                codec round-trip tests
```

## 9. Milestones

1. **Skeleton.** Builds against pinned DuckDB `main`; `ATTACH ... (TYPE mnemonicdb)`
   opens RocksDB with all CFs and `meta`.
2. **Codec.** ID packing; sort-key encode/decode round trip and ordering
   tests for every supported type; key builders for all nine indexes.
3. **Schema.** `sys` bootstrap; `CREATE TABLE` → `sys` datoms; catalog
   loaded from `sys`.
4. **Write + scan.** INSERT with tempids across two tables in one
   transaction; tx entity and `txlog`; table scan with k-way merge.
5. **History.** UPDATE / DELETE, current→history moves, redundant-write
   elimination, cardinality-many.
6. **Time travel.** `AT (VERSION => ...)` and `AT (TIMESTAMP => ...)`.
7. **Constraints and indexes.** `REF` + VAET, `UNIQUE` / `PRIMARY KEY`,
   upsert, `CREATE INDEX`, AVET filter pushdown, single-datom add/retract.
8. **Schema evolution.** `ALTER COLUMN TYPE` and index rewrites.
9. **Later.** Scan-fusion optimizer pass, history table function, excision.

Milestones 1–6 together show the riskiest parts working end to end.

## 10. Open questions

None outstanding. Settled along the way:
- The add/retract syntax is the `mnemonicdb_add` / `mnemonicdb_retract`
  table functions.
- Net-empty transactions commit nothing.
- `[]` is the empty set.
