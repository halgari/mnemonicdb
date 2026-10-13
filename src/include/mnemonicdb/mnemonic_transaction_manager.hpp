#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "mnemonicdb/mnemonic_transaction.hpp"

namespace duckdb {

class MnemonicCatalog;

class MnemonicTransactionManager : public TransactionManager {
public:
	MnemonicTransactionManager(AttachedDatabase &db, MnemonicCatalog &catalog);

public:
	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;
	void Checkpoint(ClientContext &context, bool force = false) override;

private:
	MnemonicCatalog &catalog;
	mutex transaction_lock;
	reference_map_t<Transaction, unique_ptr<MnemonicTransaction>> transactions;
};

} // namespace duckdb
