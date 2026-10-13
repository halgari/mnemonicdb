#include "mnemonicdb/mnemonic_transaction_manager.hpp"
#include "mnemonicdb/mnemonic_catalog.hpp"

namespace duckdb {

MnemonicTransactionManager::MnemonicTransactionManager(AttachedDatabase &db, MnemonicCatalog &catalog)
    : TransactionManager(db), catalog(catalog) {
}

Transaction &MnemonicTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<MnemonicTransaction>(*this, context, catalog.GetStore());
	auto &result = *transaction;
	lock_guard<mutex> guard(transaction_lock);
	transactions[result] = std::move(transaction);
	return result;
}

ErrorData MnemonicTransactionManager::CommitTransaction(ClientContext &, Transaction &transaction) {
	// Nothing is written yet; committing just releases the snapshot
	lock_guard<mutex> guard(transaction_lock);
	transactions.erase(transaction);
	return ErrorData();
}

void MnemonicTransactionManager::RollbackTransaction(Transaction &transaction) {
	lock_guard<mutex> guard(transaction_lock);
	transactions.erase(transaction);
}

void MnemonicTransactionManager::Checkpoint(ClientContext &, bool) {
	catalog.GetStore().Flush();
}

} // namespace duckdb
