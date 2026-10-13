#pragma once

#include "mnemonicdb/keys.hpp"
#include "mnemonicdb/schema.hpp"
#include "mnemonicdb/store.hpp"

#include <array>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <memory>

namespace rocksdb {
class Iterator;
class Snapshot;
class WriteBatch;
class WriteBatchWithIndex;
} // namespace rocksdb

namespace mnemonicdb {

class ReadView;

//! Short prefix lookups without creating iterators: keeps one RocksDB iterator per column family (a prefix-mode one
//! for seeks covering the full Bloom prefix, and a total-order one) for the life of the reader, and merges in the
//! transaction's pending writes. Not thread-safe: each thread needs its own reader.
class PrefixReader {
public:
	explicit PrefixReader(ReadView &view);
	~PrefixReader();

	//! Call `callback(key, value)` for every visible entry of `cf` whose key starts with `prefix`, in key order
	void Scan(ColumnFamily cf, const string &prefix,
	          const std::function<void(std::string_view key, std::string_view value)> &callback);

private:
	rocksdb::Iterator &GetIterator(ColumnFamily cf, bool prefix_mode);

	ReadView &view;
	std::array<std::array<std::unique_ptr<rocksdb::Iterator>, 2>, static_cast<size_t>(ColumnFamily::COUNT)> iterators;
};

//! A consistent view of the database: a RocksDB snapshot, optionally overlaid with a transaction's pending writes
class ReadView {
	friend class PrefixReader;

public:
	ReadView(Store &store, const rocksdb::Snapshot *snapshot, rocksdb::WriteBatchWithIndex *batch = nullptr);
	ReadView(ReadView &&other) noexcept;
	ReadView &operator=(ReadView &&other) noexcept;
	~ReadView();

	Store &GetStore() {
		return *store;
	}
	//! An iterator over `cf`. With a prefix, iteration stops at the end of the prefix (iterate_upper_bound) and
	//! RocksDB may use prefix Bloom filters when the prefix covers the column family's prefix extractor.
	std::unique_ptr<rocksdb::Iterator> NewIterator(ColumnFamily cf, const string &prefix = string());
	//! All entries of `cf` whose key starts with `prefix`, decoded; history values are returned in `retracted_by`
	vector<Datom> ScanPrefix(ColumnFamily cf, const string &prefix, vector<TxId> *retracted_by = nullptr);
	//! The view's own prefix reader (single-threaded use, e.g. by the transaction's writer)
	PrefixReader &GetReader();
	//! Drop cached iterators: required before the snapshot is released, or after the batch is rolled back
	void ClearCache();

private:
	Store *store;
	const rocksdb::Snapshot *snapshot;
	rocksdb::WriteBatchWithIndex *batch;
	unique_ptr<PrefixReader> reader;
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
	//! Net change in current datoms per attribute
	const std::unordered_map<AttrId, int64_t> &GetCountDeltas() const {
		return count_deltas;
	}

	//! Record that `e` was allocated by this transaction, so lookups of its untouched attributes can be skipped
	void MarkFresh(EntityId e) {
		fresh_entities.insert(e);
	}

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
	//! Whether (e, a) certainly has no datoms, current or historical: a fresh entity this transaction never wrote
	bool KnownEmpty(EntityId e, AttrId a) const {
		return fresh_entities.count(e) && !touched.count({e, a});
	}
	void PutIndexes(const AttributeDef &attr, const Datom &datom);
	void RetractDatom(const AttributeDef &attr, const Datom &datom);
	bool UndoRetraction(const AttributeDef &attr, EntityId e, const string &v);
	void CheckUnique(const AttributeDef &attr, EntityId e, const string &v);

private:
	Store &store;
	TxId tx;
	//! Unique values asserted in the current statement: (attribute, encoded value, attribute name)
	vector<std::tuple<AttrId, string, string>> pending_unique;

	struct EntityAttrHash {
		size_t operator()(const std::pair<EntityId, AttrId> &key) const {
			return std::hash<uint64_t>()(key.first * 0x9E3779B97F4A7C15ULL ^ key.second);
		}
	};
	using EntityAttrSet = std::unordered_set<std::pair<EntityId, AttrId>, EntityAttrHash>;
	//! Entities allocated by this transaction: they have no datoms except those this transaction wrote
	std::unordered_set<EntityId> fresh_entities;
	//! (e, a) pairs this transaction wrote, and those it retracted into history
	EntityAttrSet touched;
	EntityAttrSet retracted;
	std::unordered_map<AttrId, int64_t> count_deltas;
	std::unordered_map<AttrId, int64_t> statement_count_deltas;
	std::unique_ptr<rocksdb::WriteBatchWithIndex> batch;
	ReadView view;
};

} // namespace mnemonicdb
