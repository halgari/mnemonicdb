#include "mnemonicdb/mnemonic_history.hpp"
#include "mnemonicdb/mnemonic_catalog.hpp"
#include "mnemonicdb/mnemonic_transaction.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"

#include "rocksdb/db.h"

namespace duckdb {

using mnemonicdb::AttrId;
using mnemonicdb::ColumnFamily;
using mnemonicdb::Datom;
using mnemonicdb::EntityId;
using mnemonicdb::TxId;

namespace {

struct HistoryBindData : public TableFunctionData {
	explicit HistoryBindData(MnemonicCatalog &catalog) : catalog(catalog) {
	}
	MnemonicCatalog &catalog;
	//! Restrict to these attributes (a schema's, a table's or one column), when
	//! `filtered`
	vector<AttrId> attributes;
	bool filtered = false;
	optional_idx entity;
	TxId since = 0;
	TxId until = mnemonicdb::LOCAL_MASK;
	//! mnemonicdb_tx_changes: only the reading transaction's pending datoms
	bool tx_changes = false;
	//! The planner's row estimate, computed once per bind (the cardinality
	//! callback only gets a const FunctionData). A prepared statement keeps its
	//! plan, and so this estimate, across executions in later transactions;
	//! only the plan's shape depends on it, never the result.
	mutable optional_idx estimate;
};

//! One assertion or retraction
struct HistoryEvent {
	TxId tx;
	Datom datom;
	bool added;

	bool operator<(const HistoryEvent &other) const {
		return std::tie(tx, datom.e, datom.a, datom.v, added) <
		       std::tie(other.tx, other.datom.e, other.datom.a, other.datom.v, other.added);
	}
};

//! How to describe and decode one attribute, including attributes whose column
//! was dropped
struct AttributeInfo {
	string schema;
	string table;
	string column;
	unique_ptr<mnemonicdb::ValueCodec> codec;
};

struct HistoryState : public GlobalTableFunctionState {
	//! Entity or attribute history: all events, sorted
	vector<HistoryEvent> events;
	idx_t next_event = 0;
	//! Whole database or table history: the txlog, in transaction order
	std::unique_ptr<rocksdb::Iterator> log;
	//! The last tx returned
	TxId until = mnemonicdb::LOCAL_MASK;
	bool filtered = false;
	unordered_set<AttrId> attribute_filter;
	unordered_map<AttrId, unique_ptr<AttributeInfo>> attributes;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> HistoryBindInternal(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<Identifier> &names,
                                                    const char *function_name, bool tx_changes) {
	string name;
	if (input.inputs.empty()) {
		name = DatabaseManager::GetDefaultDatabase(context).GetIdentifierName();
	} else {
		if (input.inputs[0].IsNull()) {
			throw BinderException("%s: the database name cannot be NULL", function_name);
		}
		name = input.inputs[0].GetValue<string>();
	}
	auto &catalog = Catalog::GetCatalog(context, Identifier(name));
	if (catalog.GetCatalogType() != MnemonicCatalog::CATALOG_TYPE) {
		throw BinderException("%s: \"%s\" is not a MnemonicDB database", function_name, name);
	}
	auto &mnemonic_catalog = catalog.Cast<MnemonicCatalog>();
	auto result = make_uniq<HistoryBindData>(mnemonic_catalog);
	result->tx_changes = tx_changes;
	auto &schema = MnemonicTransaction::Get(context, mnemonic_catalog).GetSchema();

	string schema_filter;
	string table_name;
	string column_name;
	for (auto &entry : input.named_parameters) {
		auto &key = entry.first;
		auto &value = entry.second;
		if (value.IsNull()) {
			continue;
		}
		if (key == "schema_name") {
			schema_filter = value.GetValue<string>();
		} else if (key == "table_name") {
			table_name = value.GetValue<string>();
		} else if (key == "column_name") {
			column_name = value.GetValue<string>();
		} else if (key == "entity") {
			result->entity = value.GetValue<uint64_t>();
		} else if (key == "since") {
			result->since = value.GetValue<uint64_t>();
		} else if (key == "until") {
			result->until = value.GetValue<uint64_t>();
		}
	}
	if (!column_name.empty() && table_name.empty()) {
		throw BinderException("%s: column_name := needs table_name :=", function_name);
	}
	auto &view = MnemonicTransaction::Get(context, mnemonic_catalog).GetReadView();
	// Every attribute ever attached to `table`: its columns, plus dropped columns,
	// whose table link is in the history index
	auto add_table_attributes = [&](const mnemonicdb::TableDef &table) {
		result->attributes.insert(result->attributes.end(), table.attributes.begin(), table.attributes.end());
		string table_ref;
		mnemonicdb::PutEntityId(table_ref, table.id);
		for (auto &datom :
		     view.ScanPrefix(ColumnFamily::AEVT_HIST, mnemonicdb::AevtPrefix(mnemonicdb::sys::ATTRIBUTES_TABLE))) {
			auto local = static_cast<AttrId>(mnemonicdb::LocalOf(datom.e));
			if (datom.v == table_ref && !schema.FindAttributeById(local)) {
				result->attributes.push_back(local);
			}
		}
	};
	if (!table_name.empty()) {
		// "table" or "schema.table", or schema_name := with a bare table name
		string schema_name = schema_filter.empty() ? "main" : schema_filter;
		auto dot = table_name.find('.');
		if (dot != string::npos) {
			auto table_schema = table_name.substr(0, dot);
			if (!schema_filter.empty() && !StringUtil::CIEquals(schema_filter, table_schema)) {
				throw BinderException("%s: table_name := \"%s\" is not in schema_name := \"%s\"", function_name,
				                      table_name, schema_filter);
			}
			schema_name = table_schema;
			table_name = table_name.substr(dot + 1);
		}
		auto table = schema.FindTable(schema_name, table_name);
		if (!table) {
			throw BinderException("%s: table \"%s.%s\" does not exist", function_name, schema_name, table_name);
		}
		result->filtered = true;
		if (column_name.empty()) {
			add_table_attributes(*table);
		} else {
			auto attr = schema.FindAttribute(*table, column_name);
			if (!attr) {
				throw BinderException("%s: table \"%s\" has no column \"%s\"", function_name, table_name, column_name);
			}
			result->attributes.push_back(attr->Local());
		}
	} else if (!schema_filter.empty()) {
		if (!schema.FindSchema(schema_filter)) {
			throw BinderException("%s: schema \"%s\" does not exist", function_name, schema_filter);
		}
		result->filtered = true;
		for (auto table : schema.TablesInSchema(schema.FindSchema(schema_filter)->name)) {
			add_table_attributes(*table);
		}
	}

	names = {Identifier("tx"),          Identifier("entity"),    Identifier("schema_name"), Identifier("table_name"),
	         Identifier("column_name"), Identifier("attribute"), Identifier("value"),       Identifier("added")};
	return_types = {LogicalType::UBIGINT, mnemonicdb::RefType(), LogicalType::VARCHAR,   LogicalType::VARCHAR,
	                LogicalType::VARCHAR, mnemonicdb::RefType(), LogicalType::VARIANT(), LogicalType::BOOLEAN};
	if (tx_changes) {
		// Whether the entity was minted by this transaction (a tempid, or a new
		// table or attribute): such entities had no state before it
		names.emplace_back("minted");
		return_types.push_back(LogicalType::BOOLEAN);
	}
	return std::move(result);
}

static unique_ptr<FunctionData> HistoryBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<Identifier> &names) {
	return HistoryBindInternal(context, input, return_types, names, "mnemonicdb_history", false);
}

static unique_ptr<FunctionData> TxChangesBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<Identifier> &names) {
	return HistoryBindInternal(context, input, return_types, names, "mnemonicdb_tx_changes", true);
}

//! Collect the events of the datoms under `prefix` in a current index and its
//! history index
static void CollectEvents(mnemonicdb::ReadView &view, ColumnFamily cf, ColumnFamily history_cf, const string &prefix,
                          vector<HistoryEvent> &events) {
	auto kind = mnemonicdb::IndexKindOf(cf);
	for (auto history : {false, true}) {
		auto it = view.NewIterator(history ? history_cf : cf, prefix);
		for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next()) {
			auto datom = mnemonicdb::DecodeKey(kind, std::string_view(it->key().data(), it->key().size()));
			events.push_back(HistoryEvent {datom.t, datom, true});
			if (history) {
				auto retracted_by =
				    mnemonicdb::DecodeHistoryValue(std::string_view(it->value().data(), it->value().size()));
				events.push_back(HistoryEvent {retracted_by, datom, false});
			}
		}
		if (!it->status().ok()) {
			throw IOException("MnemonicDB: RocksDB error: %s", it->status().ToString());
		}
	}
}

static unique_ptr<GlobalTableFunctionState> HistoryInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &data = input.bind_data->Cast<HistoryBindData>();
	auto &transaction = MnemonicTransaction::Get(context, data.catalog);
	transaction.MarkDataRead();
	auto &view = transaction.GetReadView();
	auto state = make_uniq<HistoryState>();
	state->filtered = data.filtered;
	state->attribute_filter.insert(data.attributes.begin(), data.attributes.end());
	state->until = data.until;

	if (data.tx_changes) {
		// The pending datoms are the txlog entries of the transaction's own tx,
		// which exist only in its write batch
		auto tx = transaction.PendingTx();
		if (tx != 0) {
			state->until = tx;
			state->log = view.NewIterator(ColumnFamily::TXLOG);
			state->log->Seek(mnemonicdb::TxlogPrefix(tx));
		}
		return std::move(state);
	}
	if (data.filtered && data.attributes.empty()) {
		// A filter that matches no attribute (e.g. a schema without tables)
		return std::move(state);
	}
	if (data.entity.IsValid()) {
		auto entity = data.entity.GetIndex();
		if (!data.filtered) {
			CollectEvents(view, ColumnFamily::EAVT, ColumnFamily::EAVT_HIST, mnemonicdb::EavtPrefix(entity),
			              state->events);
		} else {
			for (auto attr : data.attributes) {
				CollectEvents(view, ColumnFamily::EAVT, ColumnFamily::EAVT_HIST, mnemonicdb::EavtPrefix(entity, attr),
				              state->events);
			}
		}
	} else if (data.attributes.size() == 1) {
		CollectEvents(view, ColumnFamily::AEVT, ColumnFamily::AEVT_HIST, mnemonicdb::AevtPrefix(data.attributes[0]),
		              state->events);
	} else {
		state->log = view.NewIterator(ColumnFamily::TXLOG);
		state->log->Seek(mnemonicdb::TxlogPrefix(data.since));
		return std::move(state);
	}
	std::sort(state->events.begin(), state->events.end());
	return std::move(state);
}

//! Name, table and decoder of an attribute; dropped columns are described from
//! their own definition datoms
static AttributeInfo &GetAttributeInfo(ClientContext &context, HistoryState &state, MnemonicTransaction &transaction,
                                       AttrId local) {
	auto entry = state.attributes.find(local);
	if (entry != state.attributes.end()) {
		return *entry->second;
	}
	auto info = make_uniq<AttributeInfo>();
	auto &schema = transaction.GetSchema();
	auto attr = schema.FindAttributeById(local);
	if (attr) {
		auto table = schema.FindTable(attr->table);
		info->schema = table ? table->schema : string();
		info->table = table ? table->name : string();
		info->column = attr->name;
		info->codec = make_uniq<mnemonicdb::ValueCodec>(attr->codec);
	} else {
		// A dropped column: its definition datoms are still there, only its table
		// link was retracted
		auto &view = transaction.GetReadView();
		auto read = [&](AttrId sys_attr) -> string {
			auto datoms =
			    view.ScanPrefix(ColumnFamily::EAVT, mnemonicdb::EavtPrefix(mnemonicdb::AttrEntity(local), sys_attr));
			return datoms.empty() ? string() : schema.GetAttribute(sys_attr).codec.DecodeValue(datoms[0].v).ToString();
		};
		info->column = read(mnemonicdb::sys::ATTRIBUTES_NAME);
		auto table_links =
		    view.ScanPrefix(ColumnFamily::EAVT_HIST,
		                    mnemonicdb::EavtPrefix(mnemonicdb::AttrEntity(local), mnemonicdb::sys::ATTRIBUTES_TABLE));
		if (!table_links.empty()) {
			auto table = schema.FindTable(mnemonicdb::GetEntityId(table_links.back().v.data()));
			if (table) {
				info->schema = table->schema;
				info->table = table->name;
			}
		}
		auto type_name = read(mnemonicdb::sys::ATTRIBUTES_TYPE);
		if (type_name.empty()) {
			throw InternalException("MnemonicDB: attribute %d has no definition", local);
		}
		mnemonicdb::AttributeDef dropped(mnemonicdb::AttrEntity(local), 0, info->column,
		                                 schema.ParseType(type_name, context), mnemonicdb::Uniqueness::NONE, false);
		info->codec = make_uniq<mnemonicdb::ValueCodec>(dropped.codec);
	}
	auto &result = *info;
	state.attributes[local] = std::move(info);
	return result;
}

static void HistoryFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<HistoryBindData>();
	auto &state = input.global_state->Cast<HistoryState>();
	auto &transaction = MnemonicTransaction::Get(context, data.catalog);

	idx_t count = 0;
	auto emit = [&](TxId tx, const Datom &datom, bool added) {
		auto &info = GetAttributeInfo(context, state, transaction, datom.a);
		output.data[0].SetValue(count, Value::UBIGINT(tx));
		output.data[1].SetValue(count, Value::UBIGINT(datom.e));
		output.data[2].SetValue(count, info.schema.empty() ? Value() : Value(info.schema));
		output.data[3].SetValue(count, info.table.empty() ? Value() : Value(info.table));
		output.data[4].SetValue(count, Value(info.column));
		output.data[5].SetValue(count, Value::UBIGINT(mnemonicdb::AttrEntity(datom.a)));
		output.data[6].SetValue(count, info.codec->DecodeValue(datom.v).DefaultCastAs(LogicalType::VARIANT()));
		output.data[7].SetValue(count, Value::BOOLEAN(added));
		if (data.tx_changes) {
			output.data[8].SetValue(count, Value::BOOLEAN(transaction.MintedHere(datom.e)));
		}
		count++;
	};

	if (state.log) {
		auto &it = *state.log;
		for (; count < STANDARD_VECTOR_SIZE && it.Valid(); it.Next()) {
			auto datom =
			    mnemonicdb::DecodeKey(mnemonicdb::IndexKind::TXLOG, std::string_view(it.key().data(), it.key().size()));
			if (datom.t > state.until) {
				break;
			}
			if (state.filtered && state.attribute_filter.count(datom.a) == 0) {
				continue;
			}
			auto op = mnemonicdb::DecodeTxOp(std::string_view(it.value().data(), it.value().size()));
			emit(datom.t, datom, op == mnemonicdb::TxOp::ASSERT);
		}
		if (!it.status().ok()) {
			throw IOException("MnemonicDB: RocksDB error: %s", it.status().ToString());
		}
	} else {
		while (count < STANDARD_VECTOR_SIZE && state.next_event < state.events.size()) {
			auto &event = state.events[state.next_event++];
			if (event.tx < data.since || event.tx > data.until) {
				continue;
			}
			emit(event.tx, event.datom, event.added);
		}
	}
	output.SetChildCardinality(count);
}

//! Upper limit on the pending txlog entries counted for an estimate
static constexpr idx_t TX_CHANGES_COUNT_LIMIT = 20000;

//! The estimate for a transaction with more txlog entries than were counted:
//! `matched` of the entries before `stopped_at` matched; scale it by the share of
//! all entries that were scanned. txlog keys are T5 E6 A2 V, so entries are in
//! entity order; that share is estimated from where the scan stopped in the
//! entity range of the transaction (from its first entry to its last), as a
//! transaction's new entities are allocated consecutively.
static idx_t ScaleTxChanges(MnemonicTransaction &transaction, const string &prefix, EntityId first_entity,
                            const rocksdb::Slice &stopped_at, idx_t matched) {
	auto stop_entity = mnemonicdb::GetEntityId(stopped_at.data() + mnemonicdb::PACKED_TX_SIZE);
	auto last = transaction.GetReadView().NewIterator(ColumnFamily::TXLOG, prefix);
	last->SeekForPrev(prefix + string(mnemonicdb::PACKED_ID_SIZE + mnemonicdb::PACKED_ATTR_SIZE + 1, '\xff'));
	if (!last->Valid() || !last->key().starts_with(prefix)) {
		return matched;
	}
	auto last_entity = mnemonicdb::GetEntityId(last->key().data() + mnemonicdb::PACKED_TX_SIZE);
	// Only comparable within one partition: ids of different partitions are
	// 2^56 apart, which says nothing about how many entries lie between. With
	// more partitions still ahead, assume a tenth was seen.
	double fraction = 0.1;
	auto part = mnemonicdb::PartitionOf(last_entity);
	if (mnemonicdb::PartitionOf(first_entity) == part && mnemonicdb::PartitionOf(stop_entity) == part &&
	    last_entity > first_entity) {
		auto first = double(mnemonicdb::LocalOf(first_entity));
		fraction = (double(mnemonicdb::LocalOf(stop_entity)) - first + 1) /
		           (double(mnemonicdb::LocalOf(last_entity)) - first + 1);
	}
	fraction = MinValue(MaxValue(fraction, 0.001), 1.0);
	return MaxValue<idx_t>(matched, idx_t(double(matched) / fraction));
}

//! The number of rows mnemonicdb_tx_changes will return: its pending txlog
//! entries (those of the filtered attributes), counted up to a limit
static idx_t EstimateTxChanges(ClientContext &context, const HistoryBindData &data) {
	auto &transaction = MnemonicTransaction::Get(context, data.catalog);
	auto tx = transaction.PendingTx();
	if (tx == 0) {
		return 0;
	}
	auto prefix = mnemonicdb::TxlogPrefix(tx);
	auto it = transaction.GetReadView().NewIterator(ColumnFamily::TXLOG, prefix);
	unordered_set<AttrId> filter(data.attributes.begin(), data.attributes.end());
	idx_t scanned = 0;
	idx_t matched = 0;
	EntityId first_entity = 0;
	for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next()) {
		if (++scanned > TX_CHANGES_COUNT_LIMIT) {
			return ScaleTxChanges(transaction, prefix, first_entity, it->key(), matched);
		}
		// The first entity of the partition the scan is in (e.g. past the tx
		// entity's own datoms)
		auto entity = mnemonicdb::GetEntityId(it->key().data() + mnemonicdb::PACKED_TX_SIZE);
		if (scanned == 1 || mnemonicdb::PartitionOf(entity) != mnemonicdb::PartitionOf(first_entity)) {
			first_entity = entity;
		}
		if (!data.filtered) {
			matched++;
			continue;
		}
		// txlog keys are T5 E6 A2 V
		auto key = it->key();
		auto attr = mnemonicdb::GetAttrId(key.data() + mnemonicdb::PACKED_TX_SIZE + mnemonicdb::PACKED_ID_SIZE);
		matched += filter.count(attr);
	}
	return matched;
}

static idx_t EstimateHistory(const HistoryBindData &data) {
	auto &store = data.catalog.GetStore();
	if (data.filtered && data.attributes.empty()) {
		return 0;
	}
	if (data.entity.IsValid()) {
		// One entity's datoms: a few per attribute
		return data.filtered ? 2 * data.attributes.size() : 32;
	}
	// Current datoms (each asserted once), plus as many again for history
	idx_t current = 0;
	if (data.filtered) {
		for (auto attr : data.attributes) {
			current += store.GetAttributeCount(attr);
		}
	} else {
		current = store.GetTotalCount();
	}
	return 2 * current;
}

static unique_ptr<NodeStatistics> HistoryCardinality(ClientContext &context, const FunctionData *bind_data) {
	auto &data = bind_data->Cast<HistoryBindData>();
	if (!data.estimate.IsValid()) {
		data.estimate = data.tx_changes ? EstimateTxChanges(context, data) : EstimateHistory(data);
	}
	// Never 0: some planner arithmetic divides by estimates
	auto rows = MaxValue<idx_t>(data.estimate.GetIndex(), 1);
	return make_uniq<NodeStatistics>(rows, rows);
}

} // namespace

void RegisterHistoryFunction(ExtensionLoader &loader) {
	TableFunction function("mnemonicdb_history", {LogicalType::VARCHAR}, HistoryFunction, HistoryBind, HistoryInit);
	function.named_parameters["schema_name"] = LogicalType::VARCHAR;
	function.named_parameters["table_name"] = LogicalType::VARCHAR;
	function.named_parameters["column_name"] = LogicalType::VARCHAR;
	function.named_parameters["entity"] = LogicalType::UBIGINT;
	function.named_parameters["since"] = LogicalType::UBIGINT;
	function.named_parameters["until"] = LogicalType::UBIGINT;
	function.cardinality = HistoryCardinality;
	loader.RegisterFunction(function);

	// mnemonicdb_tx_changes([db], table_name :=, column_name :=): the datoms the
	// reading transaction has written but not committed. Commit assertions use it
	// to check only what changed.
	TableFunctionSet tx_changes("mnemonicdb_tx_changes");
	for (auto &arguments : {vector<LogicalType> {}, vector<LogicalType> {LogicalType::VARCHAR}}) {
		TableFunction overload("mnemonicdb_tx_changes", arguments, HistoryFunction, TxChangesBind, HistoryInit);
		overload.named_parameters["schema_name"] = LogicalType::VARCHAR;
		overload.named_parameters["table_name"] = LogicalType::VARCHAR;
		overload.named_parameters["column_name"] = LogicalType::VARCHAR;
		overload.cardinality = HistoryCardinality;
		tx_changes.AddFunction(overload);
	}
	loader.RegisterFunction(tx_changes);
}

} // namespace duckdb
