#include "mnemonicdb/tx_writer.hpp"

#include "duckdb/common/exception.hpp"

#include <set>

using duckdb::optional_idx;

#include "rocksdb/db.h"
#include "rocksdb/utilities/write_batch_with_index.h"

namespace mnemonicdb {

static void CheckStatus(const rocksdb::Status &status) {
	if (!status.ok()) {
		throw duckdb::IOException("MnemonicDB: RocksDB error: %s", status.ToString());
	}
}

//===--------------------------------------------------------------------===//
// ReadView
//===--------------------------------------------------------------------===//
ReadView::ReadView(Store &store_p, const rocksdb::Snapshot *snapshot_p, rocksdb::WriteBatchWithIndex *batch_p)
    : store(&store_p), snapshot(snapshot_p), batch(batch_p) {
}

std::unique_ptr<rocksdb::Iterator> ReadView::NewIterator(ColumnFamily cf) {
	rocksdb::ReadOptions options;
	options.snapshot = snapshot;
	auto &handle = store->GetHandle(cf);
	auto base = store->GetDB().NewIterator(options, &handle);
	if (!batch) {
		return std::unique_ptr<rocksdb::Iterator>(base);
	}
	return std::unique_ptr<rocksdb::Iterator>(batch->NewIteratorWithBase(&handle, base));
}

vector<Datom> ReadView::ScanPrefix(ColumnFamily cf, const string &prefix, vector<TxId> *retracted_by) {
	vector<Datom> result;
	auto kind = IndexKindOf(cf);
	auto it = NewIterator(cf);
	for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next()) {
		result.push_back(DecodeKey(kind, std::string_view(it->key().data(), it->key().size())));
		if (retracted_by) {
			retracted_by->push_back(DecodeHistoryValue(std::string_view(it->value().data(), it->value().size())));
		}
	}
	CheckStatus(it->status());
	return result;
}

//===--------------------------------------------------------------------===//
// TxWriter
//===--------------------------------------------------------------------===//
TxWriter::TxWriter(Store &store_p, const rocksdb::Snapshot *snapshot, TxId tx_p)
    : store(store_p), tx(tx_p),
      batch(std::make_unique<rocksdb::WriteBatchWithIndex>(rocksdb::BytewiseComparator(), 0, true)),
      view(store_p, snapshot, batch.get()) {
}

TxWriter::~TxWriter() = default;

bool TxWriter::HasChanges() {
	// Writes that cancel out leave deletes in the batch but no txlog entry for this transaction
	auto prefix = TxlogPrefix(tx);
	auto it = view.NewIterator(ColumnFamily::TXLOG);
	it->Seek(prefix);
	return it->Valid() && it->key().starts_with(prefix);
}

rocksdb::WriteBatch &TxWriter::GetWriteBatch() {
	return *batch->GetWriteBatch();
}

void TxWriter::Put(ColumnFamily cf, const string &key, const string &value) {
	CheckStatus(batch->Put(&store.GetHandle(cf), key, value));
}

void TxWriter::Delete(ColumnFamily cf, const string &key) {
	CheckStatus(batch->Delete(&store.GetHandle(cf), key));
}

vector<Datom> TxWriter::CurrentDatoms(EntityId e, AttrId a) {
	return view.ScanPrefix(ColumnFamily::EAVT, EavtPrefix(e, a));
}

//! The column families holding an attribute's current datoms, paired with their history column families
static vector<std::pair<ColumnFamily, ColumnFamily>> IndexesOf(const AttributeDef &attr) {
	vector<std::pair<ColumnFamily, ColumnFamily>> result {{ColumnFamily::EAVT, ColumnFamily::EAVT_HIST},
	                                                      {ColumnFamily::AEVT, ColumnFamily::AEVT_HIST}};
	if (attr.HasAvet()) {
		result.emplace_back(ColumnFamily::AVET, ColumnFamily::AVET_HIST);
	}
	if (attr.is_ref) {
		result.emplace_back(ColumnFamily::VAET, ColumnFamily::VAET_HIST);
	}
	return result;
}

void TxWriter::PutIndexes(const AttributeDef &attr, const Datom &datom) {
	for (auto &index : IndexesOf(attr)) {
		Put(index.first, EncodeKey(IndexKindOf(index.first), datom), string());
	}
}

void TxWriter::CheckUnique(const AttributeDef &attr, EntityId, const string &v) {
	if (attr.unique != Uniqueness::NONE) {
		pending_unique.emplace_back(attr.Local(), v, attr.name);
	}
}

void TxWriter::BeginStatement() {
	batch->SetSavePoint();
	pending_unique.clear();
}

void TxWriter::PopStatement() {
	CheckStatus(batch->PopSavePoint());
}

void TxWriter::RollbackStatement() {
	CheckStatus(batch->RollbackToSavePoint());
	pending_unique.clear();
}

void TxWriter::FinishStatement() {
	auto pending = std::move(pending_unique);
	pending_unique.clear();
	for (auto &entry : pending) {
		auto &[local, v, name] = entry;
		optional_idx first;
		for (auto &existing : view.ScanPrefix(ColumnFamily::AVET, AvetPrefix(local, v))) {
			if (existing.v != v) {
				continue;
			}
			if (!first.IsValid()) {
				first = existing.e;
			} else if (first.GetIndex() != existing.e) {
				throw duckdb::ConstraintException(
				    "Duplicate key: attribute \"%s\" has the same value on entities %d and %d", name,
				    first.GetIndex(), existing.e);
			}
		}
	}
}

void TxWriter::Assert(const AttributeDef &attr, EntityId e, const string &v) {
	auto current = CurrentDatoms(e, attr.Local());
	for (auto &datom : current) {
		if (datom.v == v) {
			return;
		}
	}
	CheckUnique(attr, e, v);
	if (!attr.many) {
		for (auto &datom : current) {
			RetractDatom(attr, datom);
		}
	}
	if (UndoRetraction(attr, e, v)) {
		return;
	}
	Datom datom;
	datom.e = e;
	datom.a = attr.Local();
	datom.v = v;
	datom.t = tx;
	PutIndexes(attr, datom);
	Put(ColumnFamily::TXLOG, EncodeKey(IndexKind::TXLOG, datom), EncodeTxOp(TxOp::ASSERT));
}

void TxWriter::Retract(const AttributeDef &attr, EntityId e, const string &v) {
	for (auto &datom : CurrentDatoms(e, attr.Local())) {
		if (datom.v == v) {
			RetractDatom(attr, datom);
		}
	}
}

void TxWriter::RetractAll(const AttributeDef &attr, EntityId e) {
	for (auto &datom : CurrentDatoms(e, attr.Local())) {
		RetractDatom(attr, datom);
	}
}

void TxWriter::RetractDatom(const AttributeDef &attr, const Datom &datom) {
	Datom log_entry = datom;
	log_entry.t = tx;
	auto log_key = EncodeKey(IndexKind::TXLOG, log_entry);
	if (datom.t == tx) {
		// Asserted earlier in this transaction: it never becomes visible, so it leaves no history
		for (auto &index : IndexesOf(attr)) {
			Delete(index.first, EncodeKey(IndexKindOf(index.first), datom));
		}
		Delete(ColumnFamily::TXLOG, log_key);
		return;
	}
	auto history_value = EncodeHistoryValue(tx);
	for (auto &index : IndexesOf(attr)) {
		auto key = EncodeKey(IndexKindOf(index.first), datom);
		Delete(index.first, key);
		Put(index.second, key, history_value);
	}
	Put(ColumnFamily::TXLOG, log_key, EncodeTxOp(TxOp::RETRACT));
}

bool TxWriter::UndoRetraction(const AttributeDef &attr, EntityId e, const string &v) {
	// A value retracted earlier in this transaction and asserted again is simply restored
	vector<TxId> retracted_by;
	auto history = view.ScanPrefix(ColumnFamily::EAVT_HIST, EavtPrefix(e, attr.Local()) + v, &retracted_by);
	for (idx_t i = 0; i < history.size(); i++) {
		auto &datom = history[i];
		if (datom.v != v || retracted_by[i] != tx) {
			continue;
		}
		for (auto &index : IndexesOf(attr)) {
			auto key = EncodeKey(IndexKindOf(index.first), datom);
			Delete(index.second, key);
			Put(index.first, key, string());
		}
		Datom log_entry = datom;
		log_entry.t = tx;
		Delete(ColumnFamily::TXLOG, EncodeKey(IndexKind::TXLOG, log_entry));
		return true;
	}
	return false;
}

void TxWriter::RewriteAttribute(const AttributeDef &old_attr, const AttributeDef &new_attr,
                                const std::function<string(std::string_view)> &reencode) {
	struct Entry {
		Datom datom;
		bool history;
		TxId retracted_by;
		string new_v;
	};
	vector<Entry> entries;
	auto prefix = AevtPrefix(old_attr.Local());
	for (auto history : {false, true}) {
		auto it = view.NewIterator(history ? ColumnFamily::AEVT_HIST : ColumnFamily::AEVT);
		for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next()) {
			Entry entry;
			entry.datom = DecodeKey(IndexKind::AEVT, std::string_view(it->key().data(), it->key().size()));
			entry.history = history;
			entry.retracted_by =
			    history ? DecodeHistoryValue(std::string_view(it->value().data(), it->value().size())) : 0;
			entry.new_v = reencode(entry.datom.v);
			entries.push_back(std::move(entry));
		}
		CheckStatus(it->status());
	}

	// The conversion must keep distinct current values distinct, per entity and, for unique attributes, overall
	std::set<std::pair<EntityId, string>> per_entity;
	std::set<string> overall;
	for (auto &entry : entries) {
		if (entry.history) {
			continue;
		}
		if (!per_entity.insert({entry.datom.e, entry.new_v}).second) {
			throw duckdb::InvalidInputException(
			    "MnemonicDB: changing the type of \"%s\" would merge two values of entity %d", old_attr.name,
			    entry.datom.e);
		}
		if (new_attr.unique != Uniqueness::NONE && !overall.insert(entry.new_v).second) {
			throw duckdb::ConstraintException(
			    "MnemonicDB: changing the type of \"%s\" would make two entities share a unique value",
			    old_attr.name);
		}
	}

	auto history_cfs = [](bool history, ColumnFamily current, ColumnFamily hist) { return history ? hist : current; };
	for (auto &entry : entries) {
		auto old_datom = entry.datom;
		auto new_datom = entry.datom;
		new_datom.v = entry.new_v;
		auto value = entry.history ? EncodeHistoryValue(entry.retracted_by) : string();
		for (auto &index : IndexesOf(old_attr)) {
			Delete(history_cfs(entry.history, index.first, index.second), EncodeKey(IndexKindOf(index.first), old_datom));
		}
		for (auto &index : IndexesOf(new_attr)) {
			Put(history_cfs(entry.history, index.first, index.second), EncodeKey(IndexKindOf(index.first), new_datom),
			    value);
		}
		// The txlog records the assertion, and for history entries the retraction
		vector<std::pair<TxId, TxOp>> log_entries {{old_datom.t, TxOp::ASSERT}};
		if (entry.history) {
			log_entries.emplace_back(entry.retracted_by, TxOp::RETRACT);
		}
		for (auto &log : log_entries) {
			auto old_log = old_datom;
			old_log.t = log.first;
			auto new_log = new_datom;
			new_log.t = log.first;
			Delete(ColumnFamily::TXLOG, EncodeKey(IndexKind::TXLOG, old_log));
			Put(ColumnFamily::TXLOG, EncodeKey(IndexKind::TXLOG, new_log), EncodeTxOp(log.second));
		}
	}
}

void TxWriter::RebuildAvet(const AttributeDef &attr, bool remove) {
	auto prefix = AevtPrefix(attr.Local());
	for (auto history : {false, true}) {
		auto source = history ? ColumnFamily::AEVT_HIST : ColumnFamily::AEVT;
		auto target = history ? ColumnFamily::AVET_HIST : ColumnFamily::AVET;
		vector<std::pair<string, string>> entries;
		auto it = view.NewIterator(source);
		for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next()) {
			auto datom = DecodeKey(IndexKind::AEVT, std::string_view(it->key().data(), it->key().size()));
			entries.emplace_back(EncodeKey(IndexKind::AVET, datom), it->value().ToString());
		}
		CheckStatus(it->status());
		for (auto &entry : entries) {
			if (remove) {
				Delete(target, entry.first);
			} else {
				Put(target, entry.first, entry.second);
			}
		}
	}
}

} // namespace mnemonicdb
