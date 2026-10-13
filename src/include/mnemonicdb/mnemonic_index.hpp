#pragma once

#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"

namespace duckdb {

class MnemonicTableEntry;
struct CreateIndexInfo;

//! A CREATE INDEX on one attribute: the attribute gets AVET entries
class MnemonicIndexEntry : public IndexCatalogEntry {
public:
	MnemonicIndexEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateIndexInfo &info, Identifier table_name);

	Identifier GetSchemaName() const override;
	Identifier GetTableName() const override;

private:
	Identifier table_name;
};

//! The CreateIndexInfo describing an index on `column` of `table`
unique_ptr<CreateIndexInfo> MakeIndexInfo(MnemonicTableEntry &table, const string &index_name, const string &column);

//! Binds CREATE INDEX on a MnemonicDB table
unique_ptr<LogicalOperator> BindMnemonicCreateIndex(MnemonicTableEntry &table, unique_ptr<CreateIndexInfo> info);

} // namespace duckdb
