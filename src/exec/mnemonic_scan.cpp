#include "mnemonicdb/mnemonic_scan.hpp"
#include "mnemonicdb/mnemonic_catalog.hpp"
#include "mnemonicdb/mnemonic_table_entry.hpp"
#include "mnemonicdb/mnemonic_transaction.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"

#include "rocksdb/db.h"

namespace duckdb {

using mnemonicdb::AttributeDef;
using mnemonicdb::ColumnFamily;
using mnemonicdb::EntityId;

struct MnemonicScanBindData : public TableFunctionData {
	explicit MnemonicScanBindData(MnemonicTableEntry &table) : table(table) {
	}
	MnemonicTableEntry &table;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<MnemonicScanBindData>(table);
	}
	bool Equals(const FunctionData &other) const override {
		return &table == &other.Cast<MnemonicScanBindData>().table;
	}
};

//! Iterates the current AEVT datoms of one attribute
struct AttributeCursor {
	AttributeCursor(mnemonicdb::ReadView &view, const AttributeDef &attr)
	    : attr(attr), prefix(mnemonicdb::AevtPrefix(attr.Local())), it(view.NewIterator(ColumnFamily::AEVT)) {
		it->Seek(prefix);
		Check();
	}

	const AttributeDef &attr;
	string prefix;
	std::unique_ptr<rocksdb::Iterator> it;
	bool valid = false;
	EntityId entity = 0;

	void Next() {
		it->Next();
		Check();
	}
	//! The V segment of the current key: AEVT keys are A2 E6 V T5
	std::string_view Value() const {
		auto key = it->key();
		constexpr size_t head = mnemonicdb::PACKED_ATTR_SIZE + mnemonicdb::PACKED_ID_SIZE;
		return std::string_view(key.data() + head, key.size() - head - mnemonicdb::PACKED_TX_SIZE);
	}

private:
	void Check() {
		valid = it->Valid() && it->key().starts_with(prefix);
		if (valid) {
			entity = mnemonicdb::GetEntityId(it->key().data() + mnemonicdb::PACKED_ATTR_SIZE);
		} else if (!it->status().ok()) {
			throw IOException("MnemonicDB: RocksDB error: %s", it->status().ToString());
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
	//! For each cursor, the output column it fills, if projected
	vector<optional_idx> cursor_output;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<GlobalTableFunctionState> MnemonicScanInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &table = input.bind_data->Cast<MnemonicScanBindData>().table;
	auto &transaction = MnemonicTransaction::Get(context, table.catalog);
	auto &view = transaction.GetReadView();
	auto &def = table.GetTableDef();
	auto &schema = table.GetMnemonicSchema();

	auto state = make_uniq<MnemonicScanState>();
	for (auto local : def.attributes) {
		state->cursors.emplace_back(view, schema.GetAttribute(local));
	}
	state->cursor_output.resize(state->cursors.size());
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
			state->cursor_output[column.cursor] = i;
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
			auto output_index = state.cursor_output[c];
			bool has_value = cursor.valid && cursor.entity == entity;
			if (!output_index.IsValid()) {
				while (cursor.valid && cursor.entity == entity) {
					cursor.Next();
				}
				continue;
			}
			auto &vector = output.data[output_index.GetIndex()];
			if (!has_value) {
				FlatVector::SetNull(vector, count, true);
				continue;
			}
			auto &attr = cursor.attr;
			if (!attr.many) {
				attr.codec.Decode(cursor.Value(), vector, count);
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
			vector.SetValue(count, Value::LIST(attr.value_type, many_values));
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

TableFunction MnemonicScanFunction(ClientContext &, MnemonicTableEntry &table, unique_ptr<FunctionData> &bind_data,
                                   optional_ptr<BoundAtClause> at_clause) {
	if (at_clause) {
		throw NotImplementedException("MnemonicDB does not support AT queries yet");
	}
	bind_data = make_uniq<MnemonicScanBindData>(table);
	TableFunction function("mnemonicdb_scan", {}, MnemonicScan, nullptr, MnemonicScanInit);
	function.projection_pushdown = true;
	function.get_bind_info = [](const optional_ptr<FunctionData> data) {
		return BindInfo(data->Cast<MnemonicScanBindData>().table);
	};
	return function;
}

} // namespace duckdb
