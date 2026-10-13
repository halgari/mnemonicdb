#pragma once

#include "duckdb/transaction/transaction.hpp"

namespace rocksdb {
class Snapshot;
}

namespace mnemonicdb {
class Store;
}

namespace duckdb {

class MnemonicTransactionManager;

class MnemonicTransaction : public Transaction {
public:
	MnemonicTransaction(MnemonicTransactionManager &manager, ClientContext &context, mnemonicdb::Store &store);
	~MnemonicTransaction() override;

public:
	static MnemonicTransaction &Get(ClientContext &context, Catalog &catalog);

	//! The RocksDB snapshot every read in this transaction goes through
	const rocksdb::Snapshot &GetSnapshot() const {
		return *snapshot;
	}
	//! The last transaction committed when this transaction began
	uint64_t GetBasisTx() const {
		return basis_tx;
	}

private:
	mnemonicdb::Store &store;
	const rocksdb::Snapshot *snapshot;
	uint64_t basis_tx;
};

} // namespace duckdb
