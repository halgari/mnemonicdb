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

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/timestamp.hpp"

#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/storage/storage_extension.hpp"

#include <algorithm>

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

//! A database created by an older version lacks the datoms describing built-ins
//! added since (e.g. sys.schemas and its `main`/`sys` rows). On a read-write
//! attach, write whichever are missing, once, in one transaction. Asserting a
//! datom that is already current is a no-op, so this rewrites nothing that is
//! there; when nothing is missing no transaction is written.
static void UpgradeBuiltins(mnemonicdb::Store &store) {
	// The backfill writes the built-in schema entities in partition 4. No
	// version let users write there, but refuse to merge into anything else
	{
		mnemonicdb::ReadView view(store, nullptr);
		auto builtin = mnemonicdb::Schema::Builtin();
		for (auto schema_def : builtin->AllSchemas()) {
			for (auto &datom :
			     view.ScanPrefix(mnemonicdb::ColumnFamily::EAVT, mnemonicdb::EavtPrefix(schema_def->id))) {
				bool expected =
				    datom.a == mnemonicdb::sys::SCHEMAS_DOC ||
				    (datom.a == mnemonicdb::sys::SCHEMAS_NAME &&
				     builtin->GetAttribute(datom.a).codec.DecodeValue(datom.v).ToString() == schema_def->name);
				if (!expected) {
					throw IOException("MnemonicDB: cannot upgrade \"%s\": entity %d, reserved for the built-in schema "
					                  "\"%s\", already holds other data (attribute %d)",
					                  store.GetPath(), schema_def->id, schema_def->name, datom.a);
				}
			}
		}
	}
	store.AcquireWriter();
	try {
		auto tx = store.GetLastTx() + 1;
		mnemonicdb::TxWriter writer(store, nullptr, tx);
		mnemonicdb::Schema::WriteBootstrap(writer);
		writer.FinishStatement();
		if (writer.HasChanges()) {
			auto instant = MaxValue<int64_t>(Timestamp::GetCurrentTimestamp().value, store.GetLastInstant() + 1);
			mnemonicdb::Schema::WriteTxEntity(writer, *mnemonicdb::Schema::Builtin(), tx, instant);
			store.Commit(writer.GetWriteBatch(), tx, instant, writer.GetCountDeltas());
		}
	} catch (...) {
		store.ReleaseWriter();
		throw;
	}
	store.ReleaseWriter();
}

//! The MnemonicDB ATTACH options (beyond DuckDB's own TYPE, READ_ONLY, ...):
//! RocksDB thread settings, named after the RocksDB options they set
static mnemonicdb::StoreOptions ParseStoreOptions(const unordered_map<string, Value> &attach_options) {
	struct ThreadOption {
		const char *name;
		int mnemonicdb::StoreOptions::*field;
		int64_t min;
	};
	// Each thread counts against the process's (or container's) thread limit,
	// and the pools are process-wide, so keep a typo from starting thousands
	static constexpr int64_t MAX_THREADS = 256;
	static const ThreadOption THREAD_OPTIONS[] = {
	    {"max_background_jobs", &mnemonicdb::StoreOptions::max_background_jobs, 1},
	    {"max_file_opening_threads", &mnemonicdb::StoreOptions::max_file_opening_threads, 1},
	    {"num_low_pri_threads", &mnemonicdb::StoreOptions::num_low_pri_threads, 1},
	    {"num_high_pri_threads", &mnemonicdb::StoreOptions::num_high_pri_threads, 0},
	};
	mnemonicdb::StoreOptions result;
	for (auto &entry : attach_options) {
		auto key = StringUtil::Lower(entry.first);
		const ThreadOption *option = nullptr;
		for (auto &candidate : THREAD_OPTIONS) {
			if (key == candidate.name) {
				option = &candidate;
			}
		}
		if (!option) {
			throw BinderException("MnemonicDB: unrecognized ATTACH option \"%s\"", entry.first);
		}
		auto upper = StringUtil::Upper(key);
		auto &input = entry.second;
		// An integer, or a string of decimal digits; never a rounded fraction
		bool integral = !input.IsNull() && input.type().IsIntegral();
		string text = input.IsNull() ? "NULL" : input.ToString();
		if (!input.IsNull() && input.type().id() == LogicalTypeId::VARCHAR) {
			StringUtil::Trim(text);
			auto digits = !text.empty() && text[0] == '-' ? text.substr(1) : text;
			integral = !digits.empty() &&
			           std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; });
		}
		if (!integral) {
			throw BinderException("MnemonicDB: ATTACH option %s must be an integer, got \"%s\"", upper, text);
		}
		// An integer too large for BIGINT is out of range too
		auto value = input.DefaultTryCastAs(LogicalType::BIGINT);
		auto n = value && !value->IsNull() ? value->GetValue<int64_t>() : NumericLimits<int64_t>::Maximum();
		if (n < option->min || n > MAX_THREADS) {
			throw BinderException("MnemonicDB: ATTACH option %s must be between %d and %d, got %s", upper, option->min,
			                      MAX_THREADS, text);
		}
		result.*(option->field) = static_cast<int>(n);
	}
	return result;
}

static unique_ptr<Catalog> MnemonicAttach(optional_ptr<StorageExtensionInfo>, ClientContext &context,
                                          AttachedDatabase &db, const string &, AttachInfo &info,
                                          AttachOptions &options) {
	bool read_only = options.access_mode == AccessMode::READ_ONLY;
	auto store_options = ParseStoreOptions(options.options);
	auto store = mnemonicdb::Store::Open(info.path, read_only, store_options);
	if (store->WasCreated()) {
		Bootstrap(*store);
	} else if (!read_only) {
		UpgradeBuiltins(*store);
	}
	mnemonicdb::ReadView view(*store, nullptr);
	auto schema = mnemonicdb::Schema::Load(view, context);
	auto catalog = make_uniq<MnemonicCatalog>(db, std::move(store), std::move(schema));
	// Last, once nothing can fail: the pools are process-wide
	mnemonicdb::Store::ResizeThreadPools(store_options);
	return std::move(catalog);
}

static unique_ptr<TransactionManager> MnemonicCreateTransactionManager(optional_ptr<StorageExtensionInfo>,
                                                                       AttachedDatabase &db, Catalog &catalog) {
	return make_uniq<MnemonicTransactionManager>(db, catalog.Cast<MnemonicCatalog>());
}

namespace {

class MnemonicStorageExtension : public StorageExtension {
public:
	MnemonicStorageExtension() {
		attach = MnemonicAttach;
		create_transaction_manager = MnemonicCreateTransactionManager;
	}
};

} // namespace

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
