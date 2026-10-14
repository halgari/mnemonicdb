#include "mnemonicdb/mnemonic_transaction_manager.hpp"
#include "mnemonicdb/mnemonic_catalog.hpp"

namespace duckdb {

MnemonicTransactionManager::MnemonicTransactionManager(AttachedDatabase &db, MnemonicCatalog &catalog)
    : TransactionManager(db), catalog(catalog) {
}

Transaction &MnemonicTransactionManager::StartTransaction(ClientContext &context) {
	unique_ptr<MnemonicTransaction> transaction;
	auto parent = catalog.FindAssertionParent(context);
	if (parent) {
		transaction = make_uniq<MnemonicTransaction>(*this, context, catalog, *parent);
	} else {
		transaction = make_uniq<MnemonicTransaction>(*this, context, catalog);
	}
	auto &result = *transaction;
	lock_guard<mutex> guard(transaction_lock);
	transactions[result] = std::move(transaction);
	return result;
}

ErrorData MnemonicTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	ErrorData error;
	try {
		transaction.Cast<MnemonicTransaction>().Commit(context);
	} catch (std::exception &ex) {
		error = ErrorData(ex);
	}
	lock_guard<mutex> guard(transaction_lock);
	transactions.erase(transaction);
	return error;
}

void MnemonicTransactionManager::RollbackTransaction(Transaction &transaction) {
	lock_guard<mutex> guard(transaction_lock);
	transactions.erase(transaction);
}

void MnemonicTransactionManager::Checkpoint(ClientContext &, bool) {
	catalog.GetStore().Flush();
}

} // namespace duckdb
