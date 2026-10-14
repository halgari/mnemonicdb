#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "mnemonicdb/tx_writer.hpp"

namespace rocksdb {
class Snapshot;
}

namespace duckdb {

class MnemonicCatalog;
class MnemonicCatalogGeneration;
class MnemonicSchemaEntry;
class MnemonicTableEntry;
class MnemonicTransactionManager;
struct BoundCreateTableInfo;
struct AlterTableInfo;
struct CreateSchemaInfo;
struct CreateTypeInfo;
class ColumnDefinition;

class MnemonicTransaction;

//! Makes one statement's writes atomic within the transaction: on failure (an
//! exception before Commit), the write batch, tempid bindings, ID counters and
//! schema are restored to their state at the start of the statement
class MnemonicStatement {
public:
	explicit MnemonicStatement(MnemonicTransaction &transaction);
	~MnemonicStatement();
	//! Check deferred constraints and keep the statement's writes
	void Commit();

private:
	MnemonicTransaction &transaction;
	bool done = false;
	mnemonicdb::Store::CounterState counters;
	unordered_map<mnemonicdb::EntityId, mnemonicdb::EntityId> tempids;
	shared_ptr<MnemonicCatalogGeneration> generation;
	bool schema_changed;
};

class MnemonicTransaction : public Transaction {
	friend class MnemonicStatement;

public:
	MnemonicTransaction(MnemonicTransactionManager &manager, ClientContext &context, MnemonicCatalog &catalog);
	//! A borrowed, read-only transaction used to evaluate commit assertions: it
	//! reads `parent`'s snapshot, pending writes and catalog generation, owns
	//! none of them, and can never take the writer lock
	MnemonicTransaction(MnemonicTransactionManager &manager, ClientContext &context, MnemonicCatalog &catalog,
	                    MnemonicTransaction &parent);
	~MnemonicTransaction() override;

public:
	static MnemonicTransaction &Get(ClientContext &context, Catalog &catalog);

	//! Called by DuckDB before the first modifying statement: take the writer
	//! lock and check for conflicts
	void SetReadWrite() override;

	//! The last transaction committed when this transaction began
	mnemonicdb::TxId GetBasisTx() const {
		return basis_tx;
	}
	//! Reads see the snapshot, plus this transaction's own writes once it writes
	mnemonicdb::ReadView &GetReadView();
	//! Only what was committed when this transaction began
	mnemonicdb::ReadView &GetCommittedView() {
		return view;
	}
	//! Record that the transaction read data, so it can no longer move its
	//! snapshot forward on its first write
	void MarkDataRead() {
		read_data = true;
	}
	//! The writer of a read-write transaction
	mnemonicdb::TxWriter &GetWriter();
	MnemonicCatalog &GetCatalog() {
		return catalog;
	}

	//! The catalog entries this transaction sees
	const shared_ptr<MnemonicCatalogGeneration> &GetGeneration() const {
		return generation;
	}
	const mnemonicdb::Schema &GetSchema() const;

	//! CREATE SCHEMA / DROP SCHEMA inside this transaction
	optional_ptr<CatalogEntry> CreateSchema(CreateSchemaInfo &info);
	void DropSchema(const string &schema_name, bool if_exists, bool cascade);
	//! CREATE [OR REPLACE] TYPE / DROP TYPE inside this transaction
	optional_ptr<CatalogEntry> CreateType(const string &schema_name, CreateTypeInfo &info);
	void DropType(const string &schema_name, const string &type_name, bool if_exists);
	//! CREATE TABLE inside this transaction
	optional_ptr<CatalogEntry> CreateTable(MnemonicSchemaEntry &schema, BoundCreateTableInfo &info);
	//! CREATE INDEX / DROP INDEX inside this transaction
	void CreateIndex(MnemonicTableEntry &table, const string &index_name, const string &column, bool if_not_exists);
	void DropIndex(const string &schema_name, const string &index_name, bool if_exists);
	//! ALTER TABLE inside this transaction
	void AlterTable(const string &schema_name, AlterTableInfo &info);
	//! Re-read the schema after a statement wrote to sys tables, and bring the
	//! indexes in line with it
	void ReloadSchema(ClientContext &context);
	//! The entity of this (writing) transaction
	mnemonicdb::EntityId CurrentTxEntity();

	//! The real entity ID a tempid placeholder resolved to in this transaction,
	//! if any
	optional_ptr<const mnemonicdb::EntityId> FindTempid(mnemonicdb::EntityId placeholder) const;
	void BindTempid(mnemonicdb::EntityId placeholder, mnemonicdb::EntityId id);

	//! The tx number of this transaction's pending writes: its own writer's tx,
	//! else (when borrowed) its parent's, else 0 (nothing pending)
	mnemonicdb::TxId PendingTx() const;
	//! Whether this transaction borrows a committing transaction's state
	bool IsBorrowed() const {
		return parent != nullptr;
	}

	//! Evaluate commit assertions, then write this transaction's changes; called
	//! by the transaction manager with the committing context
	void Commit(ClientContext &context);

	//! Serializes writers within this transaction: e.g. the actions of a MERGE
	//! run as separate pipelines
	mutex &GetWriteLock() {
		return write_lock;
	}

private:
	void ReleaseWriter();
	void TakeSnapshot();
	void SetSchema(shared_ptr<const mnemonicdb::Schema> schema);
	//! A new attribute definition for a column of a new or altered table
	mnemonicdb::AttributeDef NewAttribute(mnemonicdb::EntityId table, const ColumnDefinition &column,
	                                      mnemonicdb::Uniqueness unique);
	//! Check that every named type `type` mentions is one of this database's
	void CheckNamedTypes(const LogicalType &type) const;
	//! CREATE OR REPLACE TYPE of an existing type: append ENUM values
	void ReplaceType(const mnemonicdb::TypeDef &existing, const LogicalType &type);

private:
	MnemonicCatalog &catalog;
	mnemonicdb::Store &store;
	//! Declared before the snapshot: read before taking it, so the snapshot
	//! covers at least this tx
	mnemonicdb::TxId basis_tx;
	const rocksdb::Snapshot *snapshot;
	mnemonicdb::ReadView view;
	unique_ptr<mnemonicdb::TxWriter> writer;
	bool holds_writer = false;
	//! The ID counters when this transaction took the writer, restored if it does
	//! not commit
	mnemonicdb::Store::CounterState counters_at_start;
	bool read_data = false;
	shared_ptr<MnemonicCatalogGeneration> generation;
	bool schema_changed = false;
	unordered_map<mnemonicdb::EntityId, mnemonicdb::EntityId> tempids;
	mutex write_lock;
	//! Set for a borrowed transaction (assertion evaluation): the committing
	//! transaction whose snapshot and batch `view` reads
	MnemonicTransaction *parent = nullptr;
};

} // namespace duckdb
