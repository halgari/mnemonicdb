#include "mnemonicdb/mnemonic_catalog.hpp"
#include "mnemonicdb/mnemonic_schema_entry.hpp"

#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/storage/database_size.hpp"

namespace duckdb {

MnemonicCatalog::MnemonicCatalog(AttachedDatabase &db, unique_ptr<mnemonicdb::Store> store_p)
    : Catalog(db), store(std::move(store_p)) {
}

MnemonicCatalog::~MnemonicCatalog() = default;

void MnemonicCatalog::Initialize(bool load_builtin) {
	for (auto name : {"main", "sys"}) {
		CreateSchemaInfo info;
		info.SetQualifiedName(QualifiedName({Identifier(name)}, Identifier()));
		info.internal = true;
		schemas.push_back(make_uniq<MnemonicSchemaEntry>(*this, info));
	}
}

optional_ptr<CatalogEntry> MnemonicCatalog::CreateSchema(CatalogTransaction, CreateSchemaInfo &) {
	throw NotImplementedException("MnemonicDB schemas are fixed: only \"main\" and \"sys\" exist");
}

void MnemonicCatalog::DropSchema(ClientContext &, DropInfo &) {
	throw NotImplementedException("MnemonicDB schemas are fixed: only \"main\" and \"sys\" exist");
}

optional_ptr<SchemaCatalogEntry> MnemonicCatalog::LookupSchema(CatalogTransaction, const EntryLookupInfo &schema_lookup,
                                                               OnEntryNotFound if_not_found) {
	auto &name = schema_lookup.GetEntryName();
	for (auto &schema : schemas) {
		if (schema->name == name) {
			return schema.get();
		}
	}
	if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
		throw CatalogException("Schema with name \"%s\" not found in MnemonicDB database", name);
	}
	return nullptr;
}

void MnemonicCatalog::ScanSchemas(ClientContext &, std::function<void(SchemaCatalogEntry &)> callback) {
	for (auto &schema : schemas) {
		callback(*schema);
	}
}

PhysicalOperator &MnemonicCatalog::PlanCreateTableAs(ClientContext &, PhysicalPlanGenerator &, LogicalCreateTable &,
                                                     PhysicalOperator &) {
	throw NotImplementedException("MnemonicDB does not support CREATE TABLE AS yet");
}

PhysicalOperator &MnemonicCatalog::PlanInsert(ClientContext &, PhysicalPlanGenerator &, LogicalInsert &,
                                              optional_ptr<PhysicalOperator>) {
	throw NotImplementedException("MnemonicDB does not support INSERT yet");
}

PhysicalOperator &MnemonicCatalog::PlanDelete(ClientContext &, PhysicalPlanGenerator &, LogicalDelete &,
                                              PhysicalOperator &) {
	throw NotImplementedException("MnemonicDB does not support DELETE yet");
}

PhysicalOperator &MnemonicCatalog::PlanUpdate(ClientContext &, PhysicalPlanGenerator &, LogicalUpdate &,
                                              PhysicalOperator &) {
	throw NotImplementedException("MnemonicDB does not support UPDATE yet");
}

DatabaseSize MnemonicCatalog::GetDatabaseSize(ClientContext &) {
	DatabaseSize size;
	size.bytes = store->ApproximateSize();
	return size;
}

} // namespace duckdb
