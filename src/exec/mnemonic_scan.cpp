#include "mnemonicdb/mnemonic_scan.hpp"
#include "mnemonicdb/mnemonic_catalog.hpp"
#include "mnemonicdb/mnemonic_table_entry.hpp"
#include "mnemonicdb/mnemonic_transaction.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/table_filter_set.hpp"
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

//! Whether a datom read from a current (or, with `history_value`, a history) index is visible as of `as_of`
static bool VisibleAsOf(optional_idx as_of, std::string_view key, const rocksdb::Slice *history_value) {
	if (!as_of.IsValid()) {
		return history_value == nullptr;
	}
	auto basis = as_of.GetIndex();
	if (mnemonicdb::GetTxId(key.data() + key.size() - mnemonicdb::PACKED_TX_SIZE) > basis) {
		return false;
	}
	if (!history_value) {
		return true;
	}
	return basis < mnemonicdb::DecodeHistoryValue(std::string_view(history_value->data(), history_value->size()));
}

//! Iterate the visible entries of `cf` (and, when reading as of a tx, its history index) under `prefix`
template <class CALLBACK>
static void ForEachVisible(mnemonicdb::ReadView &view, ColumnFamily cf, ColumnFamily history_cf, const string &prefix,
                           optional_idx as_of, CALLBACK &&callback) {
	auto scan = [&](ColumnFamily family, bool is_history) {
		auto it = view.NewIterator(family);
		for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next()) {
			auto value = it->value();
			std::string_view key(it->key().data(), it->key().size());
			if (VisibleAsOf(as_of, key, is_history ? &value : nullptr)) {
				callback(key);
			}
		}
		if (!it->status().ok()) {
			throw IOException("MnemonicDB: RocksDB error: %s", it->status().ToString());
		}
	};
	scan(cf, false);
	if (as_of.IsValid()) {
		scan(history_cf, true);
	}
}

//! What one scanned column holds
struct ScanColumn {
	enum class Kind { ID, ATTRIBUTE, EMPTY } kind;
	//! Index into the table's attributes for ATTRIBUTE columns
	idx_t attribute = 0;
};

//! A pushed-down filter, applied exactly to every produced row
struct ScanFilter {
	//! Index into the scanned columns
	idx_t column;
	const ExpressionFilter &filter;
	unique_ptr<ExpressionExecutor> executor;
};

//! An encoded value range [lower, upper] on an AVET-indexed attribute; an empty bound is unbounded
struct ValueRange {
	string lower;
	bool lower_inclusive = true;
	bool has_lower = false;
	string upper;
	bool upper_inclusive = true;
	bool has_upper = false;
};

struct MnemonicScanState : public GlobalTableFunctionState {
	vector<ScanColumn> columns;
	vector<LogicalType> column_types;
	vector<idx_t> projection_ids;
	vector<ScanFilter> filters;
	//! For each table attribute, the scanned columns it fills (a column may be scanned more than once)
	vector<vector<idx_t>> attribute_columns;
	//! Columns holding the entity id
	vector<idx_t> id_columns;
	DataChunk scan_chunk;

	//! Merge mode: one cursor per attribute over AEVT
	vector<AttributeCursor> cursors;
	//! Lookup mode: the candidate entities, sorted, and the next one to produce
	bool lookup_mode = false;
	vector<EntityId> candidates;
	idx_t next_candidate = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static optional_ptr<const Expression> ColumnSide(const Expression &left, const Expression &right,
                                                 optional_ptr<const Expression> &constant) {
	if (left.GetExpressionType() == ExpressionType::BOUND_REF &&
	    right.GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
		constant = &right;
		return &left;
	}
	if (right.GetExpressionType() == ExpressionType::BOUND_REF &&
	    left.GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
		constant = &left;
		return &right;
	}
	return nullptr;
}

//! Value constants the filter compares the column to, if the filter is an equality or IN list (optionally under OR)
static bool ExtractPoints(const Expression &expr, vector<Value> &points) {
	if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_OR) {
		for (auto &child : expr.Cast<BoundConjunctionExpression>().GetChildren()) {
			if (!ExtractPoints(*child, points)) {
				return false;
			}
		}
		return true;
	}
	if (expr.GetExpressionType() == ExpressionType::COMPARE_IN) {
		auto &children = expr.Cast<BoundOperatorExpression>().GetChildren();
		if (children.empty() || children[0]->GetExpressionType() != ExpressionType::BOUND_REF) {
			return false;
		}
		for (idx_t i = 1; i < children.size(); i++) {
			if (children[i]->GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
				return false;
			}
			points.push_back(children[i]->Cast<BoundConstantExpression>().GetValue());
		}
		return true;
	}
	if (expr.GetExpressionType() == ExpressionType::COMPARE_EQUAL && BoundComparisonExpression::IsComparison(expr)) {
		auto &function = expr.Cast<BoundFunctionExpression>();
		optional_ptr<const Expression> constant;
		if (!ColumnSide(BoundComparisonExpression::Left(function), BoundComparisonExpression::Right(function),
		                constant)) {
			return false;
		}
		points.push_back(constant->Cast<BoundConstantExpression>().GetValue());
		return true;
	}
	return false;
}

static vector<ValueRange> IntersectRanges(const vector<ValueRange> &left, const vector<ValueRange> &right) {
	vector<ValueRange> result;
	for (auto &a : left) {
		for (auto &b : right) {
			ValueRange range = a;
			if (b.has_lower && (!range.has_lower || b.lower > range.lower ||
			                    (b.lower == range.lower && !b.lower_inclusive))) {
				range.lower = b.lower;
				range.lower_inclusive = b.lower_inclusive;
				range.has_lower = true;
			}
			if (b.has_upper && (!range.has_upper || b.upper < range.upper ||
			                    (b.upper == range.upper && !b.upper_inclusive))) {
				range.upper = b.upper;
				range.upper_inclusive = b.upper_inclusive;
				range.has_upper = true;
			}
			if (range.has_lower && range.has_upper &&
			    (range.lower > range.upper ||
			     (range.lower == range.upper && !(range.lower_inclusive && range.upper_inclusive)))) {
				continue;
			}
			result.push_back(std::move(range));
		}
	}
	return result;
}

//! Value ranges the filter restricts the column to, if it is made of comparisons with constants
static bool ExtractRanges(const Expression &expr, const mnemonicdb::ValueCodec &codec, vector<ValueRange> &ranges) {
	vector<Value> points;
	if (ExtractPoints(expr, points)) {
		for (auto &point : points) {
			if (point.IsNull()) {
				continue;
			}
			ValueRange range;
			range.lower = range.upper = codec.EncodeValue(point);
			range.has_lower = range.has_upper = true;
			ranges.push_back(std::move(range));
		}
		return true;
	}
	if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		// Intersect the ranges of every conjunct that has them; the full filter is still applied to every row
		bool any = false;
		vector<ValueRange> result;
		for (auto &child : expr.Cast<BoundConjunctionExpression>().GetChildren()) {
			vector<ValueRange> child_ranges;
			if (!ExtractRanges(*child, codec, child_ranges)) {
				continue;
			}
			result = any ? IntersectRanges(result, child_ranges) : std::move(child_ranges);
			any = true;
		}
		if (any) {
			ranges.insert(ranges.end(), result.begin(), result.end());
		}
		return any;
	}
	if (expr.GetExpressionType() == ExpressionType::COMPARE_BETWEEN &&
	    expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &between = expr.Cast<BoundFunctionExpression>();
		if (!BoundBetweenExpression::HasValidBindData(between) ||
		    BoundBetweenExpression::Input(between).GetExpressionType() != ExpressionType::BOUND_REF ||
		    BoundBetweenExpression::LowerBound(between).GetExpressionType() != ExpressionType::VALUE_CONSTANT ||
		    BoundBetweenExpression::UpperBound(between).GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
			return false;
		}
		auto &lower = BoundBetweenExpression::LowerBound(between).Cast<BoundConstantExpression>().GetValue();
		auto &upper = BoundBetweenExpression::UpperBound(between).Cast<BoundConstantExpression>().GetValue();
		if (lower.IsNull() || upper.IsNull()) {
			return true;
		}
		ValueRange range;
		range.lower = codec.EncodeValue(lower);
		range.has_lower = true;
		range.lower_inclusive = BoundBetweenExpression::LowerInclusive(between);
		range.upper = codec.EncodeValue(upper);
		range.has_upper = true;
		range.upper_inclusive = BoundBetweenExpression::UpperInclusive(between);
		ranges.push_back(std::move(range));
		return true;
	}
	if (!BoundComparisonExpression::IsComparison(expr)) {
		return false;
	}
	auto &function = expr.Cast<BoundFunctionExpression>();
	auto &left = BoundComparisonExpression::Left(function);
	auto type = expr.GetExpressionType();
	optional_ptr<const Expression> constant;
	auto column = ColumnSide(left, BoundComparisonExpression::Right(function), constant);
	if (!column) {
		return false;
	}
	if (column.get() != &left) {
		type = FlipComparisonExpression(type);
	}
	auto &value = constant->Cast<BoundConstantExpression>().GetValue();
	if (value.IsNull()) {
		return true;
	}
	ValueRange range;
	switch (type) {
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		range.upper = codec.EncodeValue(value);
		range.has_upper = true;
		range.upper_inclusive = type == ExpressionType::COMPARE_LESSTHANOREQUALTO;
		break;
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		range.lower = codec.EncodeValue(value);
		range.has_lower = true;
		range.lower_inclusive = type == ExpressionType::COMPARE_GREATERTHANOREQUALTO;
		break;
	default:
		return false;
	}
	ranges.push_back(std::move(range));
	return true;
}

//! Entities with a visible AVET entry for `attr` inside one of `ranges`
static vector<EntityId> EntitiesInRanges(mnemonicdb::ReadView &view, const AttributeDef &attr,
                                         const vector<ValueRange> &ranges, optional_idx as_of) {
	vector<EntityId> result;
	constexpr size_t tail = mnemonicdb::PACKED_ID_SIZE + mnemonicdb::PACKED_TX_SIZE;
	auto prefix = mnemonicdb::AvetPrefix(attr.Local());
	for (auto &range : ranges) {
		auto scan = [&](ColumnFamily family, bool is_history) {
			auto it = view.NewIterator(family);
			it->Seek(range.has_lower ? prefix + range.lower : prefix);
			for (; it->Valid() && it->key().starts_with(prefix); it->Next()) {
				std::string_view key(it->key().data(), it->key().size());
				auto v = key.substr(prefix.size(), key.size() - prefix.size() - tail);
				if (range.has_lower && !range.lower_inclusive && v == range.lower) {
					continue;
				}
				if (range.has_upper) {
					auto cmp = v.compare(range.upper);
					if (cmp > 0 || (cmp == 0 && !range.upper_inclusive)) {
						break;
					}
				}
				auto value = it->value();
				if (VisibleAsOf(as_of, key, is_history ? &value : nullptr)) {
					result.push_back(mnemonicdb::GetEntityId(key.data() + key.size() - tail));
				}
			}
			if (!it->status().ok()) {
				throw IOException("MnemonicDB: RocksDB error: %s", it->status().ToString());
			}
		};
		scan(ColumnFamily::AVET, false);
		if (as_of.IsValid()) {
			scan(ColumnFamily::AVET_HIST, true);
		}
	}
	return result;
}

static unique_ptr<GlobalTableFunctionState> MnemonicScanInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<MnemonicScanBindData>();
	auto &table = bind_data.table;
	auto &transaction = MnemonicTransaction::Get(context, table.catalog);
	transaction.MarkDataRead();
	auto &view = transaction.GetReadView();
	auto &def = table.GetTableDef();

	auto state = make_uniq<MnemonicScanState>();
	state->attribute_columns.resize(def.attributes.size());
	for (idx_t i = 0; i < input.column_ids.size(); i++) {
		auto column_id = input.column_ids[i];
		ScanColumn column;
		if (column_id == MNEMONIC_ROWID_COLUMN || column_id == MNEMONIC_ID_COLUMN_INDEX) {
			column.kind = ScanColumn::Kind::ID;
			state->id_columns.push_back(i);
			state->column_types.push_back(mnemonicdb::RefType());
		} else if (column_id == COLUMN_IDENTIFIER_EMPTY) {
			column.kind = ScanColumn::Kind::EMPTY;
			state->column_types.push_back(LogicalType::BOOLEAN);
		} else {
			column.kind = ScanColumn::Kind::ATTRIBUTE;
			column.attribute = column_id - 1;
			state->attribute_columns[column.attribute].push_back(i);
			state->column_types.push_back(table.GetAttribute(column_id).type);
		}
		state->columns.push_back(column);
	}
	state->projection_ids = input.projection_ids;
	state->scan_chunk.Initialize(context, state->column_types);

	// Every pushed filter is accepted and applied exactly to each row. Filters on `id` or on an AVET-indexed column
	// may also narrow the scan to candidate entities.
	optional_idx id_points_filter;
	vector<EntityId> id_points;
	optional_idx range_filter;
	vector<ValueRange> ranges;
	if (input.filters) {
		for (auto &entry : *input.filters) {
			auto &filter = entry.Filter();
			if (ExpressionFilter::IsOptionalFilter(filter)) {
				continue;
			}
			auto &expression_filter = ExpressionFilter::GetExpressionFilter(filter, "mnemonicdb scan");
			auto column = entry.GetIndex().GetIndex();
			state->filters.push_back(
			    ScanFilter {column, expression_filter, make_uniq<ExpressionExecutor>(context, *expression_filter.expr)});
			auto &scan_column = state->columns[column];
			if (scan_column.kind == ScanColumn::Kind::ID && !id_points_filter.IsValid()) {
				vector<Value> points;
				if (ExtractPoints(*expression_filter.expr, points)) {
					id_points_filter = column;
					for (auto &point : points) {
						if (!point.IsNull()) {
							id_points.push_back(point.DefaultCastAs(LogicalType::UBIGINT).GetValue<uint64_t>());
						}
					}
				}
			} else if (scan_column.kind == ScanColumn::Kind::ATTRIBUTE && !range_filter.IsValid()) {
				auto &attr = table.GetAttribute(scan_column.attribute + 1);
				vector<ValueRange> attr_ranges;
				if (attr.HasAvet() && !attr.many && ExtractRanges(*expression_filter.expr, attr.codec, attr_ranges)) {
					range_filter = column;
					ranges = std::move(attr_ranges);
				}
			}
		}
	}

	if (id_points_filter.IsValid()) {
		state->lookup_mode = true;
		state->candidates = std::move(id_points);
	} else if (range_filter.IsValid()) {
		state->lookup_mode = true;
		auto &attr = table.GetAttribute(state->columns[range_filter.GetIndex()].attribute + 1);
		state->candidates = EntitiesInRanges(view, attr, ranges, bind_data.as_of);
	} else {
		auto &schema = table.GetMnemonicSchema();
		for (auto local : def.attributes) {
			state->cursors.emplace_back(view, schema.GetAttribute(local), bind_data.as_of);
		}
	}
	if (state->lookup_mode) {
		std::sort(state->candidates.begin(), state->candidates.end());
		state->candidates.erase(std::unique(state->candidates.begin(), state->candidates.end()),
		                        state->candidates.end());
	}
	return std::move(state);
}

//! Write one entity's values into row `row` of the scan chunk. `values[a]` holds the encoded values of attribute a.
static void WriteRow(MnemonicScanState &state, MnemonicTableEntry &table, EntityId entity,
                     const vector<vector<string>> &values, idx_t row) {
	auto &chunk = state.scan_chunk;
	for (idx_t a = 0; a < values.size(); a++) {
		auto &columns = state.attribute_columns[a];
		if (columns.empty()) {
			continue;
		}
		if (values[a].empty()) {
			for (auto column : columns) {
				FlatVector::SetNull(chunk.data[column], row, true);
			}
			continue;
		}
		auto &attr = table.GetAttribute(a + 1);
		if (!attr.many) {
			for (auto column : columns) {
				attr.codec.Decode(values[a][0], chunk.data[column], row);
			}
			continue;
		}
		vector<Value> elements;
		for (auto &v : values[a]) {
			elements.push_back(attr.codec.DecodeValue(v));
		}
		auto list = Value::LIST(attr.value_type, std::move(elements));
		for (auto column : columns) {
			chunk.data[column].SetValue(row, list);
		}
	}
	for (auto column : state.id_columns) {
		FlatVector::GetDataMutable<uint64_t>(chunk.data[column])[row] = entity;
	}
}

//! Produce the next entity from the AEVT cursors; false when exhausted
static bool NextMerged(MnemonicScanState &state, EntityId &entity, vector<vector<string>> &values) {
	bool any = false;
	for (auto &cursor : state.cursors) {
		if (cursor.valid && (!any || cursor.entity < entity)) {
			entity = cursor.entity;
			any = true;
		}
	}
	if (!any) {
		return false;
	}
	for (idx_t a = 0; a < state.cursors.size(); a++) {
		auto &cursor = state.cursors[a];
		bool wanted = !state.attribute_columns[a].empty();
		while (cursor.valid && cursor.entity == entity) {
			if (wanted) {
				values[a].emplace_back(cursor.Value());
			}
			cursor.Next();
		}
	}
	return true;
}

//! Produce the next candidate entity that is a member of the table; false when exhausted
static bool NextCandidate(MnemonicScanState &state, MnemonicTableEntry &table, mnemonicdb::ReadView &view,
                          optional_idx as_of, EntityId &entity, vector<vector<string>> &values) {
	auto &def = table.GetTableDef();
	constexpr size_t head = mnemonicdb::PACKED_ID_SIZE + mnemonicdb::PACKED_ATTR_SIZE;
	while (state.next_candidate < state.candidates.size()) {
		entity = state.candidates[state.next_candidate++];
		if (!mnemonicdb::IsWellFormed(entity)) {
			continue;
		}
		bool member = false;
		for (idx_t a = 0; a < def.attributes.size(); a++) {
			auto &attr = table.GetAttribute(a + 1);
			auto prefix = mnemonicdb::EavtPrefix(entity, attr.Local());
			ForEachVisible(view, ColumnFamily::EAVT, ColumnFamily::EAVT_HIST, prefix, as_of, [&](std::string_view key) {
				member = true;
				values[a].emplace_back(key.substr(head, key.size() - head - mnemonicdb::PACKED_TX_SIZE));
			});
			// Values from the history index arrive after the current ones; keep them in value order
			std::sort(values[a].begin(), values[a].end());
		}
		if (member) {
			return true;
		}
	}
	return false;
}

static void MnemonicScan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<MnemonicScanBindData>();
	auto &table = bind_data.table;
	auto &state = input.global_state->Cast<MnemonicScanState>();
	auto &view = MnemonicTransaction::Get(context, table.catalog).GetReadView();
	auto attribute_count = table.GetTableDef().attributes.size();
	auto &chunk = state.scan_chunk;

	vector<vector<string>> values(attribute_count);
	while (true) {
		chunk.Reset();
		idx_t count = 0;
		bool exhausted = false;
		while (count < STANDARD_VECTOR_SIZE) {
			for (auto &v : values) {
				v.clear();
			}
			EntityId entity = 0;
			bool found = state.lookup_mode ? NextCandidate(state, table, view, bind_data.as_of, entity, values)
			                               : NextMerged(state, entity, values);
			if (!found) {
				exhausted = true;
				break;
			}
			WriteRow(state, table, entity, values, count);
			count++;
		}
		chunk.SetChildCardinality(count);

		// Apply every pushed filter exactly
		SelectionVector sel(STANDARD_VECTOR_SIZE);
		idx_t selected = count;
		for (auto &filter : state.filters) {
			if (selected == 0) {
				break;
			}
			DataChunk filter_input;
			filter_input.InitializeEmpty({state.column_types[filter.column]});
			filter_input.data[0].Reference(chunk.data[filter.column]);
			filter_input.SetCardinalityUnsafe(chunk.size());
			SelectionVector filter_sel(STANDARD_VECTOR_SIZE);
			auto passed = filter.executor->SelectExpression(filter_input, filter_sel);
			chunk.Slice(filter_sel, passed);
			selected = passed;
		}

		if (selected > 0 || exhausted) {
			if (state.projection_ids.empty()) {
				output.Reference(chunk);
			} else {
				for (idx_t i = 0; i < state.projection_ids.size(); i++) {
					output.data[i].Reference(chunk.data[state.projection_ids[i]]);
				}
				output.SetCardinalityUnsafe(chunk.size());
			}
			return;
		}
	}
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
	function.filter_pushdown = true;
	function.filter_prune = true;
	function.get_virtual_columns = [](ClientContext &, optional_ptr<FunctionData> data) {
		return data->Cast<MnemonicScanBindData>().table.GetVirtualColumns();
	};
	function.get_bind_info = [](const optional_ptr<FunctionData> data) {
		return BindInfo(data->Cast<MnemonicScanBindData>().table);
	};
	return function;
}

} // namespace duckdb
