#pragma once

#include "mnemonicdb/keys.hpp"
#include "mnemonicdb/schema.hpp"
#include "mnemonicdb/store.hpp"

#include <functional>
#include <tuple>
#include <memory>

namespace rocksdb {
class Iterator;
class Snapshot;
class WriteBatch;
class WriteBatchWithIndex;
} // namespace rocksdb

namespace mnemonicdb {

//! A consistent view of the database: a RocksDB snapshot, optionally overlaid with a transaction's pending writes
class ReadView {
public:
	ReadView(Store &store, const rocksdb::Snapshot *snapshot, rocksdb::WriteBatchWithIndex *batch = nullptr);

	Store &GetStore() {
		return *store;
	}
	std::unique_ptr<rocksdb::Iterator> NewIterator(ColumnFamily cf);
	//! All entries of `cf` whose key starts with `prefix`, decoded; history values are returned in `retracted_by`
	vector<Datom> ScanPrefix(ColumnFamily cf, const string &prefix, vector<TxId> *retracted_by = nullptr);

private:
	Store *store;
	const rocksdb::Snapshot *snapshot;
	rocksdb::WriteBatchWithIndex *batch;
};

//! Accumulates the datoms of one transaction in a write batch, keeping every index consistent
class TxWriter {
public:
	TxWriter(Store &store, const rocksdb::Snapshot *snapshot, TxId tx);
	~TxWriter();

	TxId GetTx() const {
		return tx;
	}
	ReadView &GetView() {
		return view;
	}
	//! Whether the transaction changes any datom, after writes that cancel out
	bool HasChanges();
	rocksdb::WriteBatch &GetWriteBatch();

	//! Current datoms of (e, a), including this transaction's writes
	vector<Datom> CurrentDatoms(EntityId e, AttrId a);

	//! Assert a value. Cardinality-one attributes retract any other current value; asserting a value that is
	//! already current does nothing. Throws on a uniqueness violation.
	void Assert(const AttributeDef &attr, EntityId e, const string &v);
	//! Statement savepoints: everything written after BeginStatement is undone by RollbackStatement
	void BeginStatement();
	void PopStatement();
	void RollbackStatement();

	//! Check the uniqueness of every unique value asserted since the last call. Uniqueness is checked per
	//! statement, so a statement may move unique values between entities (e.g. swap them).
	void FinishStatement();

	//! Retract one value if it is current
	void Retract(const AttributeDef &attr, EntityId e, const string &v);
	//! Retract every current value of (e, attr)
	void RetractAll(const AttributeDef &attr, EntityId e);

	//! Build (or, with `remove`, delete) the AVET and AVET history entries of an attribute from its AEVT entries
	void RebuildAvet(const AttributeDef &attr, bool remove);

	//! Re-encode every datom of an attribute, current and history, in every index and the txlog.
	//! `reencode` maps an old encoded value to the new encoding.
	void RewriteAttribute(const AttributeDef &old_attr, const AttributeDef &new_attr,
	                      const std::function<string(std::string_view)> &reencode);

	//! Raw index writes, for maintenance that bypasses datom semantics
	void Put(ColumnFamily cf, const string &key, const string &value);
	void Delete(ColumnFamily cf, const string &key);

private:
	void PutIndexes(const AttributeDef &attr, const Datom &datom);
	void RetractDatom(const AttributeDef &attr, const Datom &datom);
	bool UndoRetraction(const AttributeDef &attr, EntityId e, const string &v);
	void CheckUnique(const AttributeDef &attr, EntityId e, const string &v);

private:
	Store &store;
	TxId tx;
	//! Unique values asserted in the current statement: (attribute, encoded value, attribute name)
	vector<std::tuple<AttrId, string, string>> pending_unique;
	std::unique_ptr<rocksdb::WriteBatchWithIndex> batch;
	ReadView view;
};

} // namespace mnemonicdb
