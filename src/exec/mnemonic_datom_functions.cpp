#include "mnemonicdb/mnemonic_datom_functions.hpp"
#include "mnemonicdb/mnemonic_table_entry.hpp"
#include "mnemonicdb/mnemonic_transaction.hpp"
#include "mnemonicdb/tempid.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/parser/qualified_name.hpp"

namespace duckdb {

struct DatomFunctionData : public TableFunctionData {
	DatomFunctionData(MnemonicTableEntry &table, const mnemonicdb::AttributeDef &attr, mnemonicdb::EntityId entity,
	                  Value value)
	    : table(table), attr(attr), entity(entity), value(std::move(value)) {
	}
	MnemonicTableEntry &table;
	const mnemonicdb::AttributeDef &attr;
	mnemonicdb::EntityId entity;
	Value value;
};

struct DatomFunctionState : public GlobalTableFunctionState {
	bool done = false;
};

template <bool ASSERT>
static unique_ptr<FunctionData> DatomBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto function_name = ASSERT ? "mnemonicdb_add" : "mnemonicdb_retract";
	for (idx_t i = 0; i < 3; i++) {
		if (input.inputs[i].IsNull()) {
			throw BinderException("%s: arguments cannot be NULL", function_name);
		}
	}
	auto qualified = QualifiedName::Parse(input.inputs[0].GetValue<string>());
	optional_ptr<CatalogEntry> found;
	if (qualified.Catalog().empty() && !qualified.Schema().empty()) {
		// "db.table": try the name as catalog.table in the default schema first
		auto catalog = Catalog::GetCatalogEntry(context, qualified.Schema());
		if (catalog) {
			found = catalog->GetEntry(context, CatalogType::TABLE_ENTRY, Identifier("main"), qualified.Name(),
			                          OnEntryNotFound::RETURN_NULL);
		}
	}
	auto &entry = found ? *found : Catalog::GetEntry(context, CatalogType::TABLE_ENTRY, qualified);
	if (entry.ParentCatalog().GetCatalogType() != "mnemonicdb") {
		throw BinderException("%s: \"%s\" is not a MnemonicDB table", function_name, entry.name);
	}
	auto &table = entry.Cast<MnemonicTableEntry>();
	if (table.GetTableDef().schema != "main") {
		throw BinderException("%s: cannot write to sys tables", function_name);
	}
	auto entity = input.inputs[1].DefaultCastAs(LogicalType::UBIGINT).GetValue<uint64_t>();
	if (!mnemonicdb::IsWellFormed(entity) || mnemonicdb::IsTempid(entity) ||
	    mnemonicdb::PartitionOf(entity) < mnemonicdb::partition::DB) {
		throw BinderException("%s: %d is not a valid entity ID", function_name, entity);
	}
	auto column = input.inputs[2].GetValue<string>();
	auto attr = table.GetMnemonicSchema().FindAttribute(table.GetTableDef(), column);
	if (!attr) {
		throw BinderException("%s: table \"%s\" has no column \"%s\"", function_name, table.name, column);
	}
	auto value = input.inputs[3];
	if (value.IsNull()) {
		throw BinderException("%s: the value cannot be NULL", function_name);
	}
	value = value.DefaultCastAs(attr->value_type);

	// Writing outside DML: take the writer the way a modifying statement would
	auto &transaction = MnemonicTransaction::Get(context, table.catalog);
	if (transaction.IsReadOnly()) {
		transaction.SetReadWrite();
	}
	names = {Identifier("changed")};
	return_types = {LogicalType::BOOLEAN};
	return make_uniq<DatomFunctionData>(table, *attr, entity, std::move(value));
}

static unique_ptr<GlobalTableFunctionState> DatomInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<DatomFunctionState>();
}

template <bool ASSERT>
static void DatomFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<DatomFunctionState>();
	if (state.done) {
		return;
	}
	state.done = true;
	auto &data = input.bind_data->Cast<DatomFunctionData>();
	auto &writer = MnemonicTransaction::Get(context, data.table.catalog).GetWriter();
	string v;
	if (data.attr.is_ref) {
		mnemonicdb::PutEntityId(v, data.value.GetValue<uint64_t>());
	} else {
		v = data.attr.codec.EncodeValue(data.value);
	}
	auto before = writer.CurrentDatoms(data.entity, data.attr.Local());
	if (ASSERT) {
		writer.Assert(data.attr, data.entity, v);
	} else {
		writer.Retract(data.attr, data.entity, v);
	}
	auto after = writer.CurrentDatoms(data.entity, data.attr.Local());
	output.data[0].Append(Value::BOOLEAN(before != after));
}

//! current_tx('db'): the sys.tx entity of the current, writing transaction
static void CurrentTxFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	UnaryExecutor::Execute<string_t, uint64_t>(args.data[0], result, args.size(), [&](string_t name) {
		auto &catalog = Catalog::GetCatalog(context, Identifier(name.GetString()));
		if (catalog.GetCatalogType() != "mnemonicdb") {
			throw InvalidInputException("current_tx: \"%s\" is not a MnemonicDB database", name.GetString());
		}
		auto &transaction = MnemonicTransaction::Get(context, catalog);
		if (transaction.IsReadOnly()) {
			throw InvalidInputException("current_tx: the transaction has not written to \"%s\"; use current_tx() "
			                            "in a statement that writes, or after one in the same transaction",
			                            name.GetString());
		}
		return transaction.CurrentTxEntity();
	});
}

void RegisterDatomFunctions(ExtensionLoader &loader) {
	ScalarFunction current_tx("current_tx", {LogicalType::VARCHAR}, mnemonicdb::RefType(), CurrentTxFunction);
	current_tx.SetStability(FunctionStability::VOLATILE);
	current_tx.SetFallible();
	loader.RegisterFunction(current_tx);

	for (auto assert_value : {true, false}) {
		TableFunction function(assert_value ? "mnemonicdb_add" : "mnemonicdb_retract",
		                       {LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::VARCHAR, LogicalType::ANY},
		                       assert_value ? DatomFunction<true> : DatomFunction<false>,
		                       assert_value ? DatomBind<true> : DatomBind<false>, DatomInit);
		loader.RegisterFunction(function);
	}
}

} // namespace duckdb
