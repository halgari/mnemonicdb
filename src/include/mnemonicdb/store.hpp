#pragma once

#include "duckdb/common/common.hpp"

#include <atomic>

namespace rocksdb {
class DB;
class ColumnFamilyHandle;
} // namespace rocksdb

namespace mnemonicdb {

using duckdb::string;
using duckdb::unique_ptr;
using duckdb::vector;
using duckdb::idx_t;

//! The RocksDB column families making up a database
enum class ColumnFamily : uint8_t {
	EAVT,
	EAVT_HIST,
	AEVT,
	AEVT_HIST,
	AVET,
	AVET_HIST,
	VAET,
	VAET_HIST,
	TXLOG,
	META,
	COUNT
};

//! On-disk format version written to and checked against `meta`
static constexpr uint32_t FORMAT_VERSION = 1;

const char *ColumnFamilyName(ColumnFamily cf);

//! An open RocksDB database holding one MnemonicDB database
class Store {
public:
	~Store();

	//! Open (or create, unless read_only) the database at `path`
	static unique_ptr<Store> Open(const string &path, bool read_only);

public:
	rocksdb::DB &GetDB() {
		return *db;
	}
	rocksdb::ColumnFamilyHandle &GetHandle(ColumnFamily cf) {
		return *handles[static_cast<idx_t>(cf)];
	}
	const string &GetPath() const {
		return path;
	}
	bool IsReadOnly() const {
		return read_only;
	}
	uint32_t GetFormatVersion() const {
		return format_version;
	}
	//! The last committed transaction number
	uint64_t GetLastTx() const {
		return last_tx.load();
	}
	//! Flush all column families to disk
	void Flush();
	//! Approximate on-disk size in bytes
	uint64_t ApproximateSize();

private:
	Store(string path, bool read_only);
	void LoadMeta(bool created);

private:
	string path;
	bool read_only;
	unique_ptr<rocksdb::DB> db;
	vector<rocksdb::ColumnFamilyHandle *> handles;
	uint32_t format_version = 0;
	std::atomic<uint64_t> last_tx {0};
};

} // namespace mnemonicdb
