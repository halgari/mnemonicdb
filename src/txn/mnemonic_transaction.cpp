#include "mnemonicdb/mnemonic_transaction.hpp"
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

void MnemonicTransaction::TakeSnapshot() {
	lock_guard<mutex> guard(catalog.GetSnapshotLock());
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
	} catch (...) { // NOLINT: a destructor must not throw; the transaction is aborted anyway
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
	store.GetDB().ReleaseSnapshot(snapshot);
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
	Transaction::SetReadWrite();
	auto weak_context = context;
	store.AcquireWriter([weak_context]() {
		auto client = weak_context.lock();
		if (client) {
			client->InterruptCheck();
		}
	});
	holds_writer = true;
	// A transaction that has not read any data (e.g. an autocommit INSERT ... VALUES, or a statement whose scans
	// all run after this point) can simply move to the latest state, provided the schema it was bound against is
	// unchanged
	if (store.GetLastTx() != basis_tx && !read_data && !schema_changed &&
	    catalog.GetGeneration().get() == generation.get()) {
		TakeSnapshot();
	}
	if (store.GetLastTx() != basis_tx) {
		auto last = store.GetLastTx();
		ReleaseWriter();
		throw TransactionException("MnemonicDB: transaction conflict: this transaction read the database as of tx %d, "
		                           "but tx %d has committed since. Retry the transaction.",
		                           basis_tx, last);
	}
	counters_at_start = store.SaveCounters();
	writer = make_uniq<mnemonicdb::TxWriter>(store, snapshot, basis_tx + 1);
}

mnemonicdb::ReadView &MnemonicTransaction::GetReadView() {
	return writer ? writer->GetView() : view;
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
	if (schema_name != "main") {
		throw CatalogException("MnemonicDB: tables can only be created in schema \"main\"");
	}
	auto &schema = GetSchema();
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
			throw NotImplementedException(
			    "MnemonicDB does not support NOT NULL: a NULL column is an absent attribute");
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
	return mnemonicdb::AttributeDef(store.AllocateId(mnemonicdb::partition::ATTR), table, name, column.Type(), unique,
	                                false);
}

//! Whether values of `from` can be converted to `to` without merging distinct values
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
	if (table->schema != "main" && !tx_add_column) {
		throw NotImplementedException("MnemonicDB: sys tables cannot be altered, except ADD COLUMN on sys.tx");
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
		auto renamed = attr;
		renamed.name = new_name;
		SetSchema(schema.WithAttribute(std::move(renamed)));
		break;
	}
	case AlterTableType::ADD_COLUMN: {
		auto &add = info.Cast<AddColumnInfo>();
		if (add.add_not_null) {
			throw NotImplementedException("MnemonicDB does not support NOT NULL: a NULL column is an absent attribute");
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
		// Retract the column's values (they stay in history), then detach the attribute from the table
		vector<EntityId> entities;
		for (auto &datom : writer.GetView().ScanPrefix(mnemonicdb::ColumnFamily::AEVT, mnemonicdb::AevtPrefix(attr.Local()))) {
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
		if (!IsSafeTypeChange(attr.type, change.target_type)) {
			throw NotImplementedException("MnemonicDB cannot change column \"%s\" from %s to %s: only conversions "
			                              "that keep distinct values distinct are allowed",
			                              attr.name, attr.type.ToString(), change.target_type.ToString());
		}
		mnemonicdb::AttributeDef changed(attr.id, attr.table, attr.name, change.target_type, attr.unique,
		                                 attr.indexed, attr.index_name);
		auto &old_codec = attr.codec;
		auto &new_codec = changed.codec;
		auto &target = changed.value_type;
		writer.RewriteAttribute(attr, changed, [&](std::string_view v) {
			return new_codec.EncodeValue(old_codec.DecodeValue(v).DefaultCastAs(target, true));
		});
		auto &sys_type = schema.GetAttribute(mnemonicdb::sys::ATTRIBUTES_TYPE);
		writer.Assert(sys_type, attr.id, sys_type.codec.EncodeValue(Value(changed.type.ToString())));
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
		auto schema_name = old_schema.GetAttribute(mnemonicdb::sys::TABLES_SCHEMA).codec.DecodeValue(datom.v).ToString();
		if (schema_name != "main" && schema_name != "sys") {
			throw InvalidInputException("MnemonicDB: unknown schema \"%s\": user tables live in \"main\"", schema_name);
		}
	}
	auto new_schema = mnemonicdb::Schema::Load(writer.GetView(), context);
	for (auto table : new_schema->TablesInSchema("sys")) {
		if (!old_schema.FindTable(table->id)) {
			throw InvalidInputException("MnemonicDB: user tables must be in schema \"main\"");
		}
	}
	for (auto old_attr : old_schema.AllAttributes()) {
		auto new_table = new_schema->FindTable(old_attr->table);
		bool still_there = false;
		if (new_table) {
			for (auto local : new_table->attributes) {
				still_there = still_there || local == old_attr->Local();
			}
		}
		if (!still_there) {
			throw NotImplementedException("MnemonicDB: attribute \"%s\" cannot be removed through sys.attributes; "
			                              "use ALTER TABLE ... DROP COLUMN",
			                              old_attr->name);
		}
		auto &new_attr = new_schema->GetAttribute(old_attr->Local());
		if (new_attr.type != old_attr->type) {
			throw NotImplementedException(
			    "MnemonicDB: the type of \"%s\" cannot be changed through sys.attributes; use ALTER TABLE ... TYPE",
			    old_attr->name);
		}
		if (new_attr.unique != mnemonicdb::Uniqueness::NONE && old_attr->unique == mnemonicdb::Uniqueness::NONE) {
			std::set<string> seen;
			for (auto &datom : writer.GetView().ScanPrefix(mnemonicdb::ColumnFamily::AEVT,
			                                               mnemonicdb::AevtPrefix(new_attr.Local()))) {
				if (!seen.insert(datom.v).second) {
					throw ConstraintException("MnemonicDB: \"%s\" cannot become unique: two entities share a value",
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
	if (schema.FindIndex(index_name)) {
		if (if_not_exists) {
			return;
		}
		throw CatalogException::EntryAlreadyExists(CatalogType::INDEX_ENTRY, Identifier(index_name));
	}
	if (!attr->index_name.empty()) {
		throw CatalogException("MnemonicDB: column \"%s\" is already indexed by \"%s\"", attr->name,
		                       attr->index_name);
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

void MnemonicTransaction::DropIndex(const string &index_name, bool if_exists) {
	auto &schema = GetSchema();
	auto attr = schema.FindIndex(index_name);
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
// Commit
//===--------------------------------------------------------------------===//
void MnemonicTransaction::Commit() {
	if (writer) {
		writer->FinishStatement();
	}
	if (writer && writer->HasChanges()) {
		auto tx = writer->GetTx();
		auto instant = MaxValue<int64_t>(Timestamp::GetCurrentTimestamp().value, store.GetLastInstant() + 1);
		mnemonicdb::Schema::WriteTxEntity(*writer, GetSchema(), tx, instant);
		lock_guard<mutex> guard(catalog.GetSnapshotLock());
		store.Commit(writer->GetWriteBatch(), tx, instant);
		if (schema_changed) {
			catalog.SetGeneration(generation);
		}
	}
	writer.reset();
	ReleaseWriter();
}

} // namespace duckdb
