#pragma once

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
class ColumnDefinition;

class MnemonicTransaction : public Transaction {
public:
	MnemonicTransaction(MnemonicTransactionManager &manager, ClientContext &context, MnemonicCatalog &catalog);
	~MnemonicTransaction() override;

public:
	static MnemonicTransaction &Get(ClientContext &context, Catalog &catalog);

	//! Called by DuckDB before the first modifying statement: take the writer lock and check for conflicts
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

	//! CREATE TABLE inside this transaction
	optional_ptr<CatalogEntry> CreateTable(MnemonicSchemaEntry &schema, BoundCreateTableInfo &info);
	//! CREATE INDEX / DROP INDEX inside this transaction
	void CreateIndex(MnemonicTableEntry &table, const string &index_name, const string &column, bool if_not_exists);
	void DropIndex(const string &index_name, bool if_exists);
	//! ALTER TABLE inside this transaction
	void AlterTable(const string &schema_name, AlterTableInfo &info);
	//! Re-read the schema after a statement wrote to sys tables, and bring the indexes in line with it
	void ReloadSchema(ClientContext &context);
	//! The entity of this (writing) transaction
	mnemonicdb::EntityId CurrentTxEntity();

	//! The real entity ID a tempid placeholder resolved to in this transaction, if any
	optional_ptr<const mnemonicdb::EntityId> FindTempid(mnemonicdb::EntityId placeholder) const;
	void BindTempid(mnemonicdb::EntityId placeholder, mnemonicdb::EntityId id);

	//! Write this transaction's changes; called by the transaction manager
	void Commit();

private:
	void ReleaseWriter();
	void SetSchema(shared_ptr<const mnemonicdb::Schema> schema);
	//! A new attribute definition for a column of a new or altered table
	mnemonicdb::AttributeDef NewAttribute(mnemonicdb::EntityId table, const ColumnDefinition &column,
	                                      mnemonicdb::Uniqueness unique);

private:
	MnemonicCatalog &catalog;
	mnemonicdb::Store &store;
	//! Declared before the snapshot: read before taking it, so the snapshot covers at least this tx
	mnemonicdb::TxId basis_tx;
	const rocksdb::Snapshot *snapshot;
	mnemonicdb::ReadView view;
	unique_ptr<mnemonicdb::TxWriter> writer;
	bool holds_writer = false;
	shared_ptr<MnemonicCatalogGeneration> generation;
	bool schema_changed = false;
	unordered_map<mnemonicdb::EntityId, mnemonicdb::EntityId> tempids;
};

} // namespace duckdb
