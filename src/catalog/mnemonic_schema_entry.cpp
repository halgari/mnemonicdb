#include "mnemonicdb/mnemonic_schema_entry.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"

namespace duckdb {

MnemonicSchemaEntry::MnemonicSchemaEntry(Catalog &catalog, CreateSchemaInfo &info)
    : SchemaCatalogEntry(catalog, info) {
}

void MnemonicSchemaEntry::Scan(ClientContext &context, CatalogType type,
                               const std::function<void(CatalogEntry &)> &callback) {
	Scan(type, callback);
}

void MnemonicSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	// No tables yet: entries are loaded from `sys` once schema-as-data lands
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

optional_ptr<CatalogEntry> MnemonicSchemaEntry::CreateTable(CatalogTransaction, BoundCreateTableInfo &) {
	throw Unsupported("CREATE TABLE");
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

optional_ptr<CatalogEntry> MnemonicSchemaEntry::LookupEntry(CatalogTransaction, const EntryLookupInfo &) {
	return nullptr;
}

void MnemonicSchemaEntry::DropEntry(ClientContext &, DropInfo &) {
	throw Unsupported("DROP");
}

void MnemonicSchemaEntry::Alter(CatalogTransaction, AlterInfo &) {
	throw Unsupported("ALTER");
}

} // namespace duckdb
