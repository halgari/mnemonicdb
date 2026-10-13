#pragma once

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "mnemonicdb/schema.hpp"

namespace duckdb {

//! The hidden row-id column used by UPDATE and DELETE; carries the same value as `id`
#define MNEMONIC_ROWID_COLUMN COLUMN_IDENTIFIER_ROW_ID
//! Physical index of the `id` column, present in every table
static constexpr idx_t MNEMONIC_ID_COLUMN_INDEX = 0;

class MnemonicTableEntry : public TableCatalogEntry {
public:
	MnemonicTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
	                   shared_ptr<const mnemonicdb::Schema> mnemonic_schema, const mnemonicdb::TableDef &table);

	//! The CREATE TABLE info describing a table definition
	static unique_ptr<CreateTableInfo> MakeCreateInfo(SchemaCatalogEntry &schema, const mnemonicdb::Schema &mnemonic,
	                                                  const mnemonicdb::TableDef &table);

public:
	const ColumnList &GetColumns() const override {
		return columns;
	}
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data,
	                              const EntryLookupInfo &lookup_info) override;
	TableStorageInfo GetStorageInfo(ClientContext &context) override;
	virtual_column_map_t GetVirtualColumns() const override;
	vector<column_t> GetRowIdColumns() const override;

	const mnemonicdb::Schema &GetMnemonicSchema() const {
		return *mnemonic_schema;
	}
	const mnemonicdb::TableDef &GetTableDef() const {
		return table;
	}
	//! The attribute behind physical column `index` (index 0 is `id`, which has none)
	const mnemonicdb::AttributeDef &GetAttribute(idx_t index) const;

private:
	ColumnList columns;
	shared_ptr<const mnemonicdb::Schema> mnemonic_schema;
	const mnemonicdb::TableDef &table;
};

} // namespace duckdb
