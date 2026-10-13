#include "mnemonicdb/mnemonic_scan.hpp"
#include "mnemonicdb/mnemonic_catalog.hpp"
#include "mnemonicdb/mnemonic_table_entry.hpp"
#include "mnemonicdb/mnemonic_transaction.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"

#include "rocksdb/db.h"

namespace duckdb {

using mnemonicdb::AttributeDef;
using mnemonicdb::ColumnFamily;
using mnemonicdb::EntityId;

struct MnemonicScanBindData : public TableFunctionData {
	MnemonicScanBindData(MnemonicTableEntry &table, optional_idx as_of) : table(table), as_of(as_of) {
	}
	MnemonicTableEntry &table;
	//! The transaction to read as of (AT clause); unset reads the current state
	optional_idx as_of;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<MnemonicScanBindData>(table, as_of);
	}
	bool Equals(const FunctionData &other) const override {
		auto &other_data = other.Cast<MnemonicScanBindData>();
		return &table == &other_data.table && as_of == other_data.as_of;
	}
};

//! Iterates the datoms of one attribute in AEVT order. Reading the current state uses the current index only;
//! reading as of transaction T merges in the history index and keeps datoms with Tassert <= T < Tretract.
struct AttributeCursor {
	AttributeCursor(mnemonicdb::ReadView &view, const AttributeDef &attr, optional_idx as_of)
	    : attr(attr), prefix(mnemonicdb::AevtPrefix(attr.Local())), as_of(as_of),
	      current(view.NewIterator(ColumnFamily::AEVT)) {
		current->Seek(prefix);
		if (as_of.IsValid()) {
			history = view.NewIterator(ColumnFamily::AEVT_HIST);
			history->Seek(prefix);
		}
		Settle();
	}

	const AttributeDef &attr;
	string prefix;
	optional_idx as_of;
	std::unique_ptr<rocksdb::Iterator> current;
	std::unique_ptr<rocksdb::Iterator> history;
	//! The iterator positioned on the datom this cursor is at, or null when exhausted
	rocksdb::Iterator *at = nullptr;
	bool valid = false;
	EntityId entity = 0;

	void Next() {
		at->Next();
		Settle();
	}
	//! The V segment of the current key: AEVT keys are A2 E6 V T5
	std::string_view Value() const {
		auto key = at->key();
		constexpr size_t head = mnemonicdb::PACKED_ATTR_SIZE + mnemonicdb::PACKED_ID_SIZE;
		return std::string_view(key.data() + head, key.size() - head - mnemonicdb::PACKED_TX_SIZE);
	}

private:
	bool InRange(rocksdb::Iterator *it) const {
		if (!it) {
			return false;
		}
		if (!it->Valid()) {
			if (!it->status().ok()) {
				throw IOException("MnemonicDB: RocksDB error: %s", it->status().ToString());
			}
			return false;
		}
		return it->key().starts_with(prefix);
	}

	static mnemonicdb::TxId AssertedBy(rocksdb::Iterator *it) {
		auto key = it->key();
		return mnemonicdb::GetTxId(key.data() + key.size() - mnemonicdb::PACKED_TX_SIZE);
	}

	bool Visible(rocksdb::Iterator *it) const {
		if (!as_of.IsValid()) {
			return true;
		}
		auto basis = as_of.GetIndex();
		if (AssertedBy(it) > basis) {
			return false;
		}
		if (it == current.get()) {
			return true;
		}
		auto retracted_by = mnemonicdb::DecodeHistoryValue(std::string_view(it->value().data(), it->value().size()));
		return basis < retracted_by;
	}

	//! Move to the next visible datom, taking the smaller key of the two indexes
	void Settle() {
		while (true) {
			bool has_current = InRange(current.get());
			bool has_history = InRange(history.get());
			if (!has_current && !has_history) {
				at = nullptr;
				valid = false;
				return;
			}
			if (has_current && (!has_history || current->key().compare(history->key()) < 0)) {
				at = current.get();
			} else {
				at = history.get();
			}
			if (Visible(at)) {
				valid = true;
				entity = mnemonicdb::GetEntityId(at->key().data() + mnemonicdb::PACKED_ATTR_SIZE);
				return;
			}
			at->Next();
		}
	}
};

//! What one output column of the scan holds
struct ScanColumn {
	enum class Kind { ID, ATTRIBUTE, EMPTY } kind;
	//! Index into the cursors for ATTRIBUTE columns
	idx_t cursor = 0;
};

struct MnemonicScanState : public GlobalTableFunctionState {
	vector<AttributeCursor> cursors;
	vector<ScanColumn> columns;
	//! For each cursor, the output columns it fills (a column may be projected more than once)
	vector<vector<idx_t>> cursor_outputs;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<GlobalTableFunctionState> MnemonicScanInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<MnemonicScanBindData>();
	auto &table = bind_data.table;
	auto &transaction = MnemonicTransaction::Get(context, table.catalog);
	auto &view = transaction.GetReadView();
	auto &def = table.GetTableDef();
	auto &schema = table.GetMnemonicSchema();

	auto state = make_uniq<MnemonicScanState>();
	for (auto local : def.attributes) {
		state->cursors.emplace_back(view, schema.GetAttribute(local), bind_data.as_of);
	}
	state->cursor_outputs.resize(state->cursors.size());
	for (idx_t i = 0; i < input.column_ids.size(); i++) {
		auto column_id = input.column_ids[i];
		ScanColumn column;
		if (column_id == MNEMONIC_ROWID_COLUMN || column_id == MNEMONIC_ID_COLUMN_INDEX) {
			column.kind = ScanColumn::Kind::ID;
		} else if (column_id == COLUMN_IDENTIFIER_EMPTY) {
			column.kind = ScanColumn::Kind::EMPTY;
		} else {
			column.kind = ScanColumn::Kind::ATTRIBUTE;
			column.cursor = column_id - 1;
			state->cursor_outputs[column.cursor].push_back(i);
		}
		state->columns.push_back(column);
	}
	return std::move(state);
}

static void MnemonicScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<MnemonicScanState>();
	idx_t count = 0;
	vector<Value> many_values;
	while (count < STANDARD_VECTOR_SIZE) {
		// The next entity is the smallest one any attribute cursor is positioned on
		bool any = false;
		EntityId entity = 0;
		for (auto &cursor : state.cursors) {
			if (cursor.valid && (!any || cursor.entity < entity)) {
				entity = cursor.entity;
				any = true;
			}
		}
		if (!any) {
			break;
		}
		for (idx_t c = 0; c < state.cursors.size(); c++) {
			auto &cursor = state.cursors[c];
			auto &outputs = state.cursor_outputs[c];
			bool has_value = cursor.valid && cursor.entity == entity;
			if (outputs.empty() || !has_value) {
				for (auto output_index : outputs) {
					FlatVector::SetNull(output.data[output_index], count, true);
				}
				while (cursor.valid && cursor.entity == entity) {
					cursor.Next();
				}
				continue;
			}
			auto &attr = cursor.attr;
			if (!attr.many) {
				for (auto output_index : outputs) {
					attr.codec.Decode(cursor.Value(), output.data[output_index], count);
				}
				while (cursor.valid && cursor.entity == entity) {
					cursor.Next();
				}
				continue;
			}
			many_values.clear();
			while (cursor.valid && cursor.entity == entity) {
				many_values.push_back(attr.codec.DecodeValue(cursor.Value()));
				cursor.Next();
			}
			auto list = Value::LIST(attr.value_type, many_values);
			for (auto output_index : outputs) {
				output.data[output_index].SetValue(count, list);
			}
		}
		for (idx_t i = 0; i < state.columns.size(); i++) {
			if (state.columns[i].kind == ScanColumn::Kind::ID) {
				FlatVector::GetDataMutable<uint64_t>(output.data[i])[count] = entity;
			}
		}
		count++;
	}
	output.SetChildCardinality(count);
}

//! The latest transaction whose instant is at or before `ts`, or 0 if there is none
static mnemonicdb::TxId TxAtInstant(MnemonicTransaction &transaction, timestamp_tz_t ts) {
	auto &instant = transaction.GetSchema().GetAttribute(mnemonicdb::sys::TX_INSTANT);
	auto prefix = mnemonicdb::AvetPrefix(instant.Local());
	// AVET keys are A2 V E6 T5: seek to just past every key whose V is `ts`, then step back
	auto upper = mnemonicdb::AvetPrefix(instant.Local(), instant.codec.EncodeValue(Value::TIMESTAMPTZ(ts))) +
	             string(mnemonicdb::PACKED_ID_SIZE + mnemonicdb::PACKED_TX_SIZE + 1, '\xff');
	auto it = transaction.GetCommittedView().NewIterator(ColumnFamily::AVET);
	it->SeekForPrev(upper);
	if (!it->Valid() || !it->key().starts_with(prefix)) {
		return 0;
	}
	auto datom = mnemonicdb::DecodeKey(mnemonicdb::IndexKind::AVET, std::string_view(it->key().data(), it->key().size()));
	return mnemonicdb::LocalOf(datom.e);
}

static optional_idx ResolveAtClause(ClientContext &context, MnemonicTableEntry &table, BoundAtClause &at_clause) {
	auto &transaction = MnemonicTransaction::Get(context, table.catalog);
	auto unit = StringUtil::Upper(at_clause.Unit().GetIdentifierName());
	auto value = at_clause.GetValue();
	if (value.IsNull()) {
		throw BinderException("MnemonicDB: AT %s cannot be NULL", unit);
	}
	mnemonicdb::TxId tx;
	if (unit == "VERSION") {
		tx = value.DefaultCastAs(LogicalType::UBIGINT).GetValue<uint64_t>();
		// Either a tx number or the id of a sys.tx entity
		if (mnemonicdb::IsWellFormed(tx) && mnemonicdb::PartitionOf(tx) == mnemonicdb::partition::TX) {
			tx = mnemonicdb::LocalOf(tx);
		}
		if (tx > transaction.GetBasisTx()) {
			throw BinderException("MnemonicDB: AT VERSION %d is later than the last committed transaction (%d)", tx,
			                      transaction.GetBasisTx());
		}
	} else if (unit == "TIMESTAMP") {
		tx = TxAtInstant(transaction, value.DefaultCastAs(LogicalType::TIMESTAMP_TZ).GetValue<timestamp_tz_t>());
	} else {
		throw BinderException("MnemonicDB: unsupported AT unit \"%s\": use VERSION or TIMESTAMP", unit);
	}
	return optional_idx(tx);
}

TableFunction MnemonicScanFunction(ClientContext &context, MnemonicTableEntry &table,
                                   unique_ptr<FunctionData> &bind_data, optional_ptr<BoundAtClause> at_clause) {
	optional_idx as_of;
	if (at_clause) {
		as_of = ResolveAtClause(context, table, *at_clause);
	}
	bind_data = make_uniq<MnemonicScanBindData>(table, as_of);
	TableFunction function("mnemonicdb_scan", {}, MnemonicScan, nullptr, MnemonicScanInit);
	function.projection_pushdown = true;
	function.get_bind_info = [](const optional_ptr<FunctionData> data) {
		return BindInfo(data->Cast<MnemonicScanBindData>().table);
	};
	return function;
}

} // namespace duckdb
