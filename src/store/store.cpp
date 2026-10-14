#include "duckdb/common/types/uuid.hpp"
#include "mnemonicdb/store.hpp"
#include "mnemonicdb/encoding.hpp"

#include "duckdb/common/exception.hpp"

#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/options.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/slice_transform.h"
#include "rocksdb/table.h"
#include "rocksdb/write_batch.h"

#include <cstring>

namespace mnemonicdb {

using duckdb::IOException;
using duckdb::MaxValue;

static constexpr const char *META_FORMAT_VERSION = "format_version";
static constexpr const char *META_LAST_TX = "last_tx";
static constexpr const char *META_LAST_INSTANT = "last_instant";
//! Followed by one partition byte
static constexpr const char *META_NEXT_LOCAL = "next_local/";
//! Followed by a 2-byte attribute id: the number of current datoms of the
//! attribute
static constexpr const char *META_COUNT = "count/";
static constexpr const char *META_COUNTS_READY = "counts_ready";

static string CountKey(AttrId attr) {
	string key = META_COUNT;
	PutBigEndian(key, attr, PACKED_ATTR_SIZE);
	return key;
}

//! First local number handed out per partition; lower numbers are reserved for
//! built-in entities
static uint64_t FirstLocal(uint8_t part) {
	switch (part) {
	case partition::ATTR:
		return 64;
	case partition::TABLE:
	case partition::SCHEMA:
		return 16;
	default:
		return 1;
	}
}

static string NextLocalKey(uint8_t part) {
	string key = META_NEXT_LOCAL;
	key.push_back(static_cast<char>(part));
	return key;
}

const char *ColumnFamilyName(ColumnFamily cf) {
	switch (cf) {
	case ColumnFamily::EAVT:
		return "eavt";
	case ColumnFamily::EAVT_HIST:
		return "eavt_hist";
	case ColumnFamily::AEVT:
		return "aevt";
	case ColumnFamily::AEVT_HIST:
		return "aevt_hist";
	case ColumnFamily::AVET:
		return "avet";
	case ColumnFamily::AVET_HIST:
		return "avet_hist";
	case ColumnFamily::VAET:
		return "vaet";
	case ColumnFamily::VAET_HIST:
		return "vaet_hist";
	case ColumnFamily::TXLOG:
		return "txlog";
	case ColumnFamily::META:
		return "meta";
	default:
		throw duckdb::InternalException("Unknown column family");
	}
}

//! Indexes whose keys start with two fixed-width components (E6 A2, A2 E6, V6
//! A2) get an 8-byte prefix extractor with prefix Bloom filters in SST files
//! and memtables. A seek within one (e, a) / (a, e) / (v, a) prefix, which is
//! what every per-datom lookup is, then skips files that cannot contain the
//! prefix. Iterators opt in through iterate_upper_bound + auto_prefix_mode (see
//! ReadView::NewIterator), so seeks spanning several prefixes still use total
//! order. Every key is at least 13 bytes, so no key is shorter than the prefix.
bool HasPrefixBloom(ColumnFamily cf) {
	switch (cf) {
	case ColumnFamily::EAVT:
	case ColumnFamily::EAVT_HIST:
	case ColumnFamily::AEVT:
	case ColumnFamily::AEVT_HIST:
	case ColumnFamily::VAET:
	case ColumnFamily::VAET_HIST:
		return true;
	default:
		return false;
	}
}

static rocksdb::ColumnFamilyOptions ColumnFamilyOptionsFor(ColumnFamily cf) {
	rocksdb::ColumnFamilyOptions options;
	// Datoms repeat a lot (shared key prefixes, values stored in several
	// indexes): LZ4 for the levels that churn, ZSTD for the bottommost level that
	// holds most of the data
	options.compression = rocksdb::kLZ4Compression;
	options.bottommost_compression = rocksdb::kZSTD;
	if (!HasPrefixBloom(cf)) {
		return options;
	}
	rocksdb::BlockBasedTableOptions table_options;
	table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
	// Lookups are always prefix seeks, never Get of a whole key
	table_options.whole_key_filtering = false;
	options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));
	options.prefix_extractor.reset(rocksdb::NewFixedPrefixTransform(BLOOM_PREFIX_LENGTH));
	options.memtable_prefix_bloom_size_ratio = 0.1;
	return options;
}

static void CheckStatus(const rocksdb::Status &status, const string &path) {
	if (!status.ok()) {
		throw IOException("MnemonicDB: RocksDB error on \"%s\": %s", path, status.ToString());
	}
}

Store::Store(string path_p, bool read_only_p)
    : path(std::move(path_p)), read_only(read_only_p), tag(duckdb::UUID::ToString(duckdb::UUID::GenerateRandomUUID())) {
}

Store::~Store() {
	if (db) {
		for (auto handle : handles) {
			db->DestroyColumnFamilyHandle(handle);
		}
		auto status = db->Close();
		(void)status;
	}
}

unique_ptr<Store> Store::Open(const string &path, bool read_only, const StoreOptions &options) {
	auto store = unique_ptr<Store>(new Store(path, read_only));

	rocksdb::DBOptions db_options;
	db_options.create_if_missing = !read_only;
	if (options.max_background_jobs >= 0) {
		db_options.max_background_jobs = options.max_background_jobs;
	}
	if (options.max_file_opening_threads >= 0) {
		db_options.max_file_opening_threads = options.max_file_opening_threads;
	}
	db_options.create_missing_column_families = false;

	// An existing RocksDB directory must already be a MnemonicDB database
	vector<string> existing;
	auto list_status = rocksdb::DB::ListColumnFamilies(db_options, path, &existing);
	bool created = !list_status.ok();
	if (created) {
		if (read_only) {
			throw IOException("MnemonicDB: database \"%s\" does not exist", path);
		}
	} else if (std::find(existing.begin(), existing.end(), "meta") == existing.end()) {
		throw IOException("MnemonicDB: \"%s\" is a RocksDB database but not a "
		                  "MnemonicDB database",
		                  path);
	}
	db_options.create_missing_column_families = created;

	vector<rocksdb::ColumnFamilyDescriptor> descriptors;
	descriptors.emplace_back(rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions());
	for (idx_t i = 0; i < static_cast<idx_t>(ColumnFamily::COUNT); i++) {
		auto cf = static_cast<ColumnFamily>(i);
		descriptors.emplace_back(ColumnFamilyName(cf), ColumnFamilyOptionsFor(cf));
	}

	vector<rocksdb::ColumnFamilyHandle *> all_handles;
	std::unique_ptr<rocksdb::DB> db;
	rocksdb::Status status;
	if (read_only) {
		status = rocksdb::DB::OpenForReadOnly(db_options, path, descriptors, &all_handles, &db);
	} else {
		status = rocksdb::DB::Open(db_options, path, descriptors, &all_handles, &db);
	}
	CheckStatus(status, path);
	store->db = unique_ptr<rocksdb::DB>(db.release());

	// The default column family is unused; keep only ours, indexed by
	// ColumnFamily
	store->db->DestroyColumnFamilyHandle(all_handles[0]);
	store->handles.assign(all_handles.begin() + 1, all_handles.end());

	store->created = created;
	store->LoadMeta(created);
	store->LoadAttributeCounts();
	return store;
}

void Store::ResizeThreadPools(const StoreOptions &options) {
	// Opening a database grows the pools to what its max_background_jobs needs
	// (one flush and one compaction thread at least), so this runs after the
	// open. With no flush thread, RocksDB runs flushes in the compaction pool.
	auto env = rocksdb::Env::Default();
	if (options.num_low_pri_threads >= 0) {
		env->SetBackgroundThreads(options.num_low_pri_threads, rocksdb::Env::Priority::LOW);
	}
	if (options.num_high_pri_threads >= 0) {
		env->SetBackgroundThreads(options.num_high_pri_threads, rocksdb::Env::Priority::HIGH);
	}
}

Store::ThreadPoolSizes Store::GetThreadPoolSizes() {
	auto env = rocksdb::Env::Default();
	return {env->GetBackgroundThreads(rocksdb::Env::Priority::LOW),
	        env->GetBackgroundThreads(rocksdb::Env::Priority::HIGH)};
}

void Store::LoadMeta(bool created) {
	auto &meta = GetHandle(ColumnFamily::META);
	if (created) {
		rocksdb::WriteBatch batch;
		string version;
		PutBigEndian(version, FORMAT_VERSION, 4);
		string tx;
		PutBigEndian(tx, 0, 8);
		batch.Put(&meta, META_FORMAT_VERSION, version);
		batch.Put(&meta, META_LAST_TX, tx);
		rocksdb::WriteOptions write_options;
		write_options.sync = true;
		CheckStatus(batch.Put(&meta, META_COUNTS_READY, ""), path);
		CheckStatus(db->Write(write_options, &batch), path);
	}

	string value;
	auto status = db->Get(rocksdb::ReadOptions(), &meta, META_FORMAT_VERSION, &value);
	if (status.IsNotFound() || value.size() != 4) {
		throw IOException("MnemonicDB: \"%s\" has no valid format version", path);
	}
	CheckStatus(status, path);
	format_version = static_cast<uint32_t>(GetBigEndian(value.data(), 4));
	if (format_version != FORMAT_VERSION) {
		throw IOException("MnemonicDB: \"%s\" has format version %d, this build "
		                  "supports version %d",
		                  path, format_version, FORMAT_VERSION);
	}

	status = db->Get(rocksdb::ReadOptions(), &meta, META_LAST_TX, &value);
	if (status.IsNotFound() || value.size() != 8) {
		throw IOException("MnemonicDB: \"%s\" has no valid last transaction", path);
	}
	CheckStatus(status, path);
	last_tx = GetBigEndian(value.data(), 8);

	status = db->Get(rocksdb::ReadOptions(), &meta, META_LAST_INSTANT, &value);
	if (status.ok() && value.size() == 8) {
		last_instant = static_cast<int64_t>(GetBigEndian(value.data(), 8));
	}

	for (idx_t part = 0; part < next_local.size(); part++) {
		next_local[part] = FirstLocal(static_cast<uint8_t>(part));
	}
	std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(rocksdb::ReadOptions(), &meta));
	for (it->Seek(META_NEXT_LOCAL); it->Valid() && it->key().starts_with(META_NEXT_LOCAL); it->Next()) {
		auto key = it->key();
		auto val = it->value();
		if (key.size() != strlen(META_NEXT_LOCAL) + 1 || val.size() != 8) {
			throw IOException("MnemonicDB: \"%s\" has a malformed ID counter", path);
		}
		auto part = static_cast<uint8_t>(key[key.size() - 1]);
		next_local[part] = GetBigEndian(val.data(), 8);
	}
	CheckStatus(it->status(), path);
}

void Store::LoadAttributeCounts() {
	auto &meta = GetHandle(ColumnFamily::META);
	string ready;
	auto status = db->Get(rocksdb::ReadOptions(), &meta, META_COUNTS_READY, &ready);
	if (status.ok()) {
		std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(rocksdb::ReadOptions(), &meta));
		for (it->Seek(META_COUNT); it->Valid() && it->key().starts_with(META_COUNT); it->Next()) {
			if (it->key().size() != strlen(META_COUNT) + PACKED_ATTR_SIZE || it->value().size() != 8) {
				throw IOException("MnemonicDB: \"%s\" has a malformed attribute count", path);
			}
			auto attr = GetAttrId(it->key().data() + strlen(META_COUNT));
			attribute_counts[attr] = GetBigEndian(it->value().data(), 8);
		}
		CheckStatus(it->status(), path);
		return;
	}
	if (!status.IsNotFound()) {
		CheckStatus(status, path);
	}
	// A database written before counts existed: count the current datoms once
	std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(rocksdb::ReadOptions(), &GetHandle(ColumnFamily::AEVT)));
	for (it->SeekToFirst(); it->Valid(); it->Next()) {
		attribute_counts[GetAttrId(it->key().data())]++;
	}
	CheckStatus(it->status(), path);
	if (read_only) {
		return;
	}
	rocksdb::WriteBatch batch;
	for (auto &entry : attribute_counts) {
		string value;
		PutBigEndian(value, entry.second, 8);
		CheckStatus(batch.Put(&meta, CountKey(entry.first), value), path);
	}
	CheckStatus(batch.Put(&meta, META_COUNTS_READY, ""), path);
	CheckStatus(db->Write(rocksdb::WriteOptions(), &batch), path);
}

uint64_t Store::GetAttributeCount(AttrId attr) const {
	std::lock_guard<std::mutex> guard(count_lock);
	auto entry = attribute_counts.find(attr);
	return entry == attribute_counts.end() ? 0 : entry->second;
}

void Store::AcquireWriter(const std::function<void()> &check_interrupt) {
	if (read_only) {
		throw duckdb::InvalidInputException("MnemonicDB: database \"%s\" is attached read-only", path);
	}
	std::unique_lock<std::mutex> guard(writer_lock);
	while (writer_active) {
		writer_released.wait_for(guard, std::chrono::milliseconds(50));
		if (writer_active && check_interrupt) {
			guard.unlock();
			check_interrupt();
			guard.lock();
		}
	}
	writer_active = true;
}

void Store::ReleaseWriter() {
	{
		std::lock_guard<std::mutex> guard(writer_lock);
		D_ASSERT(writer_active);
		writer_active = false;
	}
	writer_released.notify_one();
}

EntityId Store::AllocateId(uint8_t part) {
	D_ASSERT(writer_active);
	if (part == partition::TEMP || part == partition::TX) {
		throw duckdb::InternalException("cannot allocate IDs in partition %d", part);
	}
	auto local = next_local[part];
	auto limit = (part == partition::ATTR || part == partition::TABLE) ? uint64_t(UINT16_MAX) : LOCAL_MASK;
	if (local > limit) {
		throw duckdb::InvalidInputException("MnemonicDB: partition %d has no IDs left", part);
	}
	next_local[part] = local + 1;
	next_local_dirty[part] = true;
	return MakeEntityId(part, local);
}

Store::CounterState Store::SaveCounters() const {
	return CounterState {next_local, next_local_dirty};
}

void Store::RestoreCounters(const CounterState &state) {
	D_ASSERT(writer_active);
	next_local = state.next_local;
	next_local_dirty = state.next_local_dirty;
}

void Store::CheckAllocated(EntityId id) const {
	auto part = PartitionOf(id);
	auto local = LocalOf(id);
	bool allocated;
	if (!IsWellFormed(id) || part == partition::TEMP) {
		allocated = false;
	} else if (part == partition::TX) {
		// The writing transaction's own entity counts
		allocated = local >= 1 && local <= last_tx.load() + (writer_active ? 1 : 0);
	} else {
		allocated = local < next_local[part];
	}
	if (!allocated) {
		throw duckdb::InvalidInputException("MnemonicDB: entity %d does not exist; new entities get their ids from "
		                                    "tempid() or by omitting id",
		                                    id);
	}
}

void Store::Commit(rocksdb::WriteBatch &batch, TxId tx, int64_t instant,
                   const std::unordered_map<AttrId, int64_t> &count_deltas) {
	D_ASSERT(writer_active);
	if (tx != last_tx + 1) {
		throw duckdb::InternalException("MnemonicDB: committing tx %d after tx %d", tx, last_tx.load());
	}
	auto &meta = GetHandle(ColumnFamily::META);
	string tx_value;
	PutBigEndian(tx_value, tx, 8);
	CheckStatus(batch.Put(&meta, META_LAST_TX, tx_value), path);
	string instant_value;
	PutBigEndian(instant_value, static_cast<uint64_t>(instant), 8);
	CheckStatus(batch.Put(&meta, META_LAST_INSTANT, instant_value), path);
	for (idx_t part = 0; part < next_local.size(); part++) {
		if (!next_local_dirty[part]) {
			continue;
		}
		string value;
		PutBigEndian(value, next_local[part], 8);
		CheckStatus(batch.Put(&meta, NextLocalKey(static_cast<uint8_t>(part)), value), path);
	}

	std::unordered_map<AttrId, uint64_t> new_counts;
	{
		std::lock_guard<std::mutex> guard(count_lock);
		for (auto &delta : count_deltas) {
			if (delta.second == 0) {
				continue;
			}
			auto entry = attribute_counts.find(delta.first);
			int64_t current = entry == attribute_counts.end() ? 0 : int64_t(entry->second);
			new_counts[delta.first] = uint64_t(MaxValue<int64_t>(current + delta.second, 0));
		}
	}
	for (auto &entry : new_counts) {
		string value;
		PutBigEndian(value, entry.second, 8);
		CheckStatus(batch.Put(&meta, CountKey(entry.first), value), path);
	}

	rocksdb::WriteOptions write_options;
	write_options.sync = true;
	CheckStatus(db->Write(write_options, &batch), path);
	next_local_dirty.fill(false);
	{
		std::lock_guard<std::mutex> guard(count_lock);
		for (auto &entry : new_counts) {
			attribute_counts[entry.first] = entry.second;
		}
	}
	last_instant = instant;
	last_tx = tx;
}

void Store::Flush() {
	if (read_only) {
		return;
	}
	rocksdb::FlushOptions options;
	CheckStatus(db->Flush(options, handles), path);
}

void Store::Compact() {
	if (read_only) {
		throw duckdb::InvalidInputException("MnemonicDB: database \"%s\" is attached read-only", path);
	}
	Flush();
	for (auto handle : handles) {
		rocksdb::CompactRangeOptions options;
		options.bottommost_level_compaction = rocksdb::BottommostLevelCompaction::kForce;
		CheckStatus(db->CompactRange(options, handle, nullptr, nullptr), path);
	}
}

uint64_t Store::ApproximateSize() {
	uint64_t total = 0;
	for (auto handle : handles) {
		uint64_t size = 0;
		if (db->GetIntProperty(handle, rocksdb::DB::Properties::kTotalSstFilesSize, &size)) {
			total += size;
		}
	}
	return total;
}

} // namespace mnemonicdb
