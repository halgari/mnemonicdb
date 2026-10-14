#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "mnemonicdb/schema.hpp"
#include "mnemonicdb/store.hpp"

namespace duckdb {

class MnemonicSchemaEntry;
class MnemonicTableEntry;
class MnemonicCatalog;
class MnemonicTransaction;
class AssertionStatementCache;

//! The catalog entries for one immutable Schema. Transactions hold on to the
//! generation they read, so entries stay alive for as long as any transaction
//! can see them.
class MnemonicCatalogGeneration {
public:
	MnemonicCatalogGeneration(MnemonicCatalog &catalog, shared_ptr<const mnemonicdb::Schema> schema);
	~MnemonicCatalogGeneration();

	const shared_ptr<const mnemonicdb::Schema> &GetSchema() const {
		return schema;
	}
	optional_ptr<MnemonicTableEntry> GetTable(const string &schema_name, const string &table_name) const;
	void ScanTables(const string &schema_name, const std::function<void(CatalogEntry &)> &callback) const;
	optional_ptr<CatalogEntry> GetIndex(const string &schema_name, const string &index_name) const;
	void ScanIndexes(const string &schema_name, const std::function<void(CatalogEntry &)> &callback) const;
	optional_ptr<CatalogEntry> GetType(const string &schema_name, const string &type_name) const;
	void ScanTypes(const string &schema_name, const std::function<void(CatalogEntry &)> &callback) const;
	//! The schema entry, if the schema exists in this generation
	optional_ptr<MnemonicSchemaEntry> GetSchemaEntry(const string &schema_name) const;
	void ScanSchemas(const std::function<void(SchemaCatalogEntry &)> &callback) const;

private:
	shared_ptr<const mnemonicdb::Schema> schema;
	//! Schema entries are owned by the catalog (see MnemonicCatalog::GetSchemaEntry)
	vector<reference<MnemonicSchemaEntry>> schemas;
	case_insensitive_map_t<case_insensitive_map_t<unique_ptr<MnemonicTableEntry>>> tables;
	case_insensitive_map_t<case_insensitive_map_t<unique_ptr<CatalogEntry>>> indexes;
	case_insensitive_map_t<case_insensitive_map_t<unique_ptr<CatalogEntry>>> types;
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
	PhysicalOperator &PlanMergeInto(ClientContext &context, PhysicalPlanGenerator &planner, LogicalMergeInto &op,
	                                PhysicalOperator &plan) override;
	unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
	                                            unique_ptr<LogicalOperator> plan) override;

	DatabaseSize GetDatabaseSize(ClientContext &context) override;
	bool SupportsTimeTravel() const override {
		return true;
	}
	bool InMemory() override {
		return false;
	}
	string GetDBPath() override {
		return store->GetPath();
	}

	mnemonicdb::Store &GetStore() {
		return *store;
	}
	//! The entry for schema `name`, created on first use. Schema entries carry
	//! only a name and read everything else from the transaction's generation, so
	//! one entry per name serves every generation, and entries are never freed
	//! (DuckDB may hold on to one across a schema change)
	MnemonicSchemaEntry &GetSchemaEntry(const string &name);

	//! Held while a transaction takes its basis, snapshot and generation, and
	//! while a commit publishes them, so a transaction never sees a basis and a
	//! catalog generation from different commits
	mutex &GetSnapshotLock() {
		return snapshot_lock;
	}
	//! The generation of the latest committed schema
	shared_ptr<MnemonicCatalogGeneration> GetGeneration();
	void SetGeneration(shared_ptr<MnemonicCatalogGeneration> generation);
	//! The generation a catalog transaction should read
	shared_ptr<MnemonicCatalogGeneration> GetGeneration(CatalogTransaction transaction);

	//! Commit assertions run on a separate connection. While registered, every
	//! transaction that connection's context starts on this database borrows the
	//! committing transaction `parent`'s pending state (see MnemonicTransaction).
	void RegisterAssertionContext(ClientContext &context, MnemonicTransaction &parent);
	void UnregisterAssertionContext(ClientContext &context);
	optional_ptr<MnemonicTransaction> FindAssertionParent(ClientContext &context);
	//! The parsed assertion queries, kept across commits
	AssertionStatementCache &GetAssertionCache() {
		return *assertion_cache;
	}

private:
	void DropSchema(ClientContext &context, DropInfo &info) override;

private:
	unique_ptr<mnemonicdb::Store> store;
	shared_ptr<const mnemonicdb::Schema> initial_schema;
	//! Every schema entry handed out so far, by exact name: a schema dropped and
	//! created again in different case gets an entry spelled the new way
	mutex schema_entry_lock;
	unordered_map<string, unique_ptr<MnemonicSchemaEntry>> schema_entries;
	mutex snapshot_lock;
	mutex generation_lock;
	shared_ptr<MnemonicCatalogGeneration> generation;
	mutex assertion_lock;
	unordered_map<ClientContext *, MnemonicTransaction *> assertion_contexts;
	unique_ptr<AssertionStatementCache> assertion_cache;
};

} // namespace duckdb
