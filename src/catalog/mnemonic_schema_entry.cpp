#include "mnemonicdb/mnemonic_schema_entry.hpp"

#include "duckdb/common/exception.hpp"
#include "mnemonicdb/mnemonic_catalog.hpp"
#include "mnemonicdb/mnemonic_table_entry.hpp"
#include "mnemonicdb/mnemonic_transaction.hpp"

#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/common/enum_util.hpp"

namespace duckdb {

MnemonicSchemaEntry::MnemonicSchemaEntry(Catalog &catalog, CreateSchemaInfo &info) : SchemaCatalogEntry(catalog, info) {
}

static void ScanGeneration(const MnemonicCatalogGeneration &generation, const string &schema, CatalogType type,
                           const std::function<void(CatalogEntry &)> &callback) {
	if (type == CatalogType::TABLE_ENTRY) {
		generation.ScanTables(schema, callback);
	} else if (type == CatalogType::INDEX_ENTRY) {
		generation.ScanIndexes(schema, callback);
	} else if (type == CatalogType::TYPE_ENTRY) {
		generation.ScanTypes(schema, callback);
	}
}

void MnemonicSchemaEntry::Scan(ClientContext &context, CatalogType type,
                               const std::function<void(CatalogEntry &)> &callback) {
	auto &mnemonic_catalog = catalog.Cast<MnemonicCatalog>();
	ScanGeneration(*MnemonicTransaction::Get(context, mnemonic_catalog).GetGeneration(), name.GetIdentifierName(), type,
	               callback);
}

void MnemonicSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	ScanGeneration(*catalog.Cast<MnemonicCatalog>().GetGeneration(), name.GetIdentifierName(), type, callback);
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

optional_ptr<CatalogEntry> MnemonicSchemaEntry::CreateTable(CatalogTransaction transaction,
                                                            BoundCreateTableInfo &info) {
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

optional_ptr<CatalogEntry> MnemonicSchemaEntry::CreatePragmaFunction(CatalogTransaction, CreatePragmaFunctionInfo &) {
	throw Unsupported("creating pragma functions");
}

optional_ptr<CatalogEntry> MnemonicSchemaEntry::CreateCollation(CatalogTransaction, CreateCollationInfo &) {
	throw Unsupported("CREATE COLLATION");
}

optional_ptr<CatalogEntry> MnemonicSchemaEntry::CreateType(CatalogTransaction transaction, CreateTypeInfo &info) {
	auto &transaction_entry = MnemonicTransaction::Get(transaction.GetContext(), catalog);
	return transaction_entry.CreateType(name.GetIdentifierName(), info);
}

optional_ptr<CatalogEntry> MnemonicSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                            const EntryLookupInfo &lookup_info) {
	auto type = lookup_info.GetCatalogType();
	if (type != CatalogType::TABLE_ENTRY && type != CatalogType::INDEX_ENTRY && type != CatalogType::TYPE_ENTRY) {
		return nullptr;
	}
	auto generation = catalog.Cast<MnemonicCatalog>().GetGeneration(transaction);
	if (type == CatalogType::INDEX_ENTRY) {
		return generation->GetIndex(name.GetIdentifierName(), lookup_info.GetEntryName());
	}
	if (type == CatalogType::TYPE_ENTRY) {
		// Unknown names must come back null: type names in CREATE TABLE are looked
		// up here before the system catalog
		return generation->GetType(name.GetIdentifierName(), lookup_info.GetEntryName());
	}
	return generation->GetTable(name.GetIdentifierName(), lookup_info.GetEntryName()).get();
}

void MnemonicSchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
	auto &transaction = MnemonicTransaction::Get(context, catalog);
	auto entry_name = info.GetQualifiedName().Name().GetIdentifierName();
	bool if_exists = info.if_not_found != OnEntryNotFound::THROW_EXCEPTION;
	if (info.type == CatalogType::INDEX_ENTRY) {
		transaction.DropIndex(name.GetIdentifierName(), entry_name, if_exists);
	} else if (info.type == CatalogType::TYPE_ENTRY) {
		if (info.cascade) {
			throw Unsupported("DROP TYPE ... CASCADE");
		}
		transaction.DropType(name.GetIdentifierName(), entry_name, if_exists);
	} else {
		throw Unsupported("DROP " + EnumUtil::ToString(info.type));
	}
}

void MnemonicSchemaEntry::Alter(CatalogTransaction transaction, AlterInfo &info) {
	if (info.type != AlterType::ALTER_TABLE) {
		throw Unsupported("ALTER " + EnumUtil::ToString(info.type));
	}
	auto &transaction_entry = MnemonicTransaction::Get(transaction.GetContext(), catalog);
	transaction_entry.AlterTable(name.GetIdentifierName(), info.Cast<AlterTableInfo>());
}

} // namespace duckdb
