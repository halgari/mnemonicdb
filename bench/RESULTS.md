# Performance

## Key sets: seeks from IN lists and join filters (MN-6, 2026-10-14)

`bench/keyset_bench.py --rows 1000000`: 1M file items under 5,000 groups,
with `files`, `sourced` and `content` rows for every file (indexes on
`items.parent` and `files.path`). Each query runs inside a transaction that
changed K items, the way commit assertions see it. Best of 7 runs per round,
three rounds alternating the old and new builds (`reldebug`, which is `-O3`), on
a shared 16-vCPU VM under load from other builds; milliseconds, lowest round.

| query | before | after |
|---|---:|---:|
| semi join on `id` from `mnemonicdb_tx_changes`, 1 key | 2 | 2 |
| same, 2 keys | 62 | 2 |
| same, 50 keys (`IN` filter) | 65 | 3 |
| same, 200 spread keys (key bitmap) | 67 | 6 |
| same, 200 consecutive ids (min/max) | 69 | 5 |
| inner join with `tx_changes` on `id`, 200 keys | 62 | 5 |
| same semi join, empty key set | 3 | 1 |
| `id IN (SELECT unnest(<2 ids>))` | 45 | 1 |
| `id IN (<200 constants>)` (rewritten to a join) | 46 | 5 |
| children of 2 groups (`parent IN`, AVET) | 83 | 3 |
| 200 keys under `AT (VERSION => n)` | 74 | 6 |
| full scan of `files` (reference) | 50 | 45 |

The assertions' own cost (each assertion query run once in the open
transaction, summed; best of 5, two rounds; ms). The "rule" set follows Mauvi's
DEC-081 rule (large tables read only as `id IN (<small set>)`); the other set
has one assertion that joins a fetched set straight to `items`, which DuckDB
still plans with the hash table on a full scan of `items` (it estimates every
semi join at 20% of its probe side).

| workload | before | after | after, rule set | rule set, skipping `minted` |
|---|---:|---:|---:|---:|
| change 1 item | 144 | 130 | 20 (138 before) | 21 |
| change 200 items | 834 | 218 | 35 (694 before) | 33 |
| install 1,000 new files | 2,438 | 629 | 225 (2,087 before) | 229 |

Whole commits add a new connection per commit, three statements per
assertion and a synced WAL write. On this VM the synced write alone varied
from 7 ms to 0.5 s between runs, so commit timings were too noisy to tabulate. In
the quietest runs a single-row commit with the rule set took 39–53 ms after
and at best 260 ms before; one changing 200 items 97 ms after and at best
0.84 s before (with no assertions: 20 ms after, about 0.2 s before, since the
`UPDATE ... WHERE id IN (...)` itself now seeks).

What changed:
1. Every pushed filter (exact, optional, dynamic) becomes a key set per
   column; points on `id` are EAVT point lookups, points or ranges on AVET
   columns AVET seeks, points on REF columns VAET lookups, and `id` ranges
   bound the AEVT merge. Join key bitmaps are enumerated by bisection.
2. A hash join with an empty build side in the scan's pipeline makes the scan
   read nothing.
3. `mnemonicdb_tx_changes` / `mnemonicdb_history` estimates; distinct counts
   for `id` and unique columns.
4. Fused scans keep the key sets parent joins push into either side.

## Projection pushdown, direct decoding, release build (2026-10-13)

- **NULL-rejecting filters drive membership.** A pushed filter that is false
  or NULL when its column is NULL (comparisons, BETWEEN, IN, IS NOT NULL, AND
  with any such child, OR of only such children) means rows without that
  attribute never pass. That attribute then decides table membership, and the
  scan reads only the filtered and projected attributes; the others aren't
  touched. `WHERE age = 42` on a 4-column table: 0.075 → 0.022 s.
- **Direct decoding.** Merge-mode scans decide membership from cursor
  positions, then decode each value straight from the RocksDB key into the
  output vector. List elements go into the list's child vector. No string
  copies, no `Value` objects.
- **Fusion bug fixed.** Outer joins whose optional side carries pushed filters
  are no longer fused. Those filters select matches, not output rows.

`bench/bench.py --rows 1000000` on the **release** build:

| benchmark | before these changes | now | native |
|---|---:|---:|---:|
| `count(*)` | 0.087 | 0.075 | 0.001 |
| one column | 0.093 | 0.080 | 0.001 |
| all 4 columns | 0.090 | 0.100 | 0.006 |
| unindexed filter (`age = 42`) | 0.075 | 0.022 | 0.001 |
| filter + other column (`sum(score) WHERE age > 90`) | — | 0.044 | 0.001 |
| list column scan (333k rows × 3 elements) | — | 0.134 | 0.002 |
| indexed range | 0.038 | 0.026 | 0.002 |
| join on id (fused) | 0.082 | 0.070 | 0.008 |
| join + indexed filter | 0.025 | 0.020 | 0.003 |
| UPDATE 10% | 1.547 | 0.946 | 0.005 |

**Release vs reldebug:** identical within noise. DuckDB's `reldebug` already
compiles with `-O3 -DNDEBUG` and only adds `-g`.

**Thread scaling** (release, 1M rows, compacted):

| threads | `count(*)` | all columns |
|---:|---:|---:|
| 1 | 0.414 | 0.504 |
| 2 | 0.207 | 0.256 |
| 4 | 0.105 | 0.131 |
| 8 | 0.070 | 0.083 |

Linear to 4 threads, about 6× at 8 (shared vCPUs).

## After the performance pass (2026-10-13)

Same machine and workload as the baseline below. "Native" is a DuckDB table
with an `id BIGINT PRIMARY KEY`.

| benchmark | baseline (s) | now (s) | speedup | native (s) |
|---|---:|---:|---:|---:|
| load 1M rows × 4 cols | 21.74 | 15.63 | 1.4× | 1.05 |
| load 500k rows (INSERT … SELECT) | 2.30 | 1.62 | 1.4× | 0.12 |
| `count(*)` | 0.362 | 0.087 | 4.2× | 0.001 |
| sum over all 4 columns | 0.452 | 0.090 | 5.0× | 0.006 |
| unindexed filter | 0.363 | 0.075 | 4.8× | 0.001 |
| indexed range (3% of rows) | 0.242 | 0.038 | 6.4× | 0.002 |
| 2-way join on id (fused) | 0.396 | 0.082 | 4.8× | 0.008 |
| join with a 1% table (fused) | — | 0.033 | | 0.002 |
| same, unfused hash join | — | 0.079 | | |
| join + indexed filter | 0.117 | 0.025 | 4.7× | 0.003 |
| UPDATE 10% of rows | 1.59 | 0.92 | 1.7× | 0.004 |
| DELETE 1% | 0.145 | 0.078 | 1.9× | 0.016 |
| point lookups / small transactions | ≈ native | ≈ native | | |
| size on disk (MB, before compaction) | 392 | 103 | 3.8× | 94 |
| size after `mnemonicdb_compact` (1M-row table alone) | 338 | 61 | 5.5× | |

What changed:
1. **New entities** skip lookups for attributes the transaction hasn't
   written yet, and undo-retraction only seeks for pairs this transaction
   retracted.
2. **PrefixReader** keeps cached iterators per column family and merges
   pending writes, so point lookups create no iterators.
3. **Compression:** LZ4, with ZSTD for the bottommost level (vendored,
   statically linked lz4 and zstd).
4. **Row estimates** from exact per-attribute datom counts.
5. **Parallel scans** over entity ranges or candidate slices, plus parallel
   UPDATE/DELETE sinks so their source scans run in parallel.
6. **Leapfrog in fused joins:** cursors of required tables jump ahead to the
   next entity every required table has. The work estimate uses the largest
   table, so selective fused joins still run in parallel.

Remaining gaps, and what would close them:
- **Scans:** about 75 ns per key per thread is RocksDB's floor for this
  layout. Getting near native needs a columnar cache of current state (see
  the recommendations in the conversation log / DESIGN.md roadmap).
- **Bulk load and UPDATE:** dominated by indexing pending writes in the
  WriteBatchWithIndex and by memtable inserts at commit. An SST-ingestion
  bulk path is the next step.

# Performance baseline (2026-10-13)

8 vCPU VM, `reldebug` build, DuckDB `main` at `2bb5316a8b5`, RocksDB 11.8.1,
no compression. Data fits in the OS page cache.

## `bench/bench.py --rows 1000000` (MnemonicDB vs a native DuckDB table)

| benchmark | MnemonicDB (s) | native (s) | ratio |
|---|---:|---:|---:|
| load 1M rows × 4 cols (one INSERT) | 21.74 | 1.06 | 20.6× |
| load 500k rows (INSERT … SELECT) | 2.30 | 0.12 | 19.7× |
| CHECKPOINT | 1.47 | 0.004 | |
| `count(*)` | 0.362 | 0.001 | 362× |
| `sum(one column)` | 0.377 | 0.001 | 377× |
| sum over all 4 columns | 0.452 | 0.008 | 57× |
| unindexed filter | 0.363 | 0.001 | 363× |
| point lookup, unique column | 0.001 | 0.001 | 1× |
| point lookup, id | 0.007 | 0.002 | 3.5× |
| CREATE INDEX | 1.09 | 0.04 | 26× |
| indexed range (3% of rows) | 0.242 | 0.002 | 121× |
| 2-way join on id (fused) | 0.396 | 0.007 | 57× |
| 2-way join on id (unfused) | 0.415 | | |
| join + indexed filter | 0.117 | 0.005 | 23× |
| UPDATE 10% of rows | 1.59 | 0.004 | 398× |
| UPDATE one row by unique column | 0.002 | 0.001 | 2× |
| DELETE 1% | 0.145 | 0.015 | 10× |
| `AT` full scan | 0.395 | | |
| `AT` point lookup | 0.001 | | |
| 2,000 autocommit single-row INSERTs | 0.75 | 0.93 | 0.8× |
| size on disk (MB) | 392 | 94 | 4.2× |

## Where the time goes (perf)

- **Scans:** about 76% is RocksDB iteration (`DBIter::Next`, merging heap)
  at about 75–80 ns per key, and a scan of N rows × C columns is N·C keys.
  About 18% is our decode and row assembly. These change it by at most
  about 10%: a 512 MB block cache, no checksums, readahead, full compaction.
- **Bulk insert:**
  - 40%: lookups that cannot find anything for brand-new entities
    (redundancy check, undo-retraction, uniqueness);
  - 36%: indexing every key in the WriteBatchWithIndex skiplist;
  - 15%: commit (memtable inserts and WAL).
- **UPDATE:** each row creates new RocksDB iterators (snapshot and batch) for
  its current-value and undo-retraction lookups, so iterator setup
  dominates.
- **Index-driven lookups:** each candidate entity opens one iterator per
  attribute.
- **Small transactions:** DuckDB statement overhead plus a synced WAL write
  per commit. Already on par with native DuckDB.

## Experiments

- **LZ4 compression:** the 1M-row database goes from 338 MB to 114 MB (about
  1.2× native) with no scan slowdown. Needs lz4 (and zstd, whose headers
  clash with DuckDB's vendored copy) linked statically into RocksDB.
- **Prefix Bloom filters** (8-byte fixed prefix on EAVT/AEVT/VAET and their
  history CFs, plus memtable prefix blooms, used through `iterate_upper_bound`
  + `auto_prefix_mode`). Measured on a 1M-row database in 39 SST files,
  running random point UPDATEs:
  - SST probes skipped: 6,951 of 7,610;
  - data-block accesses: 9.5k instead of 16.5k (42% fewer);
  - wall time: unchanged to 5% slower while data is in the page cache;
  - cold-cache runs: dominated by loading whole filter blocks at process
    start.

  Expected to pay off for data larger than RAM and long-running processes.
  Partitioned filters with `cache_index_and_filter_blocks` would cut the
  startup cost.
- **Patching RocksDB's per-key `ReadPathBlobResolver::Reset`:** no
  measurable gain (the profile was charging inlined iterator code to it).

## Reproducing

```sh
python3 bench/bench.py --rows 1000000            # MnemonicDB vs native
bench/warm_writes.sh build/reldebug/duckdb /tmp/warm 1000000 2000
python3 bench/keyset_bench.py --db DIR --minted --stat min   # key sets (MN-6); DIR keeps the 1M database
```
