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
```
