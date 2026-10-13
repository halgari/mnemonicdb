# MnemonicDB

A DuckDB extension providing an immutable, Datomic-style database backed by
RocksDB. Tables are attribute namespaces over one shared entity-ID space, and
DuckDB's `AT` clause queries the database as of any past transaction.

See [DESIGN.md](DESIGN.md) for the data model, storage layout and roadmap.

```sql
ATTACH 'data/app.mnemonic' AS d (TYPE mnemonicdb);
SELECT * FROM mnemonicdb_info('d');
```

## Building

Submodules: `duckdb` (tracks `main`, pinned commit), `extension-ci-tools`, and
`third_party/rocksdb` (pinned release). RocksDB is built from source and linked
statically.

```sh
git submodule update --init --recursive
GEN=ninja make reldebug
```

Requires CMake, Ninja and a C++20 compiler.

Outputs:
- `build/reldebug/duckdb`: DuckDB shell with the extension statically linked
- `build/reldebug/extension/mnemonicdb/mnemonicdb.duckdb_extension`: loadable extension

## Testing

```sh
build/reldebug/test/unittest --test-dir . "test/sql/*"
```
