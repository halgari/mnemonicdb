#include "mnemonicdb/mnemonic_schema_entry.hpp"

#include "duckdb/common/exception.hpp"
#include "mnemonicdb/mnemonic_catalog.hpp"
#include "mnemonicdb/mnemonic_table_entry.hpp"
#include "mnemonicdb/mnemonic_transaction.hpp"

#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"

namespace duckdb {

MnemonicSchemaEntry::MnemonicSchemaEntry(Catalog &catalog, CreateSchemaInfo &info)
    : SchemaCatalogEntry(catalog, info) {
}

void MnemonicSchemaEntry::Scan(ClientContext &context, CatalogType type,
                               const std::function<void(CatalogEntry &)> &callback) {
	if (type != CatalogType::TABLE_ENTRY) {
		return;
	}
	auto &mnemonic_catalog = catalog.Cast<MnemonicCatalog>();
	MnemonicTransaction::Get(context, mnemonic_catalog).GetGeneration()->ScanTables(name.GetIdentifierName(), callback);
}

void MnemonicSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	if (type != CatalogType::TABLE_ENTRY) {
		return;
	}
	catalog.Cast<MnemonicCatalog>().GetGeneration()->ScanTables(name.GetIdentifierName(), callback);
}

static NotImplementedException Unsupported(const string &what) {
	return NotImplementedException("MnemonicDB does not support %s yet", what);
}

optional_ptr<CatalogEntry> MnemonicSchemaEntry::CreateIndex(CatalogTransaction, CreateIndexInfo &,
                                                            TableCatalogEntry &) {
	throw Unsupported("CREATE INDEX");
}

optional_ptr<CatalogEntry> MnemonicSchemaEntry::CreateFunction(CatalogTransaction, CreateFunctionInfo &) {
	throw Unsupported("creating functions");
}

optional_ptr<CatalogEntry> MnemonicSchemaEntry::CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) {
	auto &transaction_entry = MnemonicTransaction::Get(transaction.GetContext(), catalog);
	return transaction_entry.CreateTable(*this, info);
}

optional_ptr<CatalogEntry> MnemonicSchemaEntry::CreateView(CatalogTransaction, CreateViewInfo &) {
	throw Unsupported("CREATE VIEW");
}

optional_ptr<CatalogEntry> MnemonicSchemaEntry::CreateSequence(CatalogTransaction, CreateSequenceInfo &) {
	throw Unsupported("CREATE SEQUENCE");
}

optional_ptr<CatalogEntry> MnemonicSchemaEntry::CreateTableFunction(CatalogTransaction, CreateTableFunctionInfo &) {
	throw Unsupported("creating table functions");
}

optional_ptr<CatalogEntry> MnemonicSchemaEntry::CreateCopyFunction(CatalogTransaction, CreateCopyFunctionInfo &) {
	throw Unsupported("creating copy functions");
}

optional_ptr<CatalogEntry> MnemonicSchemaEntry::CreatePragmaFunction(CatalogTransaction,
                                                                     CreatePragmaFunctionInfo &) {
	throw Unsupported("creating pragma functions");
}

optional_ptr<CatalogEntry> MnemonicSchemaEntry::CreateCollation(CatalogTransaction, CreateCollationInfo &) {
	throw Unsupported("CREATE COLLATION");
}

optional_ptr<CatalogEntry> MnemonicSchemaEntry::CreateType(CatalogTransaction, CreateTypeInfo &) {
	throw Unsupported("CREATE TYPE");
}

optional_ptr<CatalogEntry> MnemonicSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                            const EntryLookupInfo &lookup_info) {
	if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	auto generation = catalog.Cast<MnemonicCatalog>().GetGeneration(transaction);
	return generation->GetTable(name.GetIdentifierName(), lookup_info.GetEntryName()).get();
}

void MnemonicSchemaEntry::DropEntry(ClientContext &, DropInfo &) {
	throw Unsupported("DROP");
}

void MnemonicSchemaEntry::Alter(CatalogTransaction, AlterInfo &) {
	throw Unsupported("ALTER");
}

} // namespace duckdb
