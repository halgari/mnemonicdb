#include "mnemonicdb/mnemonicdb_info.hpp"
#include "mnemonicdb/mnemonic_catalog.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace duckdb {

namespace {

struct MnemonicInfoBindData : public TableFunctionData {
	explicit MnemonicInfoBindData(MnemonicCatalog &catalog) : catalog(catalog) {
	}
	MnemonicCatalog &catalog;
};

struct MnemonicInfoState : public GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<FunctionData> MnemonicInfoBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto name = input.inputs[0].GetValue<string>();
	auto &catalog = Catalog::GetCatalog(context, Identifier(name));
	if (catalog.GetCatalogType() != MnemonicCatalog::CATALOG_TYPE) {
		throw BinderException("mnemonicdb_info: \"%s\" is not a MnemonicDB database", name);
	}
	names = {
	    "path", "format_version", "last_tx", "read_only", "column_families", "low_pri_threads", "high_pri_threads"};
	return_types = {LogicalType::VARCHAR,
	                LogicalType::UINTEGER,
	                LogicalType::UBIGINT,
	                LogicalType::BOOLEAN,
	                LogicalType::LIST(LogicalType::VARCHAR),
	                LogicalType::INTEGER,
	                LogicalType::INTEGER};
	return make_uniq<MnemonicInfoBindData>(catalog.Cast<MnemonicCatalog>());
}

static unique_ptr<GlobalTableFunctionState> MnemonicInfoInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<MnemonicInfoState>();
}

static void MnemonicInfoFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<MnemonicInfoState>();
	if (state.done) {
		return;
	}
	state.done = true;
	auto &store = input.bind_data->Cast<MnemonicInfoBindData>().catalog.GetStore();
	vector<Value> column_families;
	for (idx_t i = 0; i < static_cast<idx_t>(mnemonicdb::ColumnFamily::COUNT); i++) {
		column_families.emplace_back(mnemonicdb::ColumnFamilyName(static_cast<mnemonicdb::ColumnFamily>(i)));
	}
	output.SetValue(0, 0, Value(store.GetPath()));
	output.SetValue(1, 0, Value::UINTEGER(store.GetFormatVersion()));
	output.SetValue(2, 0, Value::UBIGINT(store.GetLastTx()));
	output.SetValue(3, 0, Value::BOOLEAN(store.IsReadOnly()));
	output.SetValue(4, 0, Value::LIST(LogicalType::VARCHAR, std::move(column_families)));
	// RocksDB's process-wide compaction / flush pools (see DESIGN.md §4)
	auto pools = mnemonicdb::Store::GetThreadPoolSizes();
	output.SetValue(5, 0, Value::INTEGER(pools.low_pri));
	output.SetValue(6, 0, Value::INTEGER(pools.high_pri));
	output.SetChildCardinality(1);
}

struct MnemonicCompactState : public GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<FunctionData> MnemonicCompactBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto name = input.inputs[0].GetValue<string>();
	auto &catalog = Catalog::GetCatalog(context, Identifier(name));
	if (catalog.GetCatalogType() != MnemonicCatalog::CATALOG_TYPE) {
		throw BinderException("mnemonicdb_compact: \"%s\" is not a MnemonicDB database", name);
	}
	names = {Identifier("size_before"), Identifier("size_after")};
	return_types = {LogicalType::UBIGINT, LogicalType::UBIGINT};
	return make_uniq<MnemonicInfoBindData>(catalog.Cast<MnemonicCatalog>());
}

static unique_ptr<GlobalTableFunctionState> MnemonicCompactInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<MnemonicCompactState>();
}

//! Compaction changes no datom, so it does not take the writer lock (writers
//! keep going while it runs), but it is expensive: like a write, it needs a
//! read-write transaction and a read-write attach. The errors use the wording
//! of DuckDB's own checks, which mnemonicdb_add / mnemonicdb_retract get
//! (ClientContext::CheckIfPreparedStatementIsExecutable,
//! MetaTransaction::ModifyDatabase). Checked when the function runs, as a
//! prepared statement can run in a later transaction.
static void CheckCompactAllowed(ClientContext &context, MnemonicCatalog &catalog) {
	// Formatted like DuckDB's own messages, which pass the Identifier
	auto &name = catalog.GetName();
	if (catalog.GetAttached().IsReadOnly()) {
		throw InvalidInputException("Cannot execute statement of type \"mnemonicdb_compact\" on database %s which is "
		                            "attached in read-only mode!",
		                            name);
	}
	if (MetaTransaction::Get(context).IsReadOnly()) {
		throw TransactionException("Cannot write to database \"%s\" - transaction is launched in read-only mode", name);
	}
}

static void MnemonicCompactFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<MnemonicCompactState>();
	if (state.done) {
		return;
	}
	state.done = true;
	auto &catalog = input.bind_data->Cast<MnemonicInfoBindData>().catalog;
	CheckCompactAllowed(context, catalog);
	auto &store = catalog.GetStore();
	auto before = store.ApproximateSize();
	store.Compact();
	output.data[0].Append(Value::UBIGINT(before));
	output.data[1].Append(Value::UBIGINT(store.ApproximateSize()));
}

} // namespace

TableFunction MnemonicCompactTableFunction() {
	return TableFunction("mnemonicdb_compact", {LogicalType::VARCHAR}, MnemonicCompactFunction, MnemonicCompactBind,
	                     MnemonicCompactInit);
}

TableFunction MnemonicInfoTableFunction() {
	return TableFunction("mnemonicdb_info", {LogicalType::VARCHAR}, MnemonicInfoFunction, MnemonicInfoBind,
	                     MnemonicInfoInit);
}

} // namespace duckdb
