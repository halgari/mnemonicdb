#pragma once

#include "duckdb/common/common.hpp"
#include "mnemonicdb/column_family.hpp"
#include "mnemonicdb/entity_id.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace rocksdb {
class DB;
class ColumnFamilyHandle;
class WriteBatch;
} // namespace rocksdb

namespace mnemonicdb {

using duckdb::idx_t;
using duckdb::string;
using duckdb::unique_ptr;
using duckdb::vector;

//! On-disk format version written to and checked against `meta`
static constexpr uint32_t FORMAT_VERSION = 1;

//! RocksDB threading settings, from the ATTACH options (see DESIGN.md §4).
//! A negative value keeps RocksDB's default.
struct StoreOptions {
	//! DBOptions::max_background_jobs: concurrent flushes + compactions of this
	//! database
	int max_background_jobs = -1;
	//! DBOptions::max_file_opening_threads: threads opening SST files at open
	int max_file_opening_threads = -1;
	//! Env::SetBackgroundThreads(n, LOW / HIGH): the compaction and flush thread
	//! pools. These pools belong to RocksDB's default Env and are shared by every
	//! RocksDB database in the process; applied by ResizeThreadPools, not Open.
	int num_low_pri_threads = -1;
	int num_high_pri_threads = -1;
};

//! An open RocksDB database holding one MnemonicDB database
class Store {
public:
	~Store();

	//! Open (or create, unless read_only) the database at `path`
	static unique_ptr<Store> Open(const string &path, bool read_only, const StoreOptions &options = StoreOptions());

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
	//! A random token identifying this open of the database within the process
	const string &GetTag() const {
		return tag;
	}
	uint32_t GetFormatVersion() const {
		return format_version;
	}
	//! Whether Open created a new, empty database (which still needs
	//! bootstrapping)
	bool WasCreated() const {
		return created;
	}
	//! The last committed transaction number
	TxId GetLastTx() const {
		return last_tx.load();
	}
	//! The instant (microseconds since epoch) of the last committed transaction
	int64_t GetLastInstant() const {
		return last_instant.load();
	}

	//! Block until this caller is the only writer; paired with ReleaseWriter,
	//! possibly from another thread. `check_interrupt` is called periodically
	//! while waiting and may throw to give up.
	void AcquireWriter(const std::function<void()> &check_interrupt = nullptr);
	void ReleaseWriter();

	//! Allocate a new entity ID in `part`. Only the current writer may allocate.
	//! IDs allocated by a transaction that rolls back are simply never used.
	EntityId AllocateId(uint8_t part);
	//! The ID counters, saved so a failed statement or a rolled-back transaction
	//! can give its IDs back
	struct CounterState {
		std::array<uint64_t, 256> next_local;
		std::array<bool, 256> next_local_dirty;
	};
	CounterState SaveCounters() const;
	void RestoreCounters(const CounterState &state);

	//! Throw unless `id` names an entity that was allocated (explicit ids and REF
	//! targets must exist; they never move the counters, so one bad value cannot
	//! exhaust a partition)
	void CheckAllocated(EntityId id) const;

	//! Atomically write `batch` as transaction `tx`, together with the updated
	//! metadata. Only the current writer may commit, and `tx` must be GetLastTx()
	//! + 1. `count_deltas` is the change in current datoms per attribute.
	void Commit(rocksdb::WriteBatch &batch, TxId tx, int64_t instant,
	            const std::unordered_map<AttrId, int64_t> &count_deltas = {});

	//! Number of current datoms of an attribute, as of the last commit (a planner
	//! estimate)
	uint64_t GetAttributeCount(AttrId attr) const;
	//! Flush all column families to disk
	void Flush();
	//! Compact every column family down to the bottommost level (after large
	//! loads)
	void Compact();
	//! Approximate on-disk size in bytes
	uint64_t ApproximateSize();

	//! Apply the pool sizes in `options` to RocksDB's process-wide flush and
	//! compaction pools (when set)
	static void ResizeThreadPools(const StoreOptions &options);
	struct ThreadPoolSizes {
		int low_pri;
		int high_pri;
	};
	//! The current sizes of those pools
	static ThreadPoolSizes GetThreadPoolSizes();

private:
	Store(string path, bool read_only);
	void LoadMeta(bool created);
	void LoadAttributeCounts();

private:
	string path;
	bool read_only;
	string tag;
	unique_ptr<rocksdb::DB> db;
	vector<rocksdb::ColumnFamilyHandle *> handles;
	bool created = false;
	uint32_t format_version = 0;
	std::atomic<TxId> last_tx {0};
	std::atomic<int64_t> last_instant {0};

	std::mutex writer_lock;
	std::condition_variable writer_released;
	bool writer_active = false;

	//! Next local number per partition, and which ones changed since the last
	//! commit
	std::array<uint64_t, 256> next_local {};
	std::array<bool, 256> next_local_dirty {};

	mutable std::mutex count_lock;
	std::unordered_map<AttrId, uint64_t> attribute_counts;
};

} // namespace mnemonicdb
