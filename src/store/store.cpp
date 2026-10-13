#include "mnemonicdb/store.hpp"
#include "mnemonicdb/encoding.hpp"

#include "duckdb/common/exception.hpp"

#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/write_batch.h"

#include <cstring>

namespace mnemonicdb {

using duckdb::IOException;

static constexpr const char *META_FORMAT_VERSION = "format_version";
static constexpr const char *META_LAST_TX = "last_tx";
static constexpr const char *META_LAST_INSTANT = "last_instant";
//! Followed by one partition byte
static constexpr const char *META_NEXT_LOCAL = "next_local/";

//! First local number handed out per partition; lower numbers are reserved for built-in entities
static uint64_t FirstLocal(uint8_t part) {
	switch (part) {
	case partition::ATTR:
		return 64;
	case partition::TABLE:
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

static void CheckStatus(const rocksdb::Status &status, const string &path) {
	if (!status.ok()) {
		throw IOException("MnemonicDB: RocksDB error on \"%s\": %s", path, status.ToString());
	}
}

Store::Store(string path_p, bool read_only_p) : path(std::move(path_p)), read_only(read_only_p) {
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

unique_ptr<Store> Store::Open(const string &path, bool read_only) {
	auto store = unique_ptr<Store>(new Store(path, read_only));

	rocksdb::DBOptions db_options;
	db_options.create_if_missing = !read_only;
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
		throw IOException("MnemonicDB: \"%s\" is a RocksDB database but not a MnemonicDB database", path);
	}
	db_options.create_missing_column_families = created;

	vector<rocksdb::ColumnFamilyDescriptor> descriptors;
	descriptors.emplace_back(rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions());
	for (idx_t i = 0; i < static_cast<idx_t>(ColumnFamily::COUNT); i++) {
		descriptors.emplace_back(ColumnFamilyName(static_cast<ColumnFamily>(i)), rocksdb::ColumnFamilyOptions());
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

	// The default column family is unused; keep only ours, indexed by ColumnFamily
	store->db->DestroyColumnFamilyHandle(all_handles[0]);
	store->handles.assign(all_handles.begin() + 1, all_handles.end());

	store->created = created;
	store->LoadMeta(created);
	return store;
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
		throw IOException("MnemonicDB: \"%s\" has format version %d, this build supports version %d", path,
		                  format_version, FORMAT_VERSION);
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

void Store::AcquireWriter() {
	if (read_only) {
		throw duckdb::InvalidInputException("MnemonicDB: database \"%s\" is attached read-only", path);
	}
	std::unique_lock<std::mutex> guard(writer_lock);
	writer_released.wait(guard, [&] { return !writer_active; });
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

void Store::Commit(rocksdb::WriteBatch &batch, TxId tx, int64_t instant) {
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

	rocksdb::WriteOptions write_options;
	write_options.sync = true;
	CheckStatus(db->Write(write_options, &batch), path);
	next_local_dirty.fill(false);
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
