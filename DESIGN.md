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
| namespace          | table; tables are grouped in schemas (`d.core.files`)      |
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
| `schema`  | 4     | schemas and named types (DDL only) | full packed (6) |
| reserved  | 5–15  | future system use     |                     |
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
    change. Appending values to a named ENUM keeps every index valid and is
    supported (§7b); anything else means a rewrite.
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

### Threads and ATTACH options

RocksDB runs flushes and compactions in the thread pools of its default
`Env`. Those pools are **process-wide**: all RocksDB databases in the process
share them, including every attached MnemonicDB database. MnemonicDB starts
no threads of its own; scans run on DuckDB's scheduler.

With the defaults, the process has three persistent RocksDB threads however
many databases are attached: one compaction thread (`rocksdb:low`), one flush
thread (`rocksdb:high`) and RocksDB's periodic-task timer. Opening a database
also uses up to `max_file_opening_threads` (16) short-lived threads to load
SST files. Most of an idle DuckDB process's threads are DuckDB's own: its
worker threads (`threads` minus one) and its I/O threads (`async_threads`,
64 by default).

These ATTACH options cap the RocksDB threads. Each takes an integer and keeps
RocksDB's default when omitted:

| Option | RocksDB setting | Default | Range |
|---|---|---|---|
| `MAX_BACKGROUND_JOBS` | `DBOptions::max_background_jobs`: concurrent flushes + compactions of this database | 2 | 1–256 |
| `MAX_FILE_OPENING_THREADS` | `DBOptions::max_file_opening_threads`: threads that open SST files during ATTACH | 16 | 1–256 |
| `NUM_LOW_PRI_THREADS` | `Env::SetBackgroundThreads(n, LOW)`: the compaction pool | grown to what `MAX_BACKGROUND_JOBS` needs | 1–256 |
| `NUM_HIGH_PRI_THREADS` | `Env::SetBackgroundThreads(n, HIGH)`: the flush pool. With 0, flushes run in the compaction pool | grown to what `MAX_BACKGROUND_JOBS` needs | 0–256 |

```sql
ATTACH 'app.mnemonic' AS d (TYPE mnemonicdb, MAX_BACKGROUND_JOBS 1,
    MAX_FILE_OPENING_THREADS 1, NUM_LOW_PRI_THREADS 1, NUM_HIGH_PRI_THREADS 0);
```

- The two pool sizes apply to the whole process. They're set as the last
  step of a successful ATTACH, so a failed ATTACH leaves them unchanged. The
  most recent ATTACH that sets one wins.
- An ATTACH that doesn't set them only grows the pools to what its own
  `MAX_BACKGROUND_JOBS` needs (one flush and one compaction thread with the
  default). So a later default ATTACH undoes `NUM_HIGH_PRI_THREADS 0`.
- DETACH doesn't restore them.
- `mnemonicdb_info('db')` reports the current sizes in `low_pri_threads` and
  `high_pri_threads`. They're the same for every database.
- The timer thread can't be turned off.
- The upper bound is 256. Every thread counts against the process's thread
  limit (about 2,184 in the Mauvi container), and the pools are shared, so
  one oversized value would affect every database in the process.
- Unknown options, non-integers (including fractions) and out-of-range values
  are rejected before the database is opened. An integer too large for
  BIGINT gets the range error.
- **DuckDB's own storage options are rejected too.** Before this change,
  `BLOCK_SIZE`, `STORAGE_VERSION`, `ENCRYPTION_KEY` and other options DuckDB
  passes through to a storage extension were silently ignored. Now they fail
  with "unrecognized ATTACH option". DuckDB itself handles `TYPE`,
  `READ_ONLY` / `READ_WRITE`, `DEFAULT_TABLE`, `HIDDEN`, `RECOVERY_MODE` and
  `VACUUM_REBUILD_INDEXES`, so they never reach MnemonicDB.

Measured on a 16-vCPU VM, DuckDB started with `threads = 1` and
`async_threads = 1`:

| | Idle process | After ATTACH + write | Short-lived threads during ATTACH |
|---|---|---|---|
| Defaults | 2 | 5 | 15 |
| Minimum caps above | 2 | 4 | 0 |

With DuckDB's defaults on the same VM (`threads = 16`, `async_threads = 64`)
the idle process has 80 threads, and 82 to 83 after an ATTACH.

The flush thread exits asynchronously, within a few seconds of the ATTACH that
sets `NUM_HIGH_PRI_THREADS 0`.

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

Filter pushdown: the scan accepts every filter DuckDB pushes and applies every
one that is not optional exactly to each row, in batches. Filters of every
kind also say which entities can qualify: exact filters, and the optional and
dynamic ones through which a join hands its key set to the probe-side scan at
runtime (an `IN` list of up to `dynamic_or_filter_threshold` keys, min/max
bounds, or a key bitmap). An optional filter is only a hint, but a row it
rejects is one the join drops too, so it may narrow what the scan reads. Each
filter is reduced to a key set for its column (points, bounds and key bitmaps,
intersected through AND and optional wrappers, united through OR of points),
which chooses the driving mode:
- points on `id` (equality, `IN`, a join's key list, or a key bitmap
  enumerated by bisection over the table's entity range) become candidate
  entities;
- otherwise points or ranges on an AVET-indexed cardinality-one column become
  encoded value ranges, then candidate entities from AVET (plus `avet_hist`
  under `AT`); points on a REF column without AVET become VAET lookups (plus
  `vaet_hist`).

Candidates are filled in by EAVT point lookups (plus `eavt_hist` under `AT`).
Candidates from hints are used only while there are at most
max(1024, rows / 64) of them; past that a merge is cheaper. Every other query
merges the AEVT ranges, bounded by any range on `id` (a join's min/max). A key
set that is empty (contradictory filters, only NULLs, bounds that cross) reads
nothing.

A join whose build side is empty pushes no filter at all. The optimizer pass
therefore marks scans that an equality hash join feeds in the same pipeline
(only projections and filters in between; INNER, SEMI, RIGHT or RIGHT_SEMI;
key and column of the same type; not in a recursive CTE). Such a join pushes a
filter whenever its build side has a non-NULL key, so a marked scan that
receives none reads nothing. DuckDB normally skips the probe pipeline itself
in that case, but not always (for example inside a streamed CTE).

**Reading fewer attributes.** Membership normally needs every attribute of a
table. But when a pushed filter on an attribute is NULL-rejecting
(comparisons, BETWEEN, IN, IS NOT NULL, AND with any such child, OR of only
such children), rows without that attribute can't pass. That attribute then
decides membership, and the scan reads only the filtered and projected
attributes. Values are decoded straight from the iterator's key into the
output vectors, including list children.

**Parallelism.** A scan splits into tasks:
- merge mode: even slices of the entity id range, which is found from the
  attributes' first and last AEVT keys;
- lookup mode: slices of the sorted candidates.

Scans whose largest table is under about 100k rows stay single-threaded. (Declining filters through `supports_pushdown_type` breaks
DuckDB's plan when the filtered column isn't projected, so the scan accepts
them all.)

### Scan fusion

An optimizer pass (`src/optimizer/mnemonic_fusion.cpp`) runs after
DuckDB's own optimizers and fuses joins on entity id into a single scan.

- **What qualifies:** an INNER, LEFT or RIGHT comparison join whose only
  condition is `id = id` (or `rowid`) between two MnemonicDB scans of the
  same database, read at the same `AT` version.
- **The fused scan** reads every attribute of every table in one AEVT merge,
  in entity order, so no hash join is needed. An entity is produced when it
  is a member of every required table. A table on the optional side of an
  outer join is optional, and its columns (including `id`) are NULL when the
  entity isn't a member.
- **Bindings:** the fused scan's `projection_ids` reproduce the join's
  output exactly, position for position, because parent joins' projection
  maps refer to positions. Column bindings above the join are rewritten.
  Joins fuse one at a time, bottom-up, so chains of three or more tables
  collapse into one scan.
- **Filters** from both sides move into the fused scan, so an indexed
  filter on either table still drives it from AVET. Key sets that parent
  joins push at runtime into either side are redirected to the fused scan,
  shifted to its column positions.
- **Not fused:** plans containing DML (UPDATE ... FROM needs a single
  table's row ids), joins on other columns or with extra conditions, joins
  across different `AT` versions, and joins with non-MnemonicDB tables.
- **Turning it off:** `SET mnemonicdb_fuse_scans = false` (used by the
  tests to compare against unfused plans).

A further step would seek the cursors of required tables forward
(leapfrog) instead of reading every attribute in full.

### Raw history

`mnemonicdb_history('db', schema_name :=, table_name :=, column_name :=, entity :=, since :=, until :=)`
returns one row per assertion or retraction, in transaction order:

| column | type |
|---|---|
| `tx` | `UBIGINT` (transaction number) |
| `entity` | `REF` |
| `schema_name`, `table_name`, `column_name` | `VARCHAR` |
| `attribute` | `REF` (the attribute entity) |
| `value` | `VARIANT` (any attribute type, nested types included) |
| `added` | `BOOLEAN` (true = asserted, false = retracted) |

- **Filters:** `table_name` is `table` (in `main`) or `schema.table`;
  `schema_name` alone restricts to every table of that schema, and with a bare
  `table_name` names the table's schema. `mnemonicdb_tx_changes` takes the
  same `schema_name` / `table_name` / `column_name` filters.
- **Sources:**
  - whole database, `schema_name` or `table_name` alone: the `txlog`,
    streamed and seeked to `since`;
  - `column_name`: AEVT plus `aevt_hist`;
  - `entity`: EAVT plus `eavt_hist`.

  Assertions come from index keys, and retractions from history values.
- **Dropped columns** keep their history. They're decoded from their own
  definition datoms, attributed to the table they were on (found through
  `sys.attributes.table_id` in history), and included in that table's
  history.
- **Visibility:** the reading transaction's pending writes are included.
- Parameters are named `table_name` / `column_name` because `table` and
  `column` are reserved words.

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
4. **Commit.** Evaluate commit assertions (§7a). Add the tx entity's datoms (`sys.tx.instant`, plus any user
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
- **Statements are atomic.** Each statement runs under a write-batch
  savepoint. If it fails, its writes, tempid bindings, ID allocations and
  schema changes are undone. A transaction that rolls back gives its IDs
  back, so neither failures nor rollbacks use up table or attribute
  numbers.
- **Rows without values** assert nothing, create no entity, and are not
  counted as inserted.
- **Uniqueness is checked per statement.** Unique values asserted by a
  statement are verified when it finishes, so a statement may move or swap
  unique values between entities.
- **Explicit ids must exist.** An explicit `id`, a REF value, or the entity
  given to `mnemonicdb_add` must name an allocated entity. New entities come
  from `tempid()` or an omitted `id`. Explicit ids therefore never collide
  with later allocations and never move the counters. Partitions 4–15
  (including partition 4, schema objects) and 255 are rejected, in INSERT,
  REF values and `mnemonicdb_add` alike.

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
- **Side-effecting functions respect read-only transactions.**
  `mnemonicdb_add` and `mnemonicdb_retract` declare their write while binding
  (`RegisterDBModify`, like INSERT). DuckDB then calls
  `MetaTransaction::ModifyDatabase` before the statement runs. In a
  `BEGIN TRANSACTION READ ONLY` they fail with DuckDB's
  `Cannot write to database "d" - transaction is launched in read-only mode`.
  On a `READ_ONLY` attach they fail with
  `Cannot execute statement of type "CALL" on database "d" which is attached
  in read-only mode!`. Either way they never take the writer lock. This holds
  for `CALL` and for a table function in a `SELECT`.
  - Binding no longer takes the writer lock. `EXPLAIN` / `DESCRIBE` of a datom
    call, or a statement that fails before running, leaves other writers
    alone.
  - DuckDB's one-database-per-transaction rule applies. A transaction that
    has written to another attached database can't call a datom function on
    this one, and the reverse holds too.
  - A datom call is a write like INSERT: one in a transaction that read data
    before another transaction committed gets a conflict error.
  - **Table macros:** DuckDB binds a table macro's body when the macro is
    created. `CREATE MACRO m() AS TABLE SELECT * FROM mnemonicdb_add(...)`
    therefore counts as a write to that database: it's refused in a read-only
    transaction and takes the writer lock until the creating transaction ends.
    Calling the macro is checked like a direct call.
  - `mnemonicdb_compact('db')` changes no datom and doesn't take the writer
    lock, so writers continue while it runs. Because it's expensive, it fails
    in a read-only transaction and on a read-only attach. It uses the wording
    of the same two DuckDB errors, with `"mnemonicdb_compact"` as the
    statement type. These checks run when the function executes, so they
    also apply to a statement prepared earlier.
  - The other functions (`mnemonicdb_info`, `mnemonicdb_history`,
    `mnemonicdb_tx_changes`, `current_tx`, `tempid` and the codec functions)
    only read.

## 7. Schema: the `sys` schema

The schema is data, stored as entities with the same indexes and history as
everything else. Each attached database has a `sys` schema:

| Table            | Columns (besides `id`)                                         |
|------------------|----------------------------------------------------------------|
| `sys.tables`     | `schema`, `name`, `doc`                                        |
| `sys.attributes` | `table_id REF`, `name`, `type` (DuckDB type string; a top-level LIST means cardinality-many), `uniqueness` (`value`/`identity`, NULL for none), `indexed`, `index` (index name), `doc` |
| `sys.tx`         | `instant TIMESTAMPTZ`, `reencoded REF[]` (attributes whose stored values the transaction re-encoded, §7b); users may `ALTER TABLE sys.tx ADD COLUMN` |
| `sys.assertions` | `name` (unique identity), `query`, `doc`: commit assertions, see §7a |
| `sys.schemas`    | `name` (unique identity), `doc`: see §7b                       |
| `sys.types`      | `schema`, `name`, `definition` (DuckDB type string), `doc`: named types, see §7b |

(`table` and `unique` are reserved words in DuckDB, hence `table_id` and
`uniqueness`.)

- **Bootstrap.** The `sys` tables and their attributes have fixed IDs.
  Table locals 1–15, attribute locals 1–63 and schema-partition locals 1–15
  are reserved for built-ins, so user tables start at local 16, user
  attributes at 64 and user schemas and types at 16. Built-in tables:
  `tables` 1, `attributes` 2, `tx` 3, `assertions` 4, `schemas` 5, `types` 6.
  Built-in schemas: `main` (schema local 1) and `sys` (2). Transaction 1 of
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
    through ALTER TABLE / DROP;
  - a table's `schema` must name an existing schema other than `sys`.

  `sys.schemas` and `sys.types` have only the DDL door: INSERT, UPDATE and
  DELETE on them are refused.

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

## 7a. Commit assertions

An **assertion** is a named SQL query that must return no rows. Every
assertion is evaluated when a write transaction commits, against the
transaction's pending state. If any assertion returns rows, or cannot run,
the commit fails and nothing is committed.

```sql
INSERT INTO d.sys.assertions (name, query, doc) VALUES
    ('non_negative_age', 'SELECT name, age FROM person WHERE age < 0', 'Ages are never negative');
```

- **Storage.** `sys.assertions` (table local 4; attributes 12–14) is schema
  as data, but its rows are ordinary entities in partition `db`, not schema
  entities. INSERT, UPDATE, DELETE and `ON CONFLICT (name)` all work, and do
  not reload the schema. History is visible through `AT` and
  `mnemonicdb_history('d', table_name := 'sys.assertions')`.
  `mnemonicdb_add`/`mnemonicdb_retract` still refuse sys tables.
- **Old databases.** Built-in tables are never loaded from data, so a
  database created before sys.assertions existed gets the table from code.
  Its descriptive datoms (`sys.tables` / `sys.attributes` rows) are written
  by the first read-write attach of a newer version, see "Upgrading" in §7b.
- **Scope.** Assertions run only when the committing transaction changed at
  least one datom (the net-empty check in §6). Read-only transactions and
  transactions whose writes cancel out evaluate none. A transaction that adds,
  changes or removes an assertion is checked against the assertions as that
  transaction leaves them: a new or changed assertion checks the commit that
  writes it, and a removed one does not.
- **Visibility.** An assertion sees exactly what the committing transaction
  sees: its snapshot, its pending writes and its catalog generation (tables
  it created included). Unqualified names resolve to the database being
  committed, whatever it is attached as.
- **Incremental checks.** `mnemonicdb_tx_changes([db], table_name :=,
  column_name :=)` returns the reading transaction's pending datoms with the
  same columns as `mnemonicdb_history`: one row per assertion or retraction,
  read from the txlog entries of the transaction's own tx. Outside a writing
  transaction it returns nothing. Without a database name it reads the
  default database. Writes that cancel out leave no row. A ninth column,
  `minted` (BOOLEAN, only in this function), is true when the entity was
  minted by the transaction: its id was allocated after the transaction took
  the writer lock (a tempid, a new table or attribute), or it is the
  transaction's own tx entity. Such an entity had no state before, so checks
  of what it used to be (children it had, a value it replaced) can skip it. Inside an
  assertion it returns the committing transaction's changes, so a check can
  start from what changed, e.g. walk `parent` up only from entities whose
  `parent` was asserted.
- **Rules for the query.** It must parse to exactly one SELECT. It runs in a
  `READ ONLY` transaction and cannot write to any database. Its rows are
  streamed and counted, and only the first 5 are rendered. Assertions are
  independent and callers must not depend on their order (they currently run
  ordered by name).
- **Failure.** All failing assertions are reported in one error, joined by
  `"; "`:
  ```
  assertion "non_negative_age" failed (2 rows): (Dave, -5), (Eve, -6)
  assertion "broken" could not run: Catalog Error: ...
  assertion "broken" is invalid: its query must be a single SELECT statement
  assertion "#<entity>" is invalid: it has no name
  ```
  The evaluator throws a `ConstraintException`, but DuckDB wraps every error
  raised while committing, so the client sees `TransactionContext Error:
  Failed to commit: assertion "..." failed ...`. The `assertion "<name>"`
  text is the stable, parseable part. A failed commit behaves like a
  rollback: the batch is dropped, allocated IDs are given back and the writer
  lock is released.
- **Mechanism.** DuckDB has no pre-commit hook, and the committing
  `ClientContext` is locked for the whole commit, so no SQL can run on it.
  `MnemonicTransaction::Commit(context)` therefore evaluates assertions after
  the final statement check and the net-empty check, and before the tx entity
  is written:
  1. Load the assertions from the transaction's own view (two AEVT scans; this
     is the only cost when there are none).
  2. Open a fresh `Connection` on the same `DatabaseInstance` and register its
     context on the `MnemonicCatalog` as an assertion context whose parent is
     the committing transaction.
  3. `MnemonicTransactionManager::StartTransaction` sees the registration and
     builds a **borrowed** transaction: the parent's basis, snapshot (not
     owned, never released) and catalog generation, and a read view overlaying
     the parent's write batch. `SetReadWrite` throws, so it never takes the
     writer lock and cannot deadlock against its parent.
  4. `USE <this database>`, then for each assertion `BEGIN TRANSACTION READ
     ONLY`, the query, `ROLLBACK`.
  5. Unregister; throw if anything failed.

  Other connections are unaffected: the registry is keyed by
  `ClientContext`. The assertion connection uses default settings, not the
  committing session's.
- **Estimates.** `mnemonicdb_tx_changes` reports its row count to the planner
  (its pending txlog entries for the filtered attributes, counted up to
  20,000), so a join builds its hash table on the changes rather than on a
  large table. `mnemonicdb_history` estimates twice the current datoms of the
  attributes it reads. Table scans report distinct counts for `id` and for
  UNIQUE / PRIMARY KEY columns (never min/max, which DuckDB would trust for
  pruning).
- **Cost.** One connection per checked commit plus three statements per
  assertion. In a reldebug build, an autocommit single-row INSERT took about
  0.5 ms with no assertions and about 1.4 ms more per simple
  `mnemonicdb_tx_changes`-driven assertion.

## 7b. User schemas and named types

### Schemas

```sql
CREATE SCHEMA d.core;
CREATE TABLE d.core.files (path VARCHAR PRIMARY KEY, layer d.core.layer);
SELECT * FROM d.core.files AT (VERSION => 12);
USE d.core;  -- unqualified names now resolve in core
```

- **Storage.** A schema is an entity in partition `schema` with a
  `sys.schemas.name` datom (unique identity) and optionally `doc`. `main`
  and `sys` are built in. CREATE SCHEMA writes the datoms in the creating
  transaction, so schemas have history (`AT`, `mnemonicdb_history('d',
  table_name := 'sys.schemas')`) like tables.
- **Upgrading.** A database written by an older version (e.g. before
  sys.schemas and sys.types) gets the missing built-in tables from code, so a
  read-only attach works, but its `sys.schemas` has no rows. The first
  read-write attach writes every missing built-in datom (the `main`/`sys`
  rows, the `sys.tables` rows of the new tables, their attributes and
  `sys.tx.reencoded`) in one transaction. It re-asserts the whole bootstrap
  description, and since asserting a current datom is a no-op only the
  missing ones are written; if none are missing no transaction is written.
  Before writing, it checks that the reserved built-in schema entities
  (partition 4, locals 1–2) hold nothing else, and refuses to upgrade if they
  do. `test/data/legacy_f456677` and `test/data/legacy_tx_column` are
  databases written by f456677, used by `test/sql/upgrade.test`.
- **New built-in columns on user-extensible sys tables.** Users may add
  columns to `sys.tx`, so a built-in column added by a newer version (such as
  `sys.tx.reencoded`) can collide with a user column of an older database.
  Policy: the user column keeps its name and the built-in is **shadowed**:
  still written and in history, but not a column of the table, until the user
  renames their column (`ALTER TABLE sys.tx RENAME COLUMN reencoded TO
  note`, allowed for user columns of `sys.tx`), which un-shadows it. A new
  user column can never take a built-in's name. Built-in attributes sort
  before user ones, so an upgrade that adds a built-in column to `sys.tx`
  inserts it before the user's columns and changes `SELECT *` column order.
- **Tables** keep referring to their schema by name (`sys.tables.schema`).
  A table's identity is its entity ID and its attributes are keyed by their
  own IDs, so the same table name in two schemas never collides: the codec
  and every index only see attribute locals. Index names are per schema.
- **Every path** takes four-part names: DML, `ON CONFLICT`, scans and fused
  joins, `AT`, `CREATE INDEX` / `DROP INDEX`, ALTER TABLE,
  `mnemonicdb_add('d.core.files', ...)`, history and commit assertions
  (`SELECT ... FROM core.files` resolves in the committing database).
- **Reserved names.** `sys` cannot be created, dropped, or hold user tables
  or types. `main` cannot be dropped. DuckDB's default schema names
  (`information_schema`, `pg_catalog`) and `temp` are refused. Nested schemas
  are not supported.
- **DROP SCHEMA** drops only an **empty** schema (no tables, no types); it
  retracts the schema's datoms, so it stays in history and the name can be
  created again as a new entity. `CASCADE` is refused, and since tables
  cannot be dropped yet, a schema that ever held a table cannot be dropped.
- **Catalog.** The `Schema` snapshot holds the schemas and types; each catalog
  generation builds table and type entries for every schema in it. Schema
  catalog entries carry only a name and read everything through the
  transaction's generation, so the catalog keeps one entry per exact name,
  created on first use and never freed (DuckDB can hold an entry across a
  schema change). Lookups are case-insensitive; a schema dropped and created
  again in different case gets an entry spelled the new way. A schema created
  in a transaction is visible only to it until commit.
- **Cost of DDL.** Every DDL statement builds a new catalog generation: new
  table, index and type entries for the whole database (the `Schema`
  snapshot is copied, and an ENUM append re-reads it from the store). Many
  DDL statements in one transaction therefore cost O(statements × schema
  size); fine for migrations of hundreds of tables, not for thousands of
  statements.

### Named types

```sql
CREATE TYPE d.core.layer AS ENUM ('system', 'game', 'dlc', 'mods', 'overrides', 'generated');
CREATE TYPE d.core.cond AS STRUCT(layer d.core.layer, priority INTEGER);
CREATE TABLE d.mods.plugins (name VARCHAR, layer d.core.layer, conds d.core.cond[]);
```

- **Storage.** A named type is an entity in partition `schema` with
  `sys.types.schema`, `name` and `definition`: the DuckDB type string, in
  which other named types appear by qualified name (`STRUCT(layer
  core.layer, priority INTEGER)`). Any type the codec supports can be named
  (ENUM, STRUCT, UNION, LIST, MAP, ...), except REF. A type defined as another
  named type (`CREATE TYPE d.core.b AS d.core.layer`) copies its
  definition: `b` is a separate type that does not follow later appends to
  `layer`.
- **Columns** record the named type by reference: `sys.attributes.type` is
  `core.layer`, `core.cond[]`, `MAP(VARCHAR, core.layer)` and so on. On load,
  types are resolved in creation order, then column types; nested types that
  contain named types are rebuilt by MnemonicDB, everything else binds
  through DuckDB. A schema-qualified name in a stored type string is resolved
  **only** among this database's named types, never through DuckDB's lookup
  (which could find another catalog's type of the same name); a missing one
  is an error. A column can use types of any schema of the same database. A
  named type of another MnemonicDB database is refused even when its name and
  definition match: our named types carry a tag of the database they belong
  to (a random token per open of the store), and the tag must be this
  database's. A DuckDB type from another
  catalog (e.g. `memory.main.colour`) has no identity here and is copied
  structurally.
- **What DuckDB sees.** Internally a named type carries the alias
  `schema.name`, which is how CREATE TABLE tells us which named type a column
  used. Column types exposed to DuckDB have those aliases stripped, exactly as
  DuckDB treats its own named types (`duckdb_columns()` shows `ENUM(...)`),
  because DuckDB refuses implicit casts between differently aliased types and
  generic functions (`struct_extract`, `union_tag`, `len`) would not bind.
  The caveat: an explicit cast `x::d.core.layer` in a query yields an aliased
  value, which compares with columns fine but cannot be implicitly cast, e.g.
  into a UNION member; use `union_value(layer := 'dlc')` or a plain literal.
- **Appending ENUM values** is the one supported change to a named type:

  ```sql
  CREATE OR REPLACE TYPE d.core.layer AS ENUM ('system', 'game', 'dlc', 'mods', 'overrides', 'generated', 'patches');
  ```

  DuckDB has no `ALTER TYPE ... ADD VALUE` at the pinned commit, so the
  migration is `CREATE OR REPLACE TYPE` under the rule that the old values
  must be a prefix of the new list, in order. Removing, reordering or
  renaming a value is an error naming the first offending position, and
  replacing a non-ENUM type, or with a non-ENUM, is refused. The same
  definition again is a no-op. Because old dictionary indexes stay valid, the
  migration writes one `sys.types.definition` datom and reloads the schema:
  every column and type using the ENUM, at any depth, picks up the new values
  by name. Stored values are re-encoded (as by ALTER COLUMN TYPE, history and
  txlog included) only where the ENUM's physical width changes, i.e. when it
  grows past 255 or 65,535 values. Every attribute ever declared with the
  type is re-encoded: live columns and **dropped** columns, whose values live
  on in history (found as attributes with a `sys.attributes.type` but no
  table link). Dropped columns are only read when the width changes (and by
  DROP TYPE), so an append that changes no width never touches them.
- **Past revisions decode with the current definition.** `AT`, history and
  `mnemonicdb_tx_changes` always decode values with the type as it is now
  (§7, "History"). For named types that is safe: an ENUM only ever grows, so
  an old dictionary index means the same value under the new definition, and
  a width change re-encodes every stored use. The price is that a width
  change rewrites the bytes of old transactions' `txlog` entries (and
  `_hist` entries): the datoms are the same, their encoding is not. The
  migrating transaction lists the attributes it re-encoded in
  `sys.tx.reencoded`, as does ALTER COLUMN TYPE.
- **DROP TYPE** drops a type that no column, no dropped column with history,
  and no other type uses; CASCADE is refused. Keeping the type while a
  dropped column's history needs it is simpler than keeping dropped
  definitions around for decoding, and keeps the rule "past revisions decode
  with the current definition". The definition stays in history.
- **COMMENT.** `CREATE OR REPLACE TYPE` writes the type's `doc` when the
  statement carries a comment (DuckDB's parser does not produce one for
  CREATE TYPE at the pinned commit, and `COMMENT ON TYPE` is not supported).
- `duckdb_types()` lists named types.

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

### Symbol hygiene

- File-local types go in an anonymous namespace. A struct or class that lives only in one `.cpp` must not sit
  directly in `namespace duckdb` (or `mnemonicdb`): its name, and the mangled names of every template instantiated
  on it (`std::vector<duckdb::ScanFilter>`, destructors, `_Destroy_range`), can collide with a same-named type in
  DuckDB. The linker (MSVC in particular) then keeps one COMDAT, and objects get destroyed by the wrong type's code.
- Compile definitions on vendored libraries (lz4, zstd, RocksDB) are `PRIVATE`, never `PUBLIC`, so they cannot leak
  into RocksDB or the extension through `target_link_libraries`.
- `scripts/check_duplicate_symbols.sh` (run by the `check_duplicate_symbols` target) fails on any strong symbol
  defined in two objects across the bundled archives.

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
9. **Done:** atomic statements, the scan-fusion optimizer pass, and the
   history table function. **Later:** excision, leapfrog seeks in fused
   scans, multi-threaded scans, join statistics.

Milestones 1–6 together show the riskiest parts working end to end.

## 10. Open questions

None outstanding. Settled along the way:
- The add/retract syntax is the `mnemonicdb_add` / `mnemonicdb_retract`
  table functions.
- Net-empty transactions commit nothing.
- `[]` is the empty set.
