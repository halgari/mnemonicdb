#include "mnemonicdb/mnemonic_transaction.hpp"
#include "mnemonicdb/mnemonic_transaction_manager.hpp"
#include "mnemonicdb/store.hpp"

#include "rocksdb/db.h"

namespace duckdb {

MnemonicTransaction::MnemonicTransaction(MnemonicTransactionManager &manager, ClientContext &context,
                                         mnemonicdb::Store &store_p)
    : Transaction(manager, context), store(store_p) {
	// Read the last tx before taking the snapshot so the snapshot covers at least that tx
	basis_tx = store.GetLastTx();
	snapshot = store.GetDB().GetSnapshot();
}

MnemonicTransaction::~MnemonicTransaction() {
	store.GetDB().ReleaseSnapshot(snapshot);
}

MnemonicTransaction &MnemonicTransaction::Get(ClientContext &context, Catalog &catalog) {
	return Transaction::Get(context, catalog).Cast<MnemonicTransaction>();
}

} // namespace duckdb
