#include "mnemonicdb/mnemonic_catalog.hpp"
#include "mnemonicdb/mnemonic_assertions.hpp"
#include "mnemonicdb/mnemonic_index.hpp"
#include "mnemonicdb/mnemonic_insert.hpp"
#include "mnemonicdb/mnemonic_modify.hpp"
#include "mnemonicdb/mnemonic_schema_entry.hpp"
#include "mnemonicdb/mnemonic_table_entry.hpp"
#include "mnemonicdb/mnemonic_transaction.hpp"

#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/planner/operator/logical_merge_into.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/storage/database_size.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Generation
//===--------------------------------------------------------------------===//
MnemonicCatalogGeneration::MnemonicCatalogGeneration(MnemonicCatalog &catalog,
                                                     shared_ptr<const mnemonicdb::Schema> schema_p)
    : schema(std::move(schema_p)) {
	for (auto schema_def : schema->AllSchemas()) {
		auto &schema_name = schema_def->name;
		auto &schema_entry = catalog.GetSchemaEntry(schema_name);
		schemas.push_back(schema_entry);
		for (auto type : schema->TypesInSchema(schema_name)) {
			CreateTypeInfo info(type->name, type->type);
			info.SetQualifiedName(
			    QualifiedName(Identifier(catalog.GetName()), Identifier(schema_name), Identifier(type->name)));
			if (!type->doc.empty()) {
				info.comment = Value(type->doc);
			}
			types[schema_name][type->name] = make_uniq<TypeCatalogEntry>(catalog, schema_entry, info);
		}
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

optional_ptr<CatalogEntry> MnemonicCatalogGeneration::GetType(const string &schema_name,
                                                              const string &type_name) const {
	auto schema_types = types.find(schema_name);
	if (schema_types == types.end()) {
		return nullptr;
	}
	auto entry = schema_types->second.find(type_name);
	return entry == schema_types->second.end() ? nullptr : entry->second.get();
}

void MnemonicCatalogGeneration::ScanTypes(const string &schema_name,
                                          const std::function<void(CatalogEntry &)> &callback) const {
	auto schema_types = types.find(schema_name);
	if (schema_types == types.end()) {
		return;
	}
	for (auto &entry : schema_types->second) {
		callback(*entry.second);
	}
}

optional_ptr<MnemonicSchemaEntry> MnemonicCatalogGeneration::GetSchemaEntry(const string &schema_name) const {
	for (auto &entry : schemas) {
		if (StringUtil::CIEquals(entry.get().name.GetIdentifierName(), schema_name)) {
			return &entry.get();
		}
	}
	return nullptr;
}

void MnemonicCatalogGeneration::ScanSchemas(const std::function<void(SchemaCatalogEntry &)> &callback) const {
	for (auto &entry : schemas) {
		callback(entry.get());
	}
}

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
    : Catalog(db), store(std::move(store_p)), initial_schema(std::move(schema)),
      assertion_cache(make_uniq<AssertionStatementCache>()) {
}

MnemonicCatalog::~MnemonicCatalog() = default;

void MnemonicCatalog::Initialize(bool load_builtin) {
	generation = make_shared_ptr<MnemonicCatalogGeneration>(*this, initial_schema);
	initial_schema.reset();
}

MnemonicSchemaEntry &MnemonicCatalog::GetSchemaEntry(const string &name) {
	lock_guard<mutex> guard(schema_entry_lock);
	auto &entry = schema_entries[name];
	if (!entry) {
		CreateSchemaInfo info;
		info.SetQualifiedName(QualifiedName({Identifier(name)}, Identifier()));
		info.internal = name == "main" || name == "sys";
		entry = make_uniq<MnemonicSchemaEntry>(*this, info);
	}
	return *entry;
}

shared_ptr<MnemonicCatalogGeneration> MnemonicCatalog::GetGeneration() {
	lock_guard<mutex> guard(generation_lock);
	return generation;
}

void MnemonicCatalog::SetGeneration(shared_ptr<MnemonicCatalogGeneration> new_generation) {
	lock_guard<mutex> guard(generation_lock);
	generation = std::move(new_generation);
}

void MnemonicCatalog::RegisterAssertionContext(ClientContext &context, MnemonicTransaction &parent) {
	lock_guard<mutex> guard(assertion_lock);
	assertion_contexts[&context] = &parent;
}

void MnemonicCatalog::UnregisterAssertionContext(ClientContext &context) {
	lock_guard<mutex> guard(assertion_lock);
	assertion_contexts.erase(&context);
}

optional_ptr<MnemonicTransaction> MnemonicCatalog::FindAssertionParent(ClientContext &context) {
	lock_guard<mutex> guard(assertion_lock);
	auto entry = assertion_contexts.find(&context);
	return entry == assertion_contexts.end() ? nullptr : entry->second;
}

shared_ptr<MnemonicCatalogGeneration> MnemonicCatalog::GetGeneration(CatalogTransaction transaction) {
	if (transaction.HasContext()) {
		return MnemonicTransaction::Get(transaction.GetContext(), *this).GetGeneration();
	}
	return GetGeneration();
}

optional_ptr<CatalogEntry> MnemonicCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	if (info.IsNested()) {
		throw NotImplementedException("MnemonicDB does not support nested schemas");
	}
	if (!transaction.HasContext()) {
		throw InternalException("MnemonicDB: CREATE SCHEMA without a client context");
	}
	auto &transaction_entry = MnemonicTransaction::Get(transaction.GetContext(), *this);
	return transaction_entry.CreateSchema(info);
}

void MnemonicCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	auto &path = info.GetQualifiedName().Path();
	// The path is [catalog, schema] after binding, or just [schema]
	if (path.size() > 2) {
		throw NotImplementedException("MnemonicDB does not support nested schemas");
	}
	auto &transaction = MnemonicTransaction::Get(context, *this);
	transaction.DropSchema(info.GetQualifiedName().Name().GetIdentifierName(),
	                       info.if_not_found != OnEntryNotFound::THROW_EXCEPTION, info.cascade);
}

optional_ptr<SchemaCatalogEntry> MnemonicCatalog::LookupSchema(CatalogTransaction transaction,
                                                               const EntryLookupInfo &schema_lookup,
                                                               OnEntryNotFound if_not_found) {
	auto &name = schema_lookup.GetEntryName();
	auto entry = GetGeneration(transaction)->GetSchemaEntry(name);
	if (entry) {
		return entry.get();
	}
	if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
		throw CatalogException("Schema with name \"%s\" not found in MnemonicDB database", name);
	}
	return nullptr;
}

void MnemonicCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	MnemonicTransaction::Get(context, *this).GetGeneration()->ScanSchemas(callback);
}

PhysicalOperator &MnemonicCatalog::PlanCreateTableAs(ClientContext &, PhysicalPlanGenerator &, LogicalCreateTable &,
                                                     PhysicalOperator &) {
	throw NotImplementedException("MnemonicDB does not support CREATE TABLE AS yet");
}

PhysicalOperator &MnemonicCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                              optional_ptr<PhysicalOperator> plan) {
	return PlanMnemonicInsert(context, planner, op, plan);
}

PhysicalOperator &MnemonicCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                              PhysicalOperator &plan) {
	return PlanMnemonicDelete(context, planner, op, plan);
}

PhysicalOperator &MnemonicCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                              PhysicalOperator &plan) {
	return PlanMnemonicUpdate(context, planner, op, plan);
}

PhysicalOperator &MnemonicCatalog::PlanMergeInto(ClientContext &context, PhysicalPlanGenerator &planner,
                                                 LogicalMergeInto &op, PhysicalOperator &plan) {
	// Give each UPDATE action the `id` its INSERT action would have written, so a
	// tempid in an upserted row is bound to the existing entity the row merged
	// into
	optional_ptr<BoundMergeIntoAction> insert_action;
	for (auto &entry : op.actions) {
		for (auto &action : entry.second) {
			if (action->action_type == MergeActionType::MERGE_INSERT && action->column_index_map.empty() &&
			    !action->expressions.empty()) {
				insert_action = action.get();
			}
		}
	}
	if (insert_action && op.actions.find(MergeActionCondition::WHEN_MATCHED) == op.actions.end()) {
		// ON CONFLICT DO NOTHING: matched rows have no action, but their tempids
		// must still be bound
		auto bind_action = make_uniq<BoundMergeIntoAction>();
		bind_action->action_type = MergeActionType::MERGE_UPDATE;
		op.actions[MergeActionCondition::WHEN_MATCHED].push_back(std::move(bind_action));
	}
	if (insert_action) {
		for (auto &entry : op.actions) {
			for (auto &action : entry.second) {
				// A matched DO NOTHING still has to bind the row's tempid: make it an
				// UPDATE of `id` alone, which changes no datom
				if (action->action_type == MergeActionType::MERGE_DO_NOTHING &&
				    entry.first == MergeActionCondition::WHEN_MATCHED) {
					action->action_type = MergeActionType::MERGE_UPDATE;
				}
				if (action->action_type != MergeActionType::MERGE_UPDATE) {
					continue;
				}
				bool has_id = false;
				for (auto &column : action->columns) {
					has_id = has_id || column.index == MNEMONIC_ID_COLUMN_INDEX;
				}
				if (!has_id) {
					action->columns.emplace_back(MNEMONIC_ID_COLUMN_INDEX);
					action->expressions.push_back(insert_action->expressions[MNEMONIC_ID_COLUMN_INDEX]->Copy());
				}
			}
		}
	}
	return Catalog::PlanMergeInto(context, planner, op, plan);
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
