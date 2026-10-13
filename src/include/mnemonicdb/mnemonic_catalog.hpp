#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/mutex.hpp"
#include "mnemonicdb/schema.hpp"
#include "mnemonicdb/store.hpp"

namespace duckdb {

class MnemonicSchemaEntry;
class MnemonicTableEntry;
class MnemonicCatalog;

//! The catalog entries for one immutable Schema. Transactions hold on to the generation they read, so entries stay
//! alive for as long as any transaction can see them.
class MnemonicCatalogGeneration {
public:
	MnemonicCatalogGeneration(MnemonicCatalog &catalog, shared_ptr<const mnemonicdb::Schema> schema);
	~MnemonicCatalogGeneration();

	const shared_ptr<const mnemonicdb::Schema> &GetSchema() const {
		return schema;
	}
	optional_ptr<MnemonicTableEntry> GetTable(const string &schema_name, const string &table_name) const;
	void ScanTables(const string &schema_name, const std::function<void(CatalogEntry &)> &callback) const;

private:
	shared_ptr<const mnemonicdb::Schema> schema;
	case_insensitive_map_t<case_insensitive_map_t<unique_ptr<MnemonicTableEntry>>> tables;
};

class MnemonicCatalog : public Catalog {
public:
	MnemonicCatalog(AttachedDatabase &db, unique_ptr<mnemonicdb::Store> store,
	                shared_ptr<const mnemonicdb::Schema> schema);
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
	MnemonicSchemaEntry &GetSchemaEntry(const string &name);

	//! The generation of the latest committed schema
	shared_ptr<MnemonicCatalogGeneration> GetGeneration();
	void SetGeneration(shared_ptr<MnemonicCatalogGeneration> generation);
	//! The generation a catalog transaction should read
	shared_ptr<MnemonicCatalogGeneration> GetGeneration(CatalogTransaction transaction);

private:
	void DropSchema(ClientContext &context, DropInfo &info) override;

private:
	unique_ptr<mnemonicdb::Store> store;
	shared_ptr<const mnemonicdb::Schema> initial_schema;
	//! The fixed schemas: `main` for user tables and `sys` for the schema-as-data tables
	vector<unique_ptr<MnemonicSchemaEntry>> schemas;
	mutex generation_lock;
	shared_ptr<MnemonicCatalogGeneration> generation;
};

} // namespace duckdb
