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

ReadView::ReadView(ReadView &&other) noexcept
    : store(other.store), snapshot(other.snapshot), batch(other.batch), reader(nullptr) {
	other.ClearCache();
}

ReadView &ReadView::operator=(ReadView &&other) noexcept {
	ClearCache();
	other.ClearCache();
	store = other.store;
	snapshot = other.snapshot;
	batch = other.batch;
	return *this;
}

ReadView::~ReadView() = default;

PrefixReader &ReadView::GetReader() {
	if (!reader) {
		reader = duckdb::make_uniq<PrefixReader>(*this);
	}
	return *reader;
}

void ReadView::ClearCache() {
	reader.reset();
}

//===--------------------------------------------------------------------===//
// PrefixReader
//===--------------------------------------------------------------------===//
PrefixReader::PrefixReader(ReadView &view_p) : view(view_p) {
}

PrefixReader::~PrefixReader() = default;

rocksdb::Iterator &PrefixReader::GetIterator(ColumnFamily cf, bool prefix_mode) {
	auto &slot = iterators[static_cast<size_t>(cf)][prefix_mode ? 1 : 0];
	if (!slot) {
		rocksdb::ReadOptions options;
		options.snapshot = view.snapshot;
		// Prefix mode: iteration stays within the seek key's 8-byte prefix, and
		// Bloom filters apply
		options.prefix_same_as_start = prefix_mode;
		options.total_order_seek = !prefix_mode;
		slot.reset(view.store->GetDB().NewIterator(options, &view.store->GetHandle(cf)));
	}
	return *slot;
}

void PrefixReader::Scan(ColumnFamily cf, const string &prefix,
                        const std::function<void(std::string_view key, std::string_view value)> &callback) {
	bool prefix_mode = HasPrefixBloom(cf) && prefix.size() >= BLOOM_PREFIX_LENGTH;
	auto &base = GetIterator(cf, prefix_mode);
	base.Seek(prefix);
	std::unique_ptr<rocksdb::WBWIIterator> delta;
	if (view.batch) {
		delta.reset(view.batch->NewIterator(&view.store->GetHandle(cf)));
		delta->Seek(prefix);
	}
	auto base_valid = [&]() {
		return base.Valid() && base.key().starts_with(prefix);
	};
	auto delta_valid = [&]() {
		return delta && delta->Valid() && delta->Entry().key.starts_with(prefix);
	};
	auto view_of = [](const rocksdb::Slice &slice) {
		return std::string_view(slice.data(), slice.size());
	};
	// Merge the snapshot with the pending writes; with overwrite_key each key
	// appears once in the batch
	while (true) {
		bool has_base = base_valid();
		bool has_delta = delta_valid();
		if (!has_base && !has_delta) {
			break;
		}
		int cmp = !has_base ? 1 : !has_delta ? -1 : base.key().compare(delta->Entry().key);
		if (cmp < 0) {
			callback(view_of(base.key()), view_of(base.value()));
			base.Next();
			continue;
		}
		auto entry = delta->Entry();
		if (entry.type == rocksdb::kPutRecord) {
			callback(view_of(entry.key), view_of(entry.value));
		}
		if (cmp == 0) {
			base.Next();
		}
		delta->Next();
	}
	CheckStatus(base.status());
	if (delta) {
		CheckStatus(delta->status());
	}
}

//! The smallest key greater than every key starting with `prefix`, or empty if
//! there is none (all 0xFF)
static string PrefixSuccessor(const string &prefix) {
	string result = prefix;
	while (!result.empty()) {
		auto &last = reinterpret_cast<unsigned char &>(result.back());
		if (last != 0xFF) {
			last++;
			return result;
		}
		result.pop_back();
	}
	return result;
}

//! A RocksDB iterator that owns the upper bound its read options point to
class BoundedIterator : public rocksdb::Iterator {
public:
	explicit BoundedIterator(string upper_bound_p) : upper_bound(std::move(upper_bound_p)), upper_slice(upper_bound) {
	}
	const rocksdb::Slice *Bound() const {
		return &upper_slice;
	}
	void SetIterator(rocksdb::Iterator *it) {
		inner.reset(it);
	}

	bool Valid() const override {
		return inner->Valid();
	}
	void SeekToFirst() override {
		inner->SeekToFirst();
	}
	void SeekToLast() override {
		inner->SeekToLast();
	}
	void Seek(const rocksdb::Slice &target) override {
		inner->Seek(target);
	}
	void SeekForPrev(const rocksdb::Slice &target) override {
		inner->SeekForPrev(target);
	}
	void Next() override {
		inner->Next();
	}
	void Prev() override {
		inner->Prev();
	}
	rocksdb::Slice key() const override {
		return inner->key();
	}
	rocksdb::Slice value() const override {
		return inner->value();
	}
	rocksdb::Status status() const override {
		return inner->status();
	}

private:
	string upper_bound;
	rocksdb::Slice upper_slice;
	std::unique_ptr<rocksdb::Iterator> inner;
};

std::unique_ptr<rocksdb::Iterator> ReadView::NewIterator(ColumnFamily cf, const string &prefix) {
	rocksdb::ReadOptions options;
	options.snapshot = snapshot;
	auto &handle = store->GetHandle(cf);
	auto upper_bound = prefix.empty() ? string() : PrefixSuccessor(prefix);
	if (upper_bound.empty()) {
		auto base = store->GetDB().NewIterator(options, &handle);
		if (!batch) {
			return std::unique_ptr<rocksdb::Iterator>(base);
		}
		return std::unique_ptr<rocksdb::Iterator>(batch->NewIteratorWithBase(&handle, base, &options));
	}
	auto result = std::make_unique<BoundedIterator>(std::move(upper_bound));
	options.iterate_upper_bound = result->Bound();
	options.auto_prefix_mode = true;
	auto base = store->GetDB().NewIterator(options, &handle);
	result->SetIterator(batch ? batch->NewIteratorWithBase(&handle, base, &options) : base);
	return std::move(result);
}

vector<Datom> ReadView::ScanPrefix(ColumnFamily cf, const string &prefix, vector<TxId> *retracted_by) {
	vector<Datom> result;
	auto kind = IndexKindOf(cf);
	GetReader().Scan(cf, prefix, [&](std::string_view key, std::string_view value) {
		result.push_back(DecodeKey(kind, key));
		if (retracted_by) {
			retracted_by->push_back(DecodeHistoryValue(value));
		}
	});
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
	// Writes that cancel out leave deletes in the batch but no txlog entry for
	// this transaction
	auto prefix = TxlogPrefix(tx);
	auto it = view.NewIterator(ColumnFamily::TXLOG, prefix);
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

//! The column families holding an attribute's current datoms, paired with their
//! history column families
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
	touched.insert({datom.e, datom.a});
	count_deltas[datom.a]++;
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
	statement_count_deltas = count_deltas;
}

void TxWriter::PopStatement() {
	CheckStatus(batch->PopSavePoint());
}

void TxWriter::RollbackStatement() {
	view.ClearCache();
	CheckStatus(batch->RollbackToSavePoint());
	pending_unique.clear();
	count_deltas = statement_count_deltas;
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
				throw duckdb::ConstraintException("Duplicate key: attribute \"%s\" has the same value on entities %d "
				                                  "and %d",
				                                  name, first.GetIndex(), existing.e);
			}
		}
	}
}

void TxWriter::Assert(const AttributeDef &attr, EntityId e, const string &v) {
	auto current = KnownEmpty(e, attr.Local()) ? vector<Datom>() : CurrentDatoms(e, attr.Local());
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
	if (KnownEmpty(e, attr.Local())) {
		return;
	}
	for (auto &datom : CurrentDatoms(e, attr.Local())) {
		if (datom.v == v) {
			RetractDatom(attr, datom);
		}
	}
}

void TxWriter::RetractAll(const AttributeDef &attr, EntityId e) {
	if (KnownEmpty(e, attr.Local())) {
		return;
	}
	for (auto &datom : CurrentDatoms(e, attr.Local())) {
		RetractDatom(attr, datom);
	}
}

void TxWriter::RetractDatom(const AttributeDef &attr, const Datom &datom) {
	Datom log_entry = datom;
	log_entry.t = tx;
	auto log_key = EncodeKey(IndexKind::TXLOG, log_entry);
	count_deltas[datom.a]--;
	if (datom.t == tx) {
		// Asserted earlier in this transaction: it never becomes visible, so it
		// leaves no history
		for (auto &index : IndexesOf(attr)) {
			Delete(index.first, EncodeKey(IndexKindOf(index.first), datom));
		}
		Delete(ColumnFamily::TXLOG, log_key);
		return;
	}
	auto history_value = EncodeHistoryValue(tx);
	touched.insert({datom.e, datom.a});
	retracted.insert({datom.e, datom.a});
	for (auto &index : IndexesOf(attr)) {
		auto key = EncodeKey(IndexKindOf(index.first), datom);
		Delete(index.first, key);
		Put(index.second, key, history_value);
	}
	Put(ColumnFamily::TXLOG, log_key, EncodeTxOp(TxOp::RETRACT));
}

bool TxWriter::UndoRetraction(const AttributeDef &attr, EntityId e, const string &v) {
	// A value retracted earlier in this transaction and asserted again is simply
	// restored
	if (!retracted.count({e, attr.Local()})) {
		return false;
	}
	vector<TxId> retracted_by;
	auto history = view.ScanPrefix(ColumnFamily::EAVT_HIST, EavtPrefix(e, attr.Local()) + v, &retracted_by);
	for (idx_t i = 0; i < history.size(); i++) {
		auto &datom = history[i];
		if (datom.v != v || retracted_by[i] != tx) {
			continue;
		}
		count_deltas[datom.a]++;
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
		auto it = view.NewIterator(history ? ColumnFamily::AEVT_HIST : ColumnFamily::AEVT, prefix);
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

	// The conversion must keep distinct current values distinct, per entity and,
	// for unique attributes, overall
	std::set<std::pair<EntityId, string>> per_entity;
	std::set<string> overall;
	for (auto &entry : entries) {
		if (entry.history) {
			continue;
		}
		if (!per_entity.insert({entry.datom.e, entry.new_v}).second) {
			throw duckdb::InvalidInputException("MnemonicDB: changing the type of \"%s\" would merge two values of "
			                                    "entity %d",
			                                    old_attr.name, entry.datom.e);
		}
		if (new_attr.unique != Uniqueness::NONE && !overall.insert(entry.new_v).second) {
			throw duckdb::ConstraintException("MnemonicDB: changing the type of \"%s\" would make two entities "
			                                  "share a unique value",
			                                  old_attr.name);
		}
	}

	auto history_cfs = [](bool history, ColumnFamily current, ColumnFamily hist) {
		return history ? hist : current;
	};
	for (auto &entry : entries) {
		auto old_datom = entry.datom;
		auto new_datom = entry.datom;
		new_datom.v = entry.new_v;
		auto value = entry.history ? EncodeHistoryValue(entry.retracted_by) : string();
		for (auto &index : IndexesOf(old_attr)) {
			Delete(history_cfs(entry.history, index.first, index.second),
			       EncodeKey(IndexKindOf(index.first), old_datom));
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
		auto it = view.NewIterator(source, prefix);
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
