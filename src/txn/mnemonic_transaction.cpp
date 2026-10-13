#include "mnemonicdb/mnemonic_transaction.hpp"
#include "mnemonicdb/mnemonic_catalog.hpp"
#include "mnemonicdb/mnemonic_schema_entry.hpp"
#include "mnemonicdb/mnemonic_table_entry.hpp"
#include "mnemonicdb/mnemonic_transaction_manager.hpp"

#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"

#include "rocksdb/db.h"

namespace duckdb {

using mnemonicdb::EntityId;

MnemonicTransaction::MnemonicTransaction(MnemonicTransactionManager &manager, ClientContext &context,
                                         MnemonicCatalog &catalog_p)
    : Transaction(manager, context), catalog(catalog_p), store(catalog_p.GetStore()),
      basis_tx(store.GetLastTx()), snapshot(store.GetDB().GetSnapshot()), view(store, snapshot),
      generation(catalog_p.GetGeneration()) {
}

MnemonicTransaction::~MnemonicTransaction() {
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
	store.AcquireWriter();
	holds_writer = true;
	if (store.GetLastTx() != basis_tx) {
		auto last = store.GetLastTx();
		ReleaseWriter();
		throw TransactionException("MnemonicDB: transaction conflict: this transaction read the database as of tx %d, "
		                           "but tx %d has committed since. Retry the transaction.",
		                           basis_tx, last);
	}
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
	mnemonicdb::TableDef table;
	table.id = store.AllocateId(mnemonicdb::partition::TABLE);
	table.schema = schema_name;
	table.name = table_name;
	vector<mnemonicdb::AttributeDef> attributes;
	for (auto &column : info.columns.Logical()) {
		auto &name = column.Name().GetIdentifierName();
		if (StringUtil::CIEquals(name, "id") || StringUtil::CIEquals(name, "rowid")) {
			throw BinderException("MnemonicDB: column name \"%s\" is reserved", name);
		}
		if (column.Generated()) {
			throw NotImplementedException("MnemonicDB does not support generated columns");
		}
		if (column.HasDefaultValue()) {
			throw NotImplementedException("MnemonicDB does not support column defaults");
		}
		auto unique = uniqueness.find(column.Logical().index);
		attributes.emplace_back(store.AllocateId(mnemonicdb::partition::ATTR), table.id, name, column.Type(),
		                        unique == uniqueness.end() ? mnemonicdb::Uniqueness::NONE : unique->second, false);
	}

	mnemonicdb::Schema::WriteTable(writer, schema, table);
	for (auto &attr : attributes) {
		mnemonicdb::Schema::WriteAttribute(writer, schema, attr);
	}
	SetSchema(schema.WithTable(table, attributes));
	return generation->GetTable(schema_name, table_name).get();
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
	auto indexed = *attr;
	indexed.indexed = true;
	indexed.index_name = index_name;
	if (!attr->HasAvet()) {
		writer.RebuildAvet(indexed, false);
	}
	mnemonicdb::Schema::WriteAttribute(writer, schema, indexed);
	SetSchema(schema.WithAttribute(std::move(indexed)));
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
}

//===--------------------------------------------------------------------===//
// Commit
//===--------------------------------------------------------------------===//
void MnemonicTransaction::Commit() {
	if (writer && writer->HasChanges()) {
		auto tx = writer->GetTx();
		auto instant = MaxValue<int64_t>(Timestamp::GetCurrentTimestamp().value, store.GetLastInstant() + 1);
		mnemonicdb::Schema::WriteTxEntity(*writer, GetSchema(), tx, instant);
		store.Commit(writer->GetWriteBatch(), tx, instant);
		if (schema_changed) {
			catalog.SetGeneration(generation);
		}
	}
	writer.reset();
	ReleaseWriter();
}

} // namespace duckdb
