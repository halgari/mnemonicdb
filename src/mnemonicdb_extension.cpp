#define DUCKDB_EXTENSION_MAIN

#include "mnemonicdb_extension.hpp"
#include "mnemonicdb/codec_functions.hpp"
#include "mnemonicdb/mnemonic_datom_functions.hpp"
#include "mnemonicdb/mnemonic_fusion.hpp"
#include "mnemonicdb/mnemonic_history.hpp"
#include "mnemonicdb/mnemonic_catalog.hpp"
#include "mnemonicdb/mnemonic_transaction_manager.hpp"
#include "mnemonicdb/mnemonicdb_info.hpp"
#include "mnemonicdb/tx_writer.hpp"

#include "duckdb/common/types/timestamp.hpp"

#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

//! Write the first transaction of a new database: the datoms describing the sys
//! tables
static void Bootstrap(mnemonicdb::Store &store) {
	store.AcquireWriter();
	try {
		mnemonicdb::TxWriter writer(store, nullptr, 1);
		mnemonicdb::Schema::WriteBootstrap(writer);
		auto instant = Timestamp::GetCurrentTimestamp().value;
		mnemonicdb::Schema::WriteTxEntity(writer, *mnemonicdb::Schema::Builtin(), 1, instant);
		store.Commit(writer.GetWriteBatch(), 1, instant, writer.GetCountDeltas());
	} catch (...) {
		store.ReleaseWriter();
		throw;
	}
	store.ReleaseWriter();
}

static unique_ptr<Catalog> MnemonicAttach(optional_ptr<StorageExtensionInfo>, ClientContext &context,
                                          AttachedDatabase &db, const string &, AttachInfo &info,
                                          AttachOptions &options) {
	bool read_only = options.access_mode == AccessMode::READ_ONLY;
	auto store = mnemonicdb::Store::Open(info.path, read_only);
	if (store->WasCreated()) {
		Bootstrap(*store);
	}
	mnemonicdb::ReadView view(*store, nullptr);
	auto schema = mnemonicdb::Schema::Load(view, context);
	return make_uniq<MnemonicCatalog>(db, std::move(store), std::move(schema));
}

static unique_ptr<TransactionManager> MnemonicCreateTransactionManager(optional_ptr<StorageExtensionInfo>,
                                                                       AttachedDatabase &db, Catalog &catalog) {
	return make_uniq<MnemonicTransactionManager>(db, catalog.Cast<MnemonicCatalog>());
}

class MnemonicStorageExtension : public StorageExtension {
public:
	MnemonicStorageExtension() {
		attach = MnemonicAttach;
		create_transaction_manager = MnemonicCreateTransactionManager;
	}
};

static void LoadInternal(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	StorageExtension::Register(config, "mnemonicdb", make_shared_ptr<MnemonicStorageExtension>());
	loader.RegisterFunction(MnemonicInfoTableFunction());
	loader.RegisterFunction(MnemonicCompactTableFunction());
	RegisterCodecFunctions(loader);
	RegisterDatomFunctions(loader);
	RegisterScanFusion(loader);
	RegisterHistoryFunction(loader);
}

void MnemonicdbExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string MnemonicdbExtension::Name() {
	return "mnemonicdb";
}

std::string MnemonicdbExtension::Version() const {
#ifdef EXT_VERSION_MNEMONICDB
	return EXT_VERSION_MNEMONICDB;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(mnemonicdb, loader) {
	duckdb::LoadInternal(loader);
}
}
