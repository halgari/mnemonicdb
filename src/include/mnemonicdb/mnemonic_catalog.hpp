#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "mnemonicdb/store.hpp"

namespace duckdb {

class MnemonicSchemaEntry;

class MnemonicCatalog : public Catalog {
public:
	MnemonicCatalog(AttachedDatabase &db, unique_ptr<mnemonicdb::Store> store);
	~MnemonicCatalog() override;

	static constexpr const char *CATALOG_TYPE = "mnemonicdb";

public:
	void Initialize(bool load_builtin) override;
	string GetCatalogType() override {
		return CATALOG_TYPE;
	}

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;
	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override;
	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;

	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, LogicalCreateTable &op,
	                                    PhysicalOperator &plan) override;
	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                             optional_ptr<PhysicalOperator> plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override;

	DatabaseSize GetDatabaseSize(ClientContext &context) override;
	bool InMemory() override {
		return false;
	}
	string GetDBPath() override {
		return store->GetPath();
	}

	mnemonicdb::Store &GetStore() {
		return *store;
	}

private:
	void DropSchema(ClientContext &context, DropInfo &info) override;

private:
	unique_ptr<mnemonicdb::Store> store;
	//! The fixed schemas: `main` for user tables and `sys` for the schema-as-data tables
	vector<unique_ptr<MnemonicSchemaEntry>> schemas;
};

} // namespace duckdb
