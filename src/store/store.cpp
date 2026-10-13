#include "mnemonicdb/store.hpp"
#include "mnemonicdb/encoding.hpp"

#include "duckdb/common/exception.hpp"

#include "rocksdb/db.h"
#include "rocksdb/options.h"

namespace mnemonicdb {

using duckdb::IOException;

static constexpr const char *META_FORMAT_VERSION = "format_version";
static constexpr const char *META_LAST_TX = "last_tx";

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
