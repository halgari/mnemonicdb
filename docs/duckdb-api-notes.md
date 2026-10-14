# DuckDB storage-extension API notes

Checked against DuckDB `main` at `2bb5316a8b5` (2026-10-12). Paths are
relative to `duckdb/`. Re-verify after bumping the submodule.

## Gotchas

1. **ON CONFLICT becomes MERGE INTO** (`bind_insert.cpp:628`).
   `LogicalInsert::on_conflict_info` is never set. The base
   `Catalog::PlanMergeInto` calls our `PlanInsert`/`PlanUpdate`/`PlanDelete`.
   It needs `GetStorageInfo().index_info` to list UNIQUE/PK constraints.
2. **UPDATE/DELETE need `get_bind_info` on the scan function.**
   `LogicalGet::GetTable()` (`logical_get.cpp:49`) uses it. Without it you
   get "Can only update base table".
3. **Virtual columns need `projection_pushdown = true`** (`plan_get.cpp:181`).
4. **Filters we accept are removed from the plan.** With
   `filter_pushdown = true` the scan must apply every non-optional pushed
   filter. Use `supports_pushdown_type` to decline one.
5. **No `LogicalType::SetAlias`.** Use `WithAlias(string)`. `RegisterType`
   does not add an alias itself.
6. **The `TableCatalogEntry` constructor moves `info.constraints` out**
   (`table_catalog_entry.cpp:32`).
7. **Schema `LookupEntry` must return null for unknown `TYPE_ENTRY` names.**
   Type names in CREATE TABLE are looked up in our catalog first. Our named
   types are `TypeCatalogEntry`s per generation.
8. **Override `BindCreateIndex`.** The default builds an ART index, and only
   DuckDB tables can plan that.
9. **Pure virtuals:**
   - `TableCatalogEntry`: `GetColumns`, `GetStatistics`, `GetScanFunction`,
     `GetStorageInfo`.
   - `Catalog`: `PlanCreateTableAs`, `PlanInsert`, and the 4-argument
     `PlanDelete`/`PlanUpdate`.
10. **Defaults that throw:**
    - `Catalog::BindAlterAddIndex` (ALTER TABLE ADD UNIQUE/PK)
    - `CreateTrigger`/`DropTrigger`
    - `PlanMergeInto` with RETURNING
11. **Named types have no alias in DuckDB.** `CREATE TYPE` stores a plain
    type, and CREATE TABLE binds column types in place, so the catalog cannot
    tell which named type a column used. We give our types the alias
    `schema.name`, but must strip it from column types: an alias blocks
    implicit casts (`cast_rules.cpp`, "if aliases are different") and so
    generic functions like `struct_extract` / `union_tag` stop binding.
12. **No `ALTER TYPE`.** The PEG grammar has `CREATE [OR REPLACE] TYPE` only
    (`create_type.gram`); OR REPLACE arrives as `REPLACE_ON_CONFLICT`.
13. **Type strings parse to one `UNBOUND` type** wrapping a `TypeExpression`
    tree (STRUCT/LIST/MAP are type constructors with child expressions).
    Literals carry no type, so a bound type cannot be spliced into the tree.
14. **Nested schemas exist** (`CreateSchemaInfo::IsNested`, `QualifiedName::Path`).

## Table entries and scans

`catalog/catalog_entry/table_catalog_entry.hpp`:
- The binder calls `GetScanFunction(ctx, bind_data, const EntryLookupInfo &)`
  (`bind_basetableref.cpp:262`). We fill `bind_data` ourselves;
  `TableFunction::bind` is not called.
- `GetVirtualColumns()` returns `virtual_column_map_t` (`column_t ->
  TableColumn{name, type}`). Ids must be `>= VIRTUAL_COLUMN_START`, except
  `COLUMN_IDENTIFIER_ROW_ID`. Don't use `VIRTUAL_COLUMN_START` exactly,
  because MERGE RETURNING uses it.
- `GetRowIdColumns()` lists the row-id columns used by UPDATE/DELETE. Our
  `id` goes here.
- `scan_function.get_virtual_columns` and `table.GetVirtualColumns()` must
  agree.
- `TableFunctionInitInput`:
  - `column_ids` are output columns and may be virtual.
  - `projection_ids` applies when `filter_prune` is set.
  - `filters` (`TableFilterSet`) is keyed by index into `column_ids`. Filters
    are `ExpressionFilter`s over `BoundReferenceExpression(0)`. Skip optional
    ones with `ExpressionFilter::IsOptionalFilter`.
- For `count(*)` with no columns, `LogicalGet::GetAnyColumn` picks
  `COLUMN_IDENTIFIER_EMPTY` if it's in the virtual map, else ROW_ID.

## CREATE / ALTER / INDEX

- `SchemaCatalogEntry::CreateTable(CatalogTransaction, BoundCreateTableInfo&)`:
  use `info.Base()` (`CreateTableInfo`).
  - `columns` have their types bound, aliases included.
  - `constraints` holds `UniqueConstraint` (`IsPrimaryKey`, `GetIndex`,
    `GetColumnNames`) and `NotNullConstraint`. A PK adds NOT NULL.
  - Return the entry, or nullptr for IF NOT EXISTS when it already exists.
- `Catalog::SupportsCreateTable` rejects PARTITIONED BY / SORTED BY / WITH
  options.
- `Alter`: cast to `AlterTableInfo` and switch on `alter_table_type`.
  - `ADD_COLUMN` gives `AddColumnInfo`.
  - `ALTER_COLUMN_TYPE` gives `ChangeColumnTypeInfo` (`target_type` is bound;
    the USING expression is not).
- `BindCreateIndex`: return a `LogicalExtensionOperator` whose plan calls
  `schema.CreateIndex(txn, CreateIndexInfo&, table)`.
- `GetStorageInfo` returns `TableStorageInfo{cardinality, index_info}`, with
  `IndexInfo{is_unique, is_primary, column_set /*physical idx*/}`.

## INSERT / UPDATE / DELETE

- Allocate operators with `planner.Make<T>(args...)`. The constructor takes
  `PhysicalPlan&` first. Use `PhysicalOperatorType::EXTENSION`, and attach the
  child with `op.children.push_back(plan)`.
- Sink operators implement `GetGlobalSinkState`, `Sink`, `Combine` and
  `Finalize`, plus `IsSink`, `ParallelSink`, `IsSource`, and
  `GetDataInternal` (not `GetData`) to emit the row count. See
  `physical_delete.cpp:259`.
- **INSERT:** the child emits the table's physical columns in order, cast,
  with defaults filled in. The output is a BIGINT count, or `table.GetTypes()`
  if `return_chunk`. RETURNING can't see virtual columns.
- **UPDATE:** `columns` (`PhysicalIndex`) and `expressions` (refs into the
  child). **Row-id columns are last in the child chunk.**
  `row_id_handling == KEEP_FIRST` for UPDATE ... FROM.
- **DELETE:** the row-id refs are appended last in `op.expressions`, so use
  `expressions.back()`.

## AT clause

- `BoundAtClause{Unit(), GetValue()}`. The unit is free-form; compare it
  case-insensitively. The value keeps its literal type, so a string
  timestamp arrives as VARCHAR.
- It reaches `LookupSchema`, `schema.LookupEntry` and
  `GetScanFunction(ctx, bind_data, lookup_info)` through
  `EntryLookupInfo::GetAtClause()`.
- `Catalog::SupportsTimeTravel()` must return true, or you get "Catalog type
  does not support time travel".

## REF type

```cpp
LogicalType REF() { return LogicalType(LogicalTypeId::UBIGINT).WithAlias("REF"); }
loader.RegisterType("REF", REF());
```
The alias survives into `BoundCreateTableInfo` column types, and
`operator==` compares aliases. Function results usually lose the alias.

## Transaction hooks

- `Transaction::SetReadWrite()` is called once, before the first statement
  that modifies this database (after binding). Call the base implementation
  first. It's the hook for taking the writer lock.
- `SetModifications(DatabaseModificationType)` is called on every modifying
  statement.
- Binding has already used the snapshot by then, so validate the basis after
  taking the lock.
