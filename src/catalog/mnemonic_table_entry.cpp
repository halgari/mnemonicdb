#include "mnemonicdb/mnemonic_table_entry.hpp"
#include "mnemonicdb/mnemonic_scan.hpp"

#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/storage/table_storage_info.hpp"

namespace duckdb {

MnemonicTableEntry::MnemonicTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                                       shared_ptr<const mnemonicdb::Schema> mnemonic_schema_p,
                                       const mnemonicdb::TableDef &table_p)
    : TableCatalogEntry(catalog, schema, info), columns(info.columns.Copy()),
      mnemonic_schema(std::move(mnemonic_schema_p)), table(table_p) {
}

unique_ptr<CreateTableInfo> MnemonicTableEntry::MakeCreateInfo(SchemaCatalogEntry &schema,
                                                               const mnemonicdb::Schema &mnemonic,
                                                               const mnemonicdb::TableDef &table) {
	auto info = make_uniq<CreateTableInfo>(schema, Identifier(table.name));
	info->columns.AddColumn(ColumnDefinition(Identifier("id"), mnemonicdb::RefType()));
	for (auto local : table.attributes) {
		auto &attr = mnemonic.GetAttribute(local);
		info->columns.AddColumn(ColumnDefinition(Identifier(attr.name), attr.type));
	}
	for (idx_t i = 0; i < table.attributes.size(); i++) {
		auto &attr = mnemonic.GetAttribute(table.attributes[i]);
		if (attr.unique == mnemonicdb::Uniqueness::NONE) {
			continue;
		}
		bool primary = attr.unique == mnemonicdb::Uniqueness::IDENTITY;
		info->constraints.push_back(make_uniq<UniqueConstraint>(LogicalIndex(i + 1), Identifier(attr.name), primary));
		if (primary) {
			info->constraints.push_back(make_uniq<NotNullConstraint>(LogicalIndex(i + 1)));
		}
	}
	return info;
}

const mnemonicdb::AttributeDef &MnemonicTableEntry::GetAttribute(idx_t index) const {
	D_ASSERT(index > MNEMONIC_ID_COLUMN_INDEX && index <= table.attributes.size());
	return mnemonic_schema->GetAttribute(table.attributes[index - 1]);
}

unique_ptr<BaseStatistics> MnemonicTableEntry::GetStatistics(ClientContext &, column_t) {
	return nullptr;
}

TableFunction MnemonicTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	return MnemonicScanFunction(context, *this, bind_data, nullptr);
}

TableFunction MnemonicTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data,
                                                  const EntryLookupInfo &lookup_info) {
	return MnemonicScanFunction(context, *this, bind_data, lookup_info.GetAtClause());
}

TableStorageInfo MnemonicTableEntry::GetStorageInfo(ClientContext &) {
	TableStorageInfo info;
	for (idx_t i = 0; i < table.attributes.size(); i++) {
		auto &attr = mnemonic_schema->GetAttribute(table.attributes[i]);
		if (attr.unique == mnemonicdb::Uniqueness::NONE) {
			continue;
		}
		IndexInfo index;
		index.is_unique = true;
		index.is_primary = attr.unique == mnemonicdb::Uniqueness::IDENTITY;
		index.is_foreign = false;
		index.column_set.insert(i + 1);
		info.index_info.push_back(std::move(index));
	}
	return info;
}

virtual_column_map_t MnemonicTableEntry::GetVirtualColumns() const {
	virtual_column_map_t result;
	result.emplace(MNEMONIC_ROWID_COLUMN, TableColumn(Identifier("rowid"), mnemonicdb::RefType()));
	return result;
}

vector<column_t> MnemonicTableEntry::GetRowIdColumns() const {
	return {MNEMONIC_ROWID_COLUMN};
}

} // namespace duckdb
