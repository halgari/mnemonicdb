#include "mnemonicdb/mnemonic_catalog.hpp"
#include "mnemonicdb/mnemonic_index.hpp"
#include "mnemonicdb/mnemonic_insert.hpp"
#include "mnemonicdb/mnemonic_modify.hpp"
#include "mnemonicdb/mnemonic_schema_entry.hpp"
#include "mnemonicdb/mnemonic_table_entry.hpp"
#include "mnemonicdb/mnemonic_transaction.hpp"

#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/storage/database_size.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Generation
//===--------------------------------------------------------------------===//
MnemonicCatalogGeneration::MnemonicCatalogGeneration(MnemonicCatalog &catalog,
                                                     shared_ptr<const mnemonicdb::Schema> schema_p)
    : schema(std::move(schema_p)) {
	for (auto schema_name : {"main", "sys"}) {
		auto &schema_entry = catalog.GetSchemaEntry(schema_name);
		for (auto table : schema->TablesInSchema(schema_name)) {
			auto info = MnemonicTableEntry::MakeCreateInfo(schema_entry, *schema, *table);
			auto &table_entry = *(tables[schema_name][table->name] =
			                          make_uniq<MnemonicTableEntry>(catalog, schema_entry, *info, schema, *table));
			for (auto local : table->attributes) {
				auto &attr = schema->GetAttribute(local);
				if (attr.index_name.empty()) {
					continue;
				}
				auto index_info = MakeIndexInfo(table_entry, attr.index_name, attr.name);
				indexes[schema_name][attr.index_name] =
				    make_uniq<MnemonicIndexEntry>(catalog, schema_entry, *index_info, table_entry.name);
			}
		}
	}
}

optional_ptr<CatalogEntry> MnemonicCatalogGeneration::GetIndex(const string &schema_name,
                                                                const string &index_name) const {
	auto schema_indexes = indexes.find(schema_name);
	if (schema_indexes == indexes.end()) {
		return nullptr;
	}
	auto entry = schema_indexes->second.find(index_name);
	return entry == schema_indexes->second.end() ? nullptr : entry->second.get();
}

void MnemonicCatalogGeneration::ScanIndexes(const string &schema_name,
                                            const std::function<void(CatalogEntry &)> &callback) const {
	auto schema_indexes = indexes.find(schema_name);
	if (schema_indexes == indexes.end()) {
		return;
	}
	for (auto &entry : schema_indexes->second) {
		callback(*entry.second);
	}
}

MnemonicCatalogGeneration::~MnemonicCatalogGeneration() = default;

optional_ptr<MnemonicTableEntry> MnemonicCatalogGeneration::GetTable(const string &schema_name,
                                                                      const string &table_name) const {
	auto schema_tables = tables.find(schema_name);
	if (schema_tables == tables.end()) {
		return nullptr;
	}
	auto entry = schema_tables->second.find(table_name);
	return entry == schema_tables->second.end() ? nullptr : entry->second.get();
}

void MnemonicCatalogGeneration::ScanTables(const string &schema_name,
                                           const std::function<void(CatalogEntry &)> &callback) const {
	auto schema_tables = tables.find(schema_name);
	if (schema_tables == tables.end()) {
		return;
	}
	for (auto &entry : schema_tables->second) {
		callback(*entry.second);
	}
}

//===--------------------------------------------------------------------===//
// Catalog
//===--------------------------------------------------------------------===//
MnemonicCatalog::MnemonicCatalog(AttachedDatabase &db, unique_ptr<mnemonicdb::Store> store_p,
                                 shared_ptr<const mnemonicdb::Schema> schema)
    : Catalog(db), store(std::move(store_p)), initial_schema(std::move(schema)) {
}

MnemonicCatalog::~MnemonicCatalog() = default;

void MnemonicCatalog::Initialize(bool load_builtin) {
	for (auto name : {"main", "sys"}) {
		CreateSchemaInfo info;
		info.SetQualifiedName(QualifiedName({Identifier(name)}, Identifier()));
		info.internal = true;
		schemas.push_back(make_uniq<MnemonicSchemaEntry>(*this, info));
	}
	generation = make_shared_ptr<MnemonicCatalogGeneration>(*this, initial_schema);
	initial_schema.reset();
}

MnemonicSchemaEntry &MnemonicCatalog::GetSchemaEntry(const string &name) {
	for (auto &schema : schemas) {
		if (schema->name == name) {
			return *schema;
		}
	}
	throw InternalException("MnemonicDB: no schema \"%s\"", name);
}

shared_ptr<MnemonicCatalogGeneration> MnemonicCatalog::GetGeneration() {
	lock_guard<mutex> guard(generation_lock);
	return generation;
}

void MnemonicCatalog::SetGeneration(shared_ptr<MnemonicCatalogGeneration> new_generation) {
	lock_guard<mutex> guard(generation_lock);
	generation = std::move(new_generation);
}

shared_ptr<MnemonicCatalogGeneration> MnemonicCatalog::GetGeneration(CatalogTransaction transaction) {
	if (transaction.HasContext()) {
		return MnemonicTransaction::Get(transaction.GetContext(), *this).GetGeneration();
	}
	return GetGeneration();
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

PhysicalOperator &MnemonicCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner,
                                              LogicalInsert &op, optional_ptr<PhysicalOperator> plan) {
	return PlanMnemonicInsert(context, planner, op, plan);
}

PhysicalOperator &MnemonicCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner,
                                              LogicalDelete &op, PhysicalOperator &plan) {
	return PlanMnemonicDelete(context, planner, op, plan);
}

PhysicalOperator &MnemonicCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner,
                                              LogicalUpdate &op, PhysicalOperator &plan) {
	return PlanMnemonicUpdate(context, planner, op, plan);
}

unique_ptr<LogicalOperator> MnemonicCatalog::BindCreateIndex(Binder &, CreateStatement &stmt, TableCatalogEntry &table,
                                                             unique_ptr<LogicalOperator>) {
	auto info = unique_ptr_cast<CreateInfo, CreateIndexInfo>(std::move(stmt.info));
	return BindMnemonicCreateIndex(table.Cast<MnemonicTableEntry>(), std::move(info));
}

DatabaseSize MnemonicCatalog::GetDatabaseSize(ClientContext &) {
	DatabaseSize size;
	size.bytes = store->ApproximateSize();
	return size;
}

} // namespace duckdb
