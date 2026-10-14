#include "mnemonicdb/mnemonic_transaction.hpp"
#include "mnemonicdb/mnemonic_assertions.hpp"
#include "mnemonicdb/mnemonic_catalog.hpp"
#include "mnemonicdb/mnemonic_schema_entry.hpp"
#include "mnemonicdb/mnemonic_table_entry.hpp"
#include "mnemonicdb/mnemonic_transaction_manager.hpp"

#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"
#include "duckdb/catalog/default/default_schemas.hpp"
#include "duckdb/function/cast_rules.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"

#include "rocksdb/db.h"

#include <set>

namespace duckdb {

using mnemonicdb::EntityId;

MnemonicTransaction::MnemonicTransaction(MnemonicTransactionManager &manager, ClientContext &context,
                                         MnemonicCatalog &catalog_p)
    : Transaction(manager, context), catalog(catalog_p), store(catalog_p.GetStore()), basis_tx(0), snapshot(nullptr),
      view(store, nullptr) {
	TakeSnapshot();
}

MnemonicTransaction::MnemonicTransaction(MnemonicTransactionManager &manager, ClientContext &context,
                                         MnemonicCatalog &catalog_p, MnemonicTransaction &parent_p)
    : Transaction(manager, context), catalog(catalog_p), store(catalog_p.GetStore()), basis_tx(parent_p.basis_tx),
      snapshot(parent_p.snapshot),
      view(store, parent_p.snapshot, parent_p.writer ? &parent_p.writer->GetBatch() : nullptr),
      generation(parent_p.generation), parent(&parent_p) {
}

void MnemonicTransaction::TakeSnapshot() {
	lock_guard<mutex> guard(catalog.GetSnapshotLock());
	view.ClearCache();
	if (snapshot) {
		store.GetDB().ReleaseSnapshot(snapshot);
	}
	basis_tx = store.GetLastTx();
	snapshot = store.GetDB().GetSnapshot();
	view = mnemonicdb::ReadView(store, snapshot);
	generation = catalog.GetGeneration();
}

MnemonicStatement::MnemonicStatement(MnemonicTransaction &transaction_p) : transaction(transaction_p) {
	auto &writer = transaction.GetWriter();
	counters = transaction.store.SaveCounters();
	tempids = transaction.tempids;
	generation = transaction.generation;
	schema_changed = transaction.schema_changed;
	writer.BeginStatement();
}

MnemonicStatement::~MnemonicStatement() {
	if (done) {
		return;
	}
	try {
		transaction.GetWriter().RollbackStatement();
		transaction.store.RestoreCounters(counters);
		transaction.tempids = std::move(tempids);
		transaction.generation = std::move(generation);
		transaction.schema_changed = schema_changed;
	} catch (...) { // NOLINT: a destructor must not throw; the transaction is
		            // aborted anyway
	}
}

void MnemonicStatement::Commit() {
	auto &writer = transaction.GetWriter();
	writer.FinishStatement();
	writer.PopStatement();
	done = true;
}

MnemonicTransaction::~MnemonicTransaction() {
	if (writer) {
		// Not committed: give back the IDs this transaction allocated
		store.RestoreCounters(counters_at_start);
	}
	writer.reset();
	ReleaseWriter();
	view.ClearCache();
	if (!parent) {
		// A borrowed transaction's snapshot belongs to its parent
		store.GetDB().ReleaseSnapshot(snapshot);
	}
}

MnemonicTransaction &MnemonicTransaction::Get(ClientContext &context, Catalog &catalog) {
	return Transaction::Get(context, catalog).Cast<MnemonicTransaction>();
}

void MnemonicTransaction::ReleaseWriter() {
	if (holds_writer) {
		holds_writer = false;
		store.ReleaseWriter();
	}
}

void MnemonicTransaction::SetReadWrite() {
	if (parent) {
		throw TransactionException("MnemonicDB: commit assertions cannot write");
	}
	Transaction::SetReadWrite();
	auto weak_context = context;
	store.AcquireWriter([weak_context]() {
		auto client = weak_context.lock();
		if (client) {
			client->InterruptCheck();
		}
	});
	holds_writer = true;
	// A transaction that has not read any data (e.g. an autocommit INSERT ...
	// VALUES, or a statement whose scans all run after this point) can simply
	// move to the latest state, provided the schema it was bound against is
	// unchanged
	if (store.GetLastTx() != basis_tx && !read_data && !schema_changed &&
	    catalog.GetGeneration().get() == generation.get()) {
		TakeSnapshot();
	}
	if (store.GetLastTx() != basis_tx) {
		auto last = store.GetLastTx();
		ReleaseWriter();
		throw TransactionException("MnemonicDB: transaction conflict: this transaction read the database "
		                           "as of tx %d, "
		                           "but tx %d has committed since. Retry the transaction.",
		                           basis_tx, last);
	}
	counters_at_start = store.SaveCounters();
	writer = make_uniq<mnemonicdb::TxWriter>(store, snapshot, basis_tx + 1);
}

mnemonicdb::ReadView &MnemonicTransaction::GetReadView() {
	return writer ? writer->GetView() : view;
}

mnemonicdb::TxId MnemonicTransaction::PendingTx() const {
	if (writer) {
		return writer->GetTx();
	}
	return parent ? parent->PendingTx() : 0;
}

mnemonicdb::TxWriter &MnemonicTransaction::GetWriter() {
	if (!writer) {
		throw InternalException("MnemonicDB: writing in a read-only transaction");
	}
	return *writer;
}

const mnemonicdb::Schema &MnemonicTransaction::GetSchema() const {
	return *generation->GetSchema();
}

optional_ptr<const EntityId> MnemonicTransaction::FindTempid(EntityId placeholder) const {
	auto entry = tempids.find(placeholder);
	return entry == tempids.end() ? nullptr : &entry->second;
}

void MnemonicTransaction::BindTempid(EntityId placeholder, EntityId id) {
	tempids[placeholder] = id;
}

//===--------------------------------------------------------------------===//
// DDL
//===--------------------------------------------------------------------===//
optional_ptr<CatalogEntry> MnemonicTransaction::CreateTable(MnemonicSchemaEntry &schema_entry,
                                                            BoundCreateTableInfo &bound_info) {
	auto &info = bound_info.Base();
	auto schema_name = schema_entry.name.GetIdentifierName();
	auto table_name = info.GetTableName().GetIdentifierName();
	auto &schema = GetSchema();
	if (schema_name == "sys") {
		throw CatalogException("MnemonicDB: schema \"sys\" is reserved: tables cannot be created in it");
	}
	if (!schema.FindSchema(schema_name)) {
		throw CatalogException("MnemonicDB: schema \"%s\" does not exist", schema_name);
	}
	if (auto existing = schema.FindTable(schema_name, table_name)) {
		switch (info.on_conflict) {
		case OnCreateConflict::IGNORE_ON_CONFLICT:
			return nullptr;
		case OnCreateConflict::REPLACE_ON_CONFLICT:
			throw NotImplementedException("MnemonicDB does not support CREATE OR REPLACE TABLE");
		default:
			throw CatalogException::EntryAlreadyExists(CatalogType::TABLE_ENTRY, info.GetTableName());
		}
	}
	if (info.temporary) {
		throw NotImplementedException("MnemonicDB does not support temporary tables");
	}

	// Column constraints: UNIQUE -> unique value, PRIMARY KEY -> unique identity
	unordered_map<idx_t, mnemonicdb::Uniqueness> uniqueness;
	unordered_set<idx_t> not_null;
	for (auto &constraint : info.constraints) {
		switch (constraint->type) {
		case ConstraintType::UNIQUE: {
			auto &unique = constraint->Cast<UniqueConstraint>();
			auto indexes = unique.GetLogicalIndexes(info.columns);
			if (indexes.size() != 1) {
				throw NotImplementedException("MnemonicDB does not support multi-column UNIQUE or PRIMARY KEY");
			}
			uniqueness[indexes[0].index] =
			    unique.IsPrimaryKey() ? mnemonicdb::Uniqueness::IDENTITY : mnemonicdb::Uniqueness::VALUE;
			break;
		}
		case ConstraintType::NOT_NULL:
			not_null.insert(constraint->Cast<NotNullConstraint>().index.index);
			break;
		default:
			throw NotImplementedException("MnemonicDB does not support %s constraints",
			                              EnumUtil::ToString(constraint->type));
		}
	}
	for (auto index : not_null) {
		auto entry = uniqueness.find(index);
		if (entry == uniqueness.end() || entry->second != mnemonicdb::Uniqueness::IDENTITY) {
			throw NotImplementedException("MnemonicDB does not support NOT NULL: a "
			                              "NULL column is an absent attribute");
		}
	}

	auto &writer = GetWriter();
	MnemonicStatement statement(*this);
	mnemonicdb::TableDef table;
	table.id = store.AllocateId(mnemonicdb::partition::TABLE);
	table.schema = schema_name;
	table.name = table_name;
	vector<mnemonicdb::AttributeDef> attributes;
	for (auto &column : info.columns.Logical()) {
		auto unique = uniqueness.find(column.Logical().index);
		attributes.push_back(
		    NewAttribute(table.id, column, unique == uniqueness.end() ? mnemonicdb::Uniqueness::NONE : unique->second));
	}

	mnemonicdb::Schema::WriteTable(writer, schema, table);
	for (auto &attr : attributes) {
		mnemonicdb::Schema::WriteAttribute(writer, schema, attr);
	}
	SetSchema(schema.WithTable(table, attributes));
	statement.Commit();
	return generation->GetTable(schema_name, table_name).get();
}

static void CheckColumnName(const string &name) {
	if (StringUtil::CIEquals(name, "id") || StringUtil::CIEquals(name, "rowid")) {
		throw BinderException("MnemonicDB: column name \"%s\" is reserved", name);
	}
}

mnemonicdb::AttributeDef MnemonicTransaction::NewAttribute(EntityId table, const ColumnDefinition &column,
                                                           mnemonicdb::Uniqueness unique) {
	auto &name = column.Name().GetIdentifierName();
	CheckColumnName(name);
	if (column.Generated()) {
		throw NotImplementedException("MnemonicDB does not support generated columns");
	}
	if (column.HasDefaultValue()) {
		throw NotImplementedException("MnemonicDB does not support column defaults");
	}
	CheckNamedTypes(column.Type());
	return mnemonicdb::AttributeDef(store.AllocateId(mnemonicdb::partition::ATTR), table, name, column.Type(), unique,
	                                false);
}

//! Whether values of `from` can be converted to `to` without merging distinct
//! values
static bool IsSafeTypeChange(const LogicalType &from, const LogicalType &to) {
	if (from == to) {
		return true;
	}
	bool from_list = from.id() == LogicalTypeId::LIST;
	if (from_list != (to.id() == LogicalTypeId::LIST)) {
		return false;
	}
	if (from_list) {
		return IsSafeTypeChange(ListType::GetChildType(from), ListType::GetChildType(to));
	}
	if (mnemonicdb::IsRefType(from) || mnemonicdb::IsRefType(to)) {
		return false;
	}
	if (to.id() == LogicalTypeId::VARCHAR) {
		return true;
	}
	return CastRules::ImplicitCast(from, to) >= 0;
}

void MnemonicTransaction::AlterTable(const string &schema_name, AlterTableInfo &info) {
	auto &schema = GetSchema();
	auto table_name = info.GetQualifiedName().Name().GetIdentifierName();
	auto table = schema.FindTable(schema_name, table_name);
	if (!table) {
		if (info.if_not_found != OnEntryNotFound::THROW_EXCEPTION) {
			return;
		}
		throw CatalogException("MnemonicDB: table \"%s\" does not exist", table_name);
	}
	bool tx_add_column = table->id == mnemonicdb::sys::TX && info.alter_table_type == AlterTableType::ADD_COLUMN;
	bool tx_rename_user_column = false;
	if (table->id == mnemonicdb::sys::TX && info.alter_table_type == AlterTableType::RENAME_COLUMN) {
		auto attr = schema.FindAttribute(*table, info.Cast<RenameColumnInfo>().old_name.GetIdentifierName());
		tx_rename_user_column = attr && attr->Local() >= 64;
	}
	if (table->schema == "sys" && !tx_add_column && !tx_rename_user_column) {
		throw NotImplementedException("MnemonicDB: sys tables cannot be altered, "
		                              "except ADD COLUMN and RENAME COLUMN of a user column on sys.tx");
	}
	auto &writer = GetWriter();
	MnemonicStatement statement(*this);
	auto find_column = [&](const string &name) -> const mnemonicdb::AttributeDef & {
		auto attr = schema.FindAttribute(*table, name);
		if (!attr) {
			throw BinderException("MnemonicDB: table \"%s\" has no column \"%s\"", table->name, name);
		}
		return *attr;
	};

	switch (info.alter_table_type) {
	case AlterTableType::RENAME_TABLE: {
		auto &rename = info.Cast<RenameTableInfo>();
		auto new_name = rename.new_table_name.GetIdentifierName();
		if (schema.FindTable(schema_name, new_name)) {
			throw CatalogException::EntryAlreadyExists(CatalogType::TABLE_ENTRY, rename.new_table_name);
		}
		writer.Assert(schema.GetAttribute(mnemonicdb::sys::TABLES_NAME), table->id,
		              schema.GetAttribute(mnemonicdb::sys::TABLES_NAME).codec.EncodeValue(Value(new_name)));
		SetSchema(schema.WithTableName(table->id, new_name));
		break;
	}
	case AlterTableType::RENAME_COLUMN: {
		auto &rename = info.Cast<RenameColumnInfo>();
		auto &attr = find_column(rename.old_name.GetIdentifierName());
		auto new_name = rename.new_name.GetIdentifierName();
		CheckColumnName(new_name);
		if (schema.FindAttribute(*table, new_name) && !StringUtil::CIEquals(new_name, attr.name)) {
			throw CatalogException("MnemonicDB: table \"%s\" already has a column \"%s\"", table->name, new_name);
		}
		auto &sys_name = schema.GetAttribute(mnemonicdb::sys::ATTRIBUTES_NAME);
		writer.Assert(sys_name, attr.id, sys_name.codec.EncodeValue(Value(new_name)));
		if (tx_rename_user_column) {
			// Renaming a user column of sys.tx may un-shadow a built-in column of
			// the same name: re-read the schema
			auto client = context.lock();
			if (!client) {
				throw InternalException("MnemonicDB: no client context");
			}
			SetSchema(mnemonicdb::Schema::Load(writer.GetView(), *client));
			break;
		}
		auto renamed = attr;
		renamed.name = new_name;
		SetSchema(schema.WithAttribute(std::move(renamed)));
		break;
	}
	case AlterTableType::ADD_COLUMN: {
		auto &add = info.Cast<AddColumnInfo>();
		if (add.add_not_null) {
			throw NotImplementedException("MnemonicDB does not support NOT NULL: a "
			                              "NULL column is an absent attribute");
		}
		auto &name = add.new_column.Name().GetIdentifierName();
		if (schema.FindAttribute(*table, name) || StringUtil::CIEquals(name, "id")) {
			if (add.if_column_not_exists) {
				statement.Commit();
				return;
			}
			throw CatalogException("MnemonicDB: table \"%s\" already has a column \"%s\"", table->name, name);
		}
		auto attr = NewAttribute(table->id, add.new_column, mnemonicdb::Uniqueness::NONE);
		mnemonicdb::Schema::WriteAttribute(writer, schema, attr);
		SetSchema(schema.WithNewAttribute(std::move(attr)));
		break;
	}
	case AlterTableType::REMOVE_COLUMN: {
		auto &remove = info.Cast<RemoveColumnInfo>();
		auto name = remove.removed_column.GetIdentifierName();
		auto attr_ptr = schema.FindAttribute(*table, name);
		if (!attr_ptr) {
			if (remove.if_column_exists) {
				statement.Commit();
				return;
			}
			find_column(name);
		}
		auto &attr = *attr_ptr;
		// Retract the column's values (they stay in history), then detach the
		// attribute from the table
		vector<EntityId> entities;
		for (auto &datom :
		     writer.GetView().ScanPrefix(mnemonicdb::ColumnFamily::AEVT, mnemonicdb::AevtPrefix(attr.Local()))) {
			if (entities.empty() || entities.back() != datom.e) {
				entities.push_back(datom.e);
			}
		}
		for (auto entity : entities) {
			writer.RetractAll(attr, entity);
		}
		for (auto sys_attr : {mnemonicdb::sys::ATTRIBUTES_TABLE, mnemonicdb::sys::ATTRIBUTES_INDEX,
		                      mnemonicdb::sys::ATTRIBUTES_INDEXED}) {
			writer.RetractAll(schema.GetAttribute(sys_attr), attr.id);
		}
		SetSchema(schema.WithoutAttribute(attr.Local()));
		break;
	}
	case AlterTableType::ALTER_COLUMN_TYPE: {
		auto &change = info.Cast<ChangeColumnTypeInfo>();
		auto &attr = find_column(change.column_name.GetIdentifierName());
		if (change.expression && change.expression->GetExpressionClass() != ExpressionClass::CAST) {
			throw NotImplementedException("MnemonicDB does not support ALTER COLUMN TYPE ... USING");
		}
		CheckNamedTypes(change.target_type);
		if (!IsSafeTypeChange(attr.type, mnemonicdb::StripNamedTypeAliases(change.target_type))) {
			throw NotImplementedException("MnemonicDB cannot change column \"%s\" from %s to %s: only "
			                              "conversions "
			                              "that keep distinct values distinct are allowed",
			                              attr.name, attr.type.ToString(), change.target_type.ToString());
		}
		mnemonicdb::AttributeDef changed(attr.id, attr.table, attr.name, change.target_type, attr.unique, attr.indexed,
		                                 attr.index_name);
		auto &old_codec = attr.codec;
		auto &new_codec = changed.codec;
		auto &target = changed.value_type;
		writer.RewriteAttribute(attr, changed, [&](std::string_view v) {
			return new_codec.EncodeValue(old_codec.DecodeValue(v).DefaultCastAs(target, true));
		});
		MarkReencoded(attr);
		auto &sys_type = schema.GetAttribute(mnemonicdb::sys::ATTRIBUTES_TYPE);
		writer.Assert(sys_type, attr.id, sys_type.codec.EncodeValue(Value(changed.declared.ToString())));
		SetSchema(schema.WithAttribute(std::move(changed)));
		break;
	}
	default:
		throw NotImplementedException("MnemonicDB does not support ALTER TABLE %s",
		                              EnumUtil::ToString(info.alter_table_type));
	}
	statement.Commit();
}

mnemonicdb::EntityId MnemonicTransaction::CurrentTxEntity() {
	return mnemonicdb::TxEntity(GetWriter().GetTx());
}

void MnemonicTransaction::ReloadSchema(ClientContext &context) {
	auto &writer = GetWriter();
	auto &old_schema = GetSchema();
	for (auto &datom : writer.GetView().ScanPrefix(mnemonicdb::ColumnFamily::AEVT,
	                                               mnemonicdb::AevtPrefix(mnemonicdb::sys::TABLES_SCHEMA))) {
		auto schema_name =
		    old_schema.GetAttribute(mnemonicdb::sys::TABLES_SCHEMA).codec.DecodeValue(datom.v).ToString();
		if (!old_schema.FindSchema(schema_name)) {
			throw InvalidInputException("MnemonicDB: unknown schema \"%s\"; create it with CREATE SCHEMA", schema_name);
		}
	}
	auto new_schema = mnemonicdb::Schema::Load(writer.GetView(), context);
	for (auto table : new_schema->TablesInSchema("sys")) {
		if (!old_schema.FindTable(table->id)) {
			throw InvalidInputException("MnemonicDB: schema \"sys\" is reserved: user tables cannot be created in it");
		}
	}
	for (auto old_attr : old_schema.AllAttributes()) {
		if (old_attr->Local() < 64) {
			// Built-ins cannot change (and may be shadowed, see Schema::AddAttribute)
			continue;
		}
		auto new_table = new_schema->FindTable(old_attr->table);
		bool still_there = false;
		if (new_table) {
			for (auto local : new_table->attributes) {
				still_there = still_there || local == old_attr->Local();
			}
		}
		if (!still_there) {
			throw NotImplementedException("MnemonicDB: attribute \"%s\" cannot be "
			                              "removed through sys.attributes; "
			                              "use ALTER TABLE ... DROP COLUMN",
			                              old_attr->name);
		}
		auto &new_attr = new_schema->GetAttribute(old_attr->Local());
		if (new_attr.declared != old_attr->declared) {
			throw NotImplementedException("MnemonicDB: the type of \"%s\" cannot be changed through "
			                              "sys.attributes; use ALTER TABLE ... TYPE",
			                              old_attr->name);
		}
		if (new_attr.unique != mnemonicdb::Uniqueness::NONE && old_attr->unique == mnemonicdb::Uniqueness::NONE) {
			std::set<string> seen;
			for (auto &datom : writer.GetView().ScanPrefix(mnemonicdb::ColumnFamily::AEVT,
			                                               mnemonicdb::AevtPrefix(new_attr.Local()))) {
				if (!seen.insert(datom.v).second) {
					throw ConstraintException("MnemonicDB: \"%s\" cannot become unique: "
					                          "two entities share a value",
					                          new_attr.name);
				}
			}
		}
		if (new_attr.HasAvet() != old_attr->HasAvet()) {
			writer.RebuildAvet(new_attr, !new_attr.HasAvet());
		}
	}
	SetSchema(std::move(new_schema));
}

void MnemonicTransaction::SetSchema(shared_ptr<const mnemonicdb::Schema> schema) {
	generation = make_shared_ptr<MnemonicCatalogGeneration>(catalog, std::move(schema));
	schema_changed = true;
}

void MnemonicTransaction::CreateIndex(MnemonicTableEntry &table_entry, const string &index_name, const string &column,
                                      bool if_not_exists) {
	auto &schema = GetSchema();
	auto table = schema.FindTable(table_entry.GetTableDef().id);
	if (!table) {
		throw CatalogException("MnemonicDB: table \"%s\" no longer exists", table_entry.name);
	}
	if (StringUtil::CIEquals(column, "id")) {
		throw BinderException("MnemonicDB: the id column needs no index: it is the entity itself");
	}
	auto attr = schema.FindAttribute(*table, column);
	if (!attr) {
		throw BinderException("MnemonicDB: table \"%s\" has no column \"%s\"", table->name, column);
	}
	if (schema.FindIndex(table->schema, index_name)) {
		if (if_not_exists) {
			return;
		}
		throw CatalogException::EntryAlreadyExists(CatalogType::INDEX_ENTRY, Identifier(index_name));
	}
	if (!attr->index_name.empty()) {
		throw CatalogException("MnemonicDB: column \"%s\" is already indexed by \"%s\"", attr->name, attr->index_name);
	}
	auto &writer = GetWriter();
	MnemonicStatement statement(*this);
	auto indexed = *attr;
	indexed.indexed = true;
	indexed.index_name = index_name;
	if (!attr->HasAvet()) {
		writer.RebuildAvet(indexed, false);
	}
	mnemonicdb::Schema::WriteAttribute(writer, schema, indexed);
	SetSchema(schema.WithAttribute(std::move(indexed)));
	statement.Commit();
}

void MnemonicTransaction::DropIndex(const string &schema_name, const string &index_name, bool if_exists) {
	auto &schema = GetSchema();
	auto attr = schema.FindIndex(schema_name, index_name);
	if (!attr) {
		if (if_exists) {
			return;
		}
		throw CatalogException("MnemonicDB: index \"%s\" does not exist", index_name);
	}
	auto &writer = GetWriter();
	MnemonicStatement statement(*this);
	auto &sys_indexed = schema.GetAttribute(mnemonicdb::sys::ATTRIBUTES_INDEXED);
	auto &sys_index = schema.GetAttribute(mnemonicdb::sys::ATTRIBUTES_INDEX);
	writer.RetractAll(sys_indexed, attr->id);
	writer.RetractAll(sys_index, attr->id);
	auto dropped = *attr;
	dropped.indexed = false;
	dropped.index_name.clear();
	if (!dropped.HasAvet()) {
		writer.RebuildAvet(*attr, true);
	}
	SetSchema(schema.WithAttribute(std::move(dropped)));
	statement.Commit();
}

//===--------------------------------------------------------------------===//
// Schemas and named types
//===--------------------------------------------------------------------===//
optional_ptr<CatalogEntry> MnemonicTransaction::CreateSchema(CreateSchemaInfo &info) {
	auto schema_name = info.SchemaName().GetIdentifierName();
	auto &schema = GetSchema();
	if (StringUtil::CIEquals(schema_name, "sys") && info.on_conflict != OnCreateConflict::IGNORE_ON_CONFLICT) {
		throw CatalogException("MnemonicDB: schema \"sys\" is reserved");
	}
	if (schema.FindSchema(schema_name)) {
		switch (info.on_conflict) {
		case OnCreateConflict::IGNORE_ON_CONFLICT:
			return nullptr;
		case OnCreateConflict::REPLACE_ON_CONFLICT:
			throw NotImplementedException("MnemonicDB does not support CREATE OR REPLACE SCHEMA");
		default:
			throw CatalogException::EntryAlreadyExists(CatalogType::SCHEMA_ENTRY, info.SchemaName());
		}
	}
	if (DefaultSchemaGenerator::IsDefaultSchema(info.SchemaName()) || StringUtil::CIEquals(schema_name, "temp")) {
		throw CatalogException("MnemonicDB: schema name \"%s\" is reserved", schema_name);
	}
	auto &writer = GetWriter();
	MnemonicStatement statement(*this);
	mnemonicdb::SchemaDef def;
	def.id = store.AllocateId(mnemonicdb::partition::SCHEMA);
	def.name = schema_name;
	if (!info.comment.IsNull()) {
		def.doc = info.comment.ToString();
	}
	mnemonicdb::Schema::WriteSchema(writer, schema, def);
	SetSchema(schema.WithSchema(std::move(def)));
	statement.Commit();
	return generation->GetSchemaEntry(schema_name).get();
}

void MnemonicTransaction::DropSchema(const string &schema_name, bool if_exists, bool cascade) {
	auto &schema = GetSchema();
	auto def = schema.FindSchema(schema_name);
	if (!def) {
		if (if_exists) {
			return;
		}
		throw CatalogException("MnemonicDB: schema \"%s\" does not exist", schema_name);
	}
	if (def->id == mnemonicdb::sys::MAIN_SCHEMA || def->id == mnemonicdb::sys::SYS_SCHEMA) {
		throw CatalogException("MnemonicDB: schema \"%s\" is built in and cannot be dropped", def->name);
	}
	if (cascade) {
		throw NotImplementedException("MnemonicDB does not support DROP SCHEMA ... CASCADE: only an empty schema "
		                              "can be dropped");
	}
	auto tables = schema.TablesInSchema(def->name).size();
	auto types = schema.TypesInSchema(def->name).size();
	if (tables > 0 || types > 0) {
		throw CatalogException("MnemonicDB: schema \"%s\" is not empty (%d tables, %d types): only an empty schema "
		                       "can be dropped",
		                       def->name, tables, types);
	}
	auto &writer = GetWriter();
	MnemonicStatement statement(*this);
	for (auto sys_attr : {mnemonicdb::sys::SCHEMAS_NAME, mnemonicdb::sys::SCHEMAS_DOC}) {
		writer.RetractAll(schema.GetAttribute(sys_attr), def->id);
	}
	SetSchema(schema.WithoutSchema(def->name));
	statement.Commit();
}

//! Visit every named-type alias in `type`
static void ForEachAlias(const LogicalType &type, const std::function<void(const LogicalType &)> &callback) {
	if (type.HasAlias()) {
		callback(type);
	}
	switch (type.id()) {
	case LogicalTypeId::LIST:
		ForEachAlias(ListType::GetChildType(type), callback);
		break;
	case LogicalTypeId::ARRAY:
		ForEachAlias(ArrayType::GetChildType(type), callback);
		break;
	case LogicalTypeId::MAP:
		ForEachAlias(MapType::KeyType(type), callback);
		ForEachAlias(MapType::ValueType(type), callback);
		break;
	case LogicalTypeId::STRUCT:
		for (auto &child : StructType::GetChildTypes(type)) {
			ForEachAlias(child.second, callback);
		}
		break;
	case LogicalTypeId::UNION:
		for (idx_t i = 0; i < UnionType::GetMemberCount(type); i++) {
			ForEachAlias(UnionType::GetMemberType(type, i), callback);
		}
		break;
	default:
		break;
	}
}

void MnemonicTransaction::CheckNamedTypes(const LogicalType &type) const {
	auto &schema = GetSchema();
	ForEachAlias(type, [&](const LogicalType &aliased) {
		// Named types carry a schema-qualified alias; one-part aliases are
		// extension types such as REF
		if (!mnemonicdb::IsNamedTypeAlias(aliased.GetAlias())) {
			return;
		}
		auto components = QualifiedName::ParseComponents(aliased.GetAlias());
		auto def = schema.FindType(components[0].GetIdentifierName(), components[1].GetIdentifierName());
		if (mnemonicdb::NamedTypeDatabase(aliased) != mnemonicdb::DatabaseTag(store) || !def || def->type != aliased) {
			throw BinderException("MnemonicDB: type \"%s\" is not a type of this database; a column can only use "
			                      "named types from its own database",
			                      aliased.GetAlias());
		}
	});
}

//! Whether values of `a` and `b` have the same stored encoding: the types differ
//! at most in ENUM dictionaries whose physical width is the same
static bool SameEncoding(const LogicalType &a, const LogicalType &b) {
	if (a.id() != b.id() || a.InternalType() != b.InternalType()) {
		return false;
	}
	switch (a.id()) {
	case LogicalTypeId::LIST:
		return SameEncoding(ListType::GetChildType(a), ListType::GetChildType(b));
	case LogicalTypeId::ARRAY:
		return ArrayType::GetSize(a) == ArrayType::GetSize(b) &&
		       SameEncoding(ArrayType::GetChildType(a), ArrayType::GetChildType(b));
	case LogicalTypeId::MAP:
		return SameEncoding(MapType::KeyType(a), MapType::KeyType(b)) &&
		       SameEncoding(MapType::ValueType(a), MapType::ValueType(b));
	case LogicalTypeId::STRUCT: {
		auto &a_children = StructType::GetChildTypes(a);
		auto &b_children = StructType::GetChildTypes(b);
		if (a_children.size() != b_children.size()) {
			return false;
		}
		for (idx_t i = 0; i < a_children.size(); i++) {
			if (!SameEncoding(a_children[i].second, b_children[i].second)) {
				return false;
			}
		}
		return true;
	}
	case LogicalTypeId::UNION: {
		auto count = UnionType::GetMemberCount(a);
		if (count != UnionType::GetMemberCount(b)) {
			return false;
		}
		for (idx_t i = 0; i < count; i++) {
			if (!SameEncoding(UnionType::GetMemberType(a, i), UnionType::GetMemberType(b, i))) {
				return false;
			}
		}
		return true;
	}
	default:
		return true;
	}
}

optional_ptr<CatalogEntry> MnemonicTransaction::CreateType(const string &schema_name, CreateTypeInfo &info) {
	auto &schema = GetSchema();
	auto type_name = info.GetTypeName().GetIdentifierName();
	if (schema_name == "sys") {
		throw CatalogException("MnemonicDB: schema \"sys\" is reserved: types cannot be created in it");
	}
	if (!schema.FindSchema(schema_name)) {
		throw CatalogException("MnemonicDB: schema \"%s\" does not exist", schema_name);
	}
	if (info.temporary) {
		throw NotImplementedException("MnemonicDB does not support temporary types");
	}
	if (mnemonicdb::IsRefType(info.type)) {
		throw NotImplementedException("MnemonicDB: a named type cannot be REF");
	}
	// A named type defined as another named type is a copy of its definition
	auto type = mnemonicdb::WithoutNamedTypeTag(info.type);
	CheckNamedTypes(type);
	// Throws for types the store cannot encode
	mnemonicdb::ValueCodec codec(type.id() == LogicalTypeId::LIST ? ListType::GetChildType(type) : type);

	if (auto existing = schema.FindType(schema_name, type_name)) {
		switch (info.on_conflict) {
		case OnCreateConflict::IGNORE_ON_CONFLICT:
			return nullptr;
		case OnCreateConflict::REPLACE_ON_CONFLICT:
			ReplaceType(*existing, type, info.comment.IsNull() ? existing->doc : info.comment.ToString());
			return generation->GetType(schema_name, type_name);
		default:
			throw CatalogException::EntryAlreadyExists(CatalogType::TYPE_ENTRY, info.GetTypeName());
		}
	}

	auto &writer = GetWriter();
	MnemonicStatement statement(*this);
	mnemonicdb::TypeDef def;
	def.id = store.AllocateId(mnemonicdb::partition::SCHEMA);
	def.schema = schema.FindSchema(schema_name)->name;
	def.name = type_name;
	def.definition = type.ToString();
	if (!info.comment.IsNull()) {
		def.doc = info.comment.ToString();
	}
	def.type = mnemonicdb::MakeNamedType(type, def.schema, def.name, mnemonicdb::DatabaseTag(store));
	mnemonicdb::Schema::WriteType(writer, schema, def);
	SetSchema(schema.WithType(std::move(def)));
	statement.Commit();
	return generation->GetType(schema_name, type_name);
}

void MnemonicTransaction::MarkReencoded(const mnemonicdb::AttributeDef &attr) {
	auto &marker = GetSchema().GetAttribute(mnemonicdb::sys::TX_REENCODED);
	GetWriter().Assert(marker, CurrentTxEntity(), marker.codec.EncodeValue(Value::UBIGINT(attr.id)));
}

vector<mnemonicdb::AttributeDef> MnemonicTransaction::DroppedAttributes(const mnemonicdb::Schema &schema,
                                                                        ClientContext &context) {
	vector<mnemonicdb::AttributeDef> result;
	auto &view = GetReadView();
	auto &sys_type = schema.GetAttribute(mnemonicdb::sys::ATTRIBUTES_TYPE);
	auto &sys_name = schema.GetAttribute(mnemonicdb::sys::ATTRIBUTES_NAME);
	for (auto &datom :
	     view.ScanPrefix(mnemonicdb::ColumnFamily::AEVT, mnemonicdb::AevtPrefix(mnemonicdb::sys::ATTRIBUTES_TYPE))) {
		auto local = static_cast<mnemonicdb::AttrId>(mnemonicdb::LocalOf(datom.e));
		if (local < 64 || schema.FindAttributeById(local)) {
			continue;
		}
		auto names = view.ScanPrefix(mnemonicdb::ColumnFamily::EAVT,
		                             mnemonicdb::EavtPrefix(datom.e, mnemonicdb::sys::ATTRIBUTES_NAME));
		auto name = names.empty() ? string() : sys_name.codec.DecodeValue(names[0].v).ToString();
		// An index's AVET entries moved to avet_hist when the column was dropped
		bool had_avet = !view.ScanPrefix(mnemonicdb::ColumnFamily::AVET_HIST, mnemonicdb::AvetPrefix(local)).empty();
		result.emplace_back(datom.e, 0, name, schema.ParseType(sys_type.codec.DecodeValue(datom.v).ToString(), context),
		                    mnemonicdb::Uniqueness::NONE, had_avet);
	}
	return result;
}

void MnemonicTransaction::ReplaceType(const mnemonicdb::TypeDef &existing, const LogicalType &type, const string &doc) {
	auto old_type = mnemonicdb::WithoutNamedTypeTag(existing.type);
	if (old_type == type) {
		if (doc != existing.doc) {
			auto &writer = GetWriter();
			MnemonicStatement statement(*this);
			auto &schema = GetSchema();
			auto &sys_doc = schema.GetAttribute(mnemonicdb::sys::TYPES_DOC);
			writer.Assert(sys_doc, existing.id, sys_doc.codec.EncodeValue(Value(doc)));
			auto changed = existing;
			changed.doc = doc;
			SetSchema(schema.WithoutType(existing.id)->WithType(std::move(changed)));
			statement.Commit();
		}
		return;
	}
	auto qualified = existing.schema + "." + existing.name;
	if (old_type.id() != LogicalTypeId::ENUM || type.id() != LogicalTypeId::ENUM) {
		throw NotImplementedException("MnemonicDB: type \"%s\" cannot be redefined: the only supported change to a "
		                              "named type is appending values to an ENUM",
		                              qualified);
	}
	auto old_size = EnumType::GetSize(old_type);
	auto new_size = EnumType::GetSize(type);
	for (idx_t i = 0; i < old_size; i++) {
		auto old_value = EnumType::GetString(old_type, i).GetString();
		if (i >= new_size) {
			throw NotImplementedException("MnemonicDB: ENUM type \"%s\" can only be extended by appending values, "
			                              "but the new definition removes '%s'",
			                              qualified, old_value);
		}
		auto new_value = EnumType::GetString(type, i).GetString();
		if (old_value != new_value) {
			throw NotImplementedException("MnemonicDB: ENUM type \"%s\" can only be extended by appending values: "
			                              "existing values must keep their positions, but value %d is '%s' and was "
			                              "'%s' (values cannot be reordered, renamed or removed)",
			                              qualified, i + 1, new_value, old_value);
		}
	}
	auto client = context.lock();
	if (!client) {
		throw InternalException("MnemonicDB: no client context");
	}
	auto &old_schema = GetSchema();
	auto &writer = GetWriter();
	MnemonicStatement statement(*this);
	// Columns dropped earlier keep their values in history, decoded with the
	// type they were declared with: they are re-encoded too
	// Only a width change re-encodes anything, so only then are they read
	vector<mnemonicdb::AttributeDef> old_dropped;
	if (old_type.InternalType() != type.InternalType()) {
		old_dropped = DroppedAttributes(old_schema, *client);
	}
	auto &sys_definition = old_schema.GetAttribute(mnemonicdb::sys::TYPES_DEFINITION);
	writer.Assert(sys_definition, existing.id, sys_definition.codec.EncodeValue(Value(type.ToString())));
	if (doc != existing.doc) {
		auto &sys_doc = old_schema.GetAttribute(mnemonicdb::sys::TYPES_DOC);
		writer.Assert(sys_doc, existing.id, sys_doc.codec.EncodeValue(Value(doc)));
	}
	// Every type and attribute using the type is re-resolved by name. Old
	// dictionary indexes stay valid, so stored values only need re-encoding where
	// an ENUM grew past its physical width (e.g. 256 values)
	auto new_schema = mnemonicdb::Schema::Load(writer.GetView(), *client);
	for (auto old_attr : old_schema.AllAttributes()) {
		auto &new_attr = new_schema->GetAttribute(old_attr->Local());
		if (new_attr.type == old_attr->type || SameEncoding(old_attr->value_type, new_attr.value_type)) {
			continue;
		}
		auto &old_codec = old_attr->codec;
		auto &new_codec = new_attr.codec;
		auto &target = new_attr.value_type;
		writer.RewriteAttribute(*old_attr, new_attr, [&](std::string_view v) {
			return new_codec.EncodeValue(old_codec.DecodeValue(v).DefaultCastAs(target, true));
		});
		MarkReencoded(*old_attr);
	}
	for (auto &old_attr : old_dropped) {
		mnemonicdb::AttributeDef new_attr(old_attr.id, 0, old_attr.name,
		                                  new_schema->ParseType(old_attr.declared.ToString(), *client),
		                                  mnemonicdb::Uniqueness::NONE, old_attr.indexed);
		if (SameEncoding(old_attr.value_type, new_attr.value_type)) {
			continue;
		}
		auto &old_codec = old_attr.codec;
		auto &new_codec = new_attr.codec;
		auto &target = new_attr.value_type;
		writer.RewriteAttribute(old_attr, new_attr, [&](std::string_view v) {
			return new_codec.EncodeValue(old_codec.DecodeValue(v).DefaultCastAs(target, true));
		});
		MarkReencoded(old_attr);
	}
	SetSchema(std::move(new_schema));
	statement.Commit();
}

void MnemonicTransaction::DropType(const string &schema_name, const string &type_name, bool if_exists) {
	auto &schema = GetSchema();
	auto def = schema.FindType(schema_name, type_name);
	if (!def) {
		if (if_exists) {
			return;
		}
		throw CatalogException("MnemonicDB: type \"%s.%s\" does not exist", schema_name, type_name);
	}
	auto alias = mnemonicdb::NamedTypeAlias(def->schema, def->name);
	for (auto attr : schema.AllAttributes()) {
		if (mnemonicdb::TypeReferences(attr->declared, alias)) {
			auto table = schema.FindTable(attr->table);
			throw CatalogException("MnemonicDB: type \"%s.%s\" cannot be dropped: column \"%s.%s.%s\" uses it",
			                       def->schema, def->name, table ? table->schema : string(),
			                       table ? table->name : string(), attr->name);
		}
	}
	for (auto other : schema.AllTypes()) {
		if (other->id != def->id && mnemonicdb::TypeReferences(mnemonicdb::WithoutNamedTypeTag(other->type), alias)) {
			throw CatalogException("MnemonicDB: type \"%s.%s\" cannot be dropped: type \"%s.%s\" uses it", def->schema,
			                       def->name, other->schema, other->name);
		}
	}
	// Dropped columns' history is decoded with their declared type, so a type
	// stays as long as any column ever used it
	auto client = context.lock();
	if (!client) {
		throw InternalException("MnemonicDB: no client context");
	}
	for (auto &dropped : DroppedAttributes(schema, *client)) {
		if (mnemonicdb::TypeReferences(dropped.declared, alias)) {
			throw CatalogException("MnemonicDB: type \"%s.%s\" cannot be dropped: dropped column \"%s\" still has "
			                       "history that uses it",
			                       def->schema, def->name, dropped.name);
		}
	}
	auto &writer = GetWriter();
	MnemonicStatement statement(*this);
	for (auto sys_attr : {mnemonicdb::sys::TYPES_SCHEMA, mnemonicdb::sys::TYPES_NAME, mnemonicdb::sys::TYPES_DEFINITION,
	                      mnemonicdb::sys::TYPES_DOC}) {
		writer.RetractAll(schema.GetAttribute(sys_attr), def->id);
	}
	SetSchema(schema.WithoutType(def->id));
	statement.Commit();
}

//===--------------------------------------------------------------------===//
// Commit
//===--------------------------------------------------------------------===//
void MnemonicTransaction::Commit(ClientContext &context) {
	if (writer) {
		writer->FinishStatement();
	}
	if (writer && writer->HasChanges()) {
		// Throws if an assertion fails; the manager then discards the transaction,
		// which drops the batch, gives back IDs and releases the writer
		EvaluateAssertions(context, *this);
		auto tx = writer->GetTx();
		auto instant = MaxValue<int64_t>(Timestamp::GetCurrentTimestamp().value, store.GetLastInstant() + 1);
		mnemonicdb::Schema::WriteTxEntity(*writer, GetSchema(), tx, instant);
		lock_guard<mutex> guard(catalog.GetSnapshotLock());
		store.Commit(writer->GetWriteBatch(), tx, instant, writer->GetCountDeltas());
		if (schema_changed) {
			catalog.SetGeneration(generation);
		}
	}
	writer.reset();
	ReleaseWriter();
}

} // namespace duckdb
