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
class MnemonicTransactionManager;
struct BoundCreateTableInfo;

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

	//! The real entity ID a tempid placeholder resolved to in this transaction, if any
	optional_ptr<const mnemonicdb::EntityId> FindTempid(mnemonicdb::EntityId placeholder) const;
	void BindTempid(mnemonicdb::EntityId placeholder, mnemonicdb::EntityId id);

	//! Write this transaction's changes; called by the transaction manager
	void Commit();

private:
	void ReleaseWriter();

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
