#include "mnemonicdb/tx_writer.hpp"

#include "duckdb/common/exception.hpp"

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
    : store(store_p), snapshot(snapshot_p), batch(batch_p) {
}

std::unique_ptr<rocksdb::Iterator> ReadView::NewIterator(ColumnFamily cf) {
	rocksdb::ReadOptions options;
	options.snapshot = snapshot;
	auto &handle = store.GetHandle(cf);
	auto base = store.GetDB().NewIterator(options, &handle);
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

void TxWriter::CheckUnique(const AttributeDef &attr, EntityId e, const string &v) {
	if (attr.unique == Uniqueness::NONE) {
		return;
	}
	for (auto &existing : view.ScanPrefix(ColumnFamily::AVET, AvetPrefix(attr.Local(), v))) {
		if (existing.v == v && existing.e != e) {
			throw duckdb::ConstraintException("Duplicate key: attribute \"%s\" already has this value on entity %d",
			                                  attr.name, existing.e);
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

} // namespace mnemonicdb
