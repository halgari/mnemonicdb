#include "mnemonicdb/mnemonic_scan.hpp"
#include "mnemonicdb/mnemonic_catalog.hpp"
#include "mnemonicdb/mnemonic_table_entry.hpp"
#include "mnemonicdb/mnemonic_transaction.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"
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

unique_ptr<FunctionData> MnemonicScanBindData::Copy() const {
	auto result = make_uniq<MnemonicScanBindData>();
	result->tables = tables;
	result->fused = fused;
	result->as_of = as_of;
	return std::move(result);
}

bool MnemonicScanBindData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<MnemonicScanBindData>();
	if (tables.size() != other.tables.size() || fused != other.fused || as_of != other.as_of) {
		return false;
	}
	for (idx_t i = 0; i < tables.size(); i++) {
		if (&tables[i].table.get() != &other.tables[i].table.get() || tables[i].required != other.tables[i].required) {
			return false;
		}
	}
	return true;
}

idx_t MnemonicScanBindData::ColumnCount() const {
	idx_t count = 0;
	for (auto &table : tables) {
		count += table.table.get().GetColumns().PhysicalColumnCount();
	}
	return count;
}

column_t FusedRowIdColumn(idx_t index) {
	return VIRTUAL_COLUMN_START + 1 + index;
}

//! Iterates the datoms of one attribute in AEVT order. Reading the current state uses the current index only;
//! reading as of transaction T merges in the history index and keeps datoms with Tassert <= T < Tretract.
struct AttributeCursor {
	//! A cursor over the entities in [start, end) (end 0: no end)
	AttributeCursor(mnemonicdb::ReadView &view, const AttributeDef &attr, optional_idx as_of, EntityId start = 0,
	                EntityId end = 0)
	    : attr(attr), prefix(mnemonicdb::AevtPrefix(attr.Local())), as_of(as_of), end(end),
	      current(view.NewIterator(ColumnFamily::AEVT, prefix)) {
		auto seek = start == 0 ? prefix : mnemonicdb::AevtPrefix(attr.Local(), start);
		current->Seek(seek);
		if (as_of.IsValid()) {
			history = view.NewIterator(ColumnFamily::AEVT_HIST, prefix);
			history->Seek(seek);
		}
		Settle();
	}

	const AttributeDef &attr;
	string prefix;
	optional_idx as_of;
	EntityId end;
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
	//! Move to the first datom of an entity >= target: a few steps forward first, since the target is often close
	void SeekTo(EntityId target) {
		for (idx_t step = 0; step < 4; step++) {
			if (!valid || entity >= target) {
				return;
			}
			Next();
		}
		if (!valid || entity >= target) {
			return;
		}
		auto key = mnemonicdb::AevtPrefix(attr.Local(), target);
		current->Seek(key);
		if (history) {
			history->Seek(key);
		}
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
				entity = mnemonicdb::GetEntityId(at->key().data() + mnemonicdb::PACKED_ATTR_SIZE);
				if (end != 0 && entity >= end) {
					at = nullptr;
					valid = false;
					return;
				}
				valid = true;
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
static void ForEachVisible(mnemonicdb::PrefixReader &reader, ColumnFamily cf, ColumnFamily history_cf,
                           const string &prefix, optional_idx as_of, CALLBACK &&callback) {
	reader.Scan(cf, prefix, [&](std::string_view key, std::string_view) {
		if (VisibleAsOf(as_of, key, nullptr)) {
			callback(key);
		}
	});
	if (as_of.IsValid()) {
		reader.Scan(history_cf, prefix, [&](std::string_view key, std::string_view value) {
			rocksdb::Slice value_slice(value.data(), value.size());
			if (VisibleAsOf(as_of, key, &value_slice)) {
				callback(key);
			}
		});
	}
}

//! What one scanned column holds
struct ScanColumn {
	enum class Kind { ID, ATTRIBUTE, EMPTY } kind;
	//! The table (index into the bind data's tables) for ID columns
	idx_t table = 0;
	//! Index into the scan's attributes for ATTRIBUTE columns
	idx_t attribute = 0;
};

//! One attribute read by the scan, over all tables
struct ScanAttribute {
	idx_t table;
	reference<const AttributeDef> attr;
};

//! A pushed-down filter, applied exactly to every produced row
struct ScanFilter {
	//! Index into the scanned columns
	idx_t column;
	const ExpressionFilter &filter;
};

//! A unit of scan work: an entity range (merge mode) or a slice of the candidates (lookup mode)
struct ScanTask {
	EntityId start = 0;
	EntityId end = 0;
	idx_t candidate_begin = 0;
	idx_t candidate_end = 0;
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
	//! Every attribute of every table, and per attribute the scanned columns it fills (possibly several)
	vector<ScanAttribute> attributes;
	vector<vector<idx_t>> attribute_columns;
	//! Per table, its attributes' range in `attributes`, whether it is required, and its id columns
	vector<std::pair<idx_t, idx_t>> table_attributes;
	vector<bool> table_required;
	vector<vector<idx_t>> id_columns;

	//! Lookup mode: the candidate entities, sorted; otherwise AEVT cursors merge each task's entity range
	bool lookup_mode = false;
	vector<EntityId> candidates;

	//! Work handed out to threads
	mutex task_lock;
	vector<ScanTask> tasks;
	idx_t next_task = 0;

	idx_t MaxThreads() const override {
		return MaxValue<idx_t>(tasks.size(), 1);
	}
	bool NextTask(ScanTask &task) {
		lock_guard<mutex> guard(task_lock);
		if (next_task >= tasks.size()) {
			return false;
		}
		task = tasks[next_task++];
		return true;
	}
};

//! Per-thread scan state
struct MnemonicScanLocalState : public LocalTableFunctionState {
	DataChunk scan_chunk;
	vector<unique_ptr<ExpressionExecutor>> executors;
	unique_ptr<mnemonicdb::PrefixReader> reader;
	vector<AttributeCursor> cursors;
	bool active = false;
	ScanTask task;
	idx_t next_candidate = 0;
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
			auto it = view.NewIterator(family, prefix);
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

//! Resolve a column id of the scan to a table and column
static ScanColumn ResolveColumn(const MnemonicScanBindData &bind_data, const MnemonicScanState &state,
                                column_t column_id) {
	ScanColumn column;
	if (column_id == COLUMN_IDENTIFIER_EMPTY) {
		column.kind = ScanColumn::Kind::EMPTY;
		return column;
	}
	if (!bind_data.fused) {
		if (column_id == MNEMONIC_ROWID_COLUMN || column_id == MNEMONIC_ID_COLUMN_INDEX) {
			column.kind = ScanColumn::Kind::ID;
		} else {
			column.kind = ScanColumn::Kind::ATTRIBUTE;
			column.attribute = column_id - 1;
		}
		return column;
	}
	if (column_id >= FusedRowIdColumn(0) && column_id < FusedRowIdColumn(bind_data.tables.size())) {
		column.kind = ScanColumn::Kind::ID;
		column.table = column_id - FusedRowIdColumn(0);
		return column;
	}
	for (idx_t t = 0; t < bind_data.tables.size(); t++) {
		auto &scan_table = bind_data.tables[t];
		auto count = scan_table.table.get().GetColumns().PhysicalColumnCount();
		if (column_id < scan_table.column_offset || column_id >= scan_table.column_offset + count) {
			continue;
		}
		auto local = column_id - scan_table.column_offset;
		column.table = t;
		if (local == MNEMONIC_ID_COLUMN_INDEX) {
			column.kind = ScanColumn::Kind::ID;
		} else {
			column.kind = ScanColumn::Kind::ATTRIBUTE;
			column.attribute = state.table_attributes[t].first + local - 1;
		}
		return column;
	}
	throw InternalException("MnemonicDB: unknown scan column %d", column_id);
}

static idx_t EstimateTableRows(MnemonicTableEntry &table);

//! Below this many estimated rows a scan runs as a single task
static constexpr idx_t PARALLEL_SCAN_THRESHOLD = 100000;
//! Rough number of rows per task in a parallel scan
static constexpr idx_t ROWS_PER_TASK = 50000;
static constexpr idx_t MAX_SCAN_TASKS = 256;

//! The smallest and largest entity with a datom (visible or historical) of any of the scan's attributes
static bool EntityBounds(mnemonicdb::ReadView &view, const MnemonicScanState &state, bool with_history,
                         EntityId &min_entity, EntityId &max_entity) {
	bool any = false;
	for (auto &attribute : state.attributes) {
		auto local = attribute.attr.get().Local();
		auto prefix = mnemonicdb::AevtPrefix(local);
		for (auto cf : {ColumnFamily::AEVT, ColumnFamily::AEVT_HIST}) {
			if (cf == ColumnFamily::AEVT_HIST && !with_history) {
				continue;
			}
			auto it = view.NewIterator(cf, prefix);
			it->Seek(prefix);
			if (!it->Valid() || !it->key().starts_with(prefix)) {
				continue;
			}
			auto first = mnemonicdb::GetEntityId(it->key().data() + mnemonicdb::PACKED_ATTR_SIZE);
			// The next attribute's prefix bounds this one from above
			it->SeekForPrev(local == UINT16_MAX ? prefix + string(64, '\xff') : mnemonicdb::AevtPrefix(local + 1));
			if (!it->Valid() || !it->key().starts_with(prefix)) {
				continue;
			}
			auto last = mnemonicdb::GetEntityId(it->key().data() + mnemonicdb::PACKED_ATTR_SIZE);
			min_entity = any ? MinValue(min_entity, first) : first;
			max_entity = any ? MaxValue(max_entity, last) : last;
			any = true;
		}
	}
	return any;
}

static void PlanTasks(ClientContext &context, const MnemonicScanBindData &bind_data, MnemonicScanState &state,
                      mnemonicdb::ReadView &view) {
	auto threads = NumericCast<idx_t>(TaskScheduler::GetScheduler(context).NumberOfThreads());
	if (state.lookup_mode) {
		std::sort(state.candidates.begin(), state.candidates.end());
		state.candidates.erase(std::unique(state.candidates.begin(), state.candidates.end()), state.candidates.end());
		auto count = state.candidates.size();
		idx_t per_task = count < 4096 || threads <= 1 ? MaxValue<idx_t>(count, 1)
		                                             : MaxValue<idx_t>(1024, count / (threads * 4));
		for (idx_t begin = 0; begin < count || begin == 0; begin += per_task) {
			ScanTask task;
			task.candidate_begin = begin;
			task.candidate_end = MinValue(count, begin + per_task);
			state.tasks.push_back(task);
			if (count == 0) {
				break;
			}
		}
		return;
	}
	EntityId min_entity = 0;
	EntityId max_entity = 0;
	if (!EntityBounds(view, state, bind_data.as_of.IsValid(), min_entity, max_entity)) {
		state.tasks.push_back(ScanTask {});
		return;
	}
	// Work is proportional to what the scan reads, not what it returns: size tasks by the largest table
	idx_t rows = 0;
	for (auto &table : bind_data.tables) {
		rows = MaxValue(rows, EstimateTableRows(table.table.get()));
	}
	idx_t task_count = 1;
	if (threads > 1 && rows >= PARALLEL_SCAN_THRESHOLD) {
		task_count = MinValue<idx_t>(MAX_SCAN_TASKS, MaxValue<idx_t>(threads, rows / ROWS_PER_TASK));
	}
	// Entity ids are allocated in order, so an even split of the id range is an even split of the rows
	auto span = max_entity - min_entity + 1;
	task_count = MinValue<idx_t>(task_count, span);
	for (idx_t i = 0; i < task_count; i++) {
		ScanTask task;
		task.start = min_entity + EntityId(span * i / task_count);
		task.end = i + 1 == task_count ? max_entity + 1 : min_entity + EntityId(span * (i + 1) / task_count);
		state.tasks.push_back(task);
	}
}

static unique_ptr<GlobalTableFunctionState> MnemonicScanInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<MnemonicScanBindData>();
	auto &catalog = bind_data.tables[0].table.get().catalog;
	auto &transaction = MnemonicTransaction::Get(context, catalog);
	transaction.MarkDataRead();
	auto &view = transaction.GetReadView();

	auto state = make_uniq<MnemonicScanState>();
	for (idx_t t = 0; t < bind_data.tables.size(); t++) {
		auto &table = bind_data.tables[t].table.get();
		auto first = state->attributes.size();
		for (idx_t a = 1; a <= table.GetTableDef().attributes.size(); a++) {
			state->attributes.push_back(ScanAttribute {t, table.GetAttribute(a)});
		}
		state->table_attributes.emplace_back(first, state->attributes.size());
		state->table_required.push_back(bind_data.tables[t].required);
	}
	state->attribute_columns.resize(state->attributes.size());
	state->id_columns.resize(bind_data.tables.size());
	for (idx_t i = 0; i < input.column_ids.size(); i++) {
		auto column = ResolveColumn(bind_data, *state, input.column_ids[i]);
		switch (column.kind) {
		case ScanColumn::Kind::ID:
			state->id_columns[column.table].push_back(i);
			state->column_types.push_back(mnemonicdb::RefType());
			break;
		case ScanColumn::Kind::EMPTY:
			state->column_types.push_back(LogicalType::BOOLEAN);
			break;
		case ScanColumn::Kind::ATTRIBUTE:
			state->attribute_columns[column.attribute].push_back(i);
			state->column_types.push_back(state->attributes[column.attribute].attr.get().type);
			break;
		}
		state->columns.push_back(column);
	}
	state->projection_ids = input.projection_ids;

	// Every pushed filter is accepted and applied exactly to each row. Filters on an id or on an AVET-indexed
	// column may also narrow the scan to candidate entities.
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
			state->filters.push_back(ScanFilter {column, expression_filter});
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
				auto &attr = state->attributes[scan_column.attribute].attr.get();
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
		auto &attr = state->attributes[state->columns[range_filter.GetIndex()].attribute].attr.get();
		state->candidates = EntitiesInRanges(view, attr, ranges, bind_data.as_of);
	}
	PlanTasks(context, bind_data, *state, view);
	return std::move(state);
}

static unique_ptr<LocalTableFunctionState> MnemonicScanInitLocal(ExecutionContext &context,
                                                                 TableFunctionInitInput &,
                                                                 GlobalTableFunctionState *global_state) {
	auto &state = global_state->Cast<MnemonicScanState>();
	auto local = make_uniq<MnemonicScanLocalState>();
	local->scan_chunk.Initialize(context.client, state.column_types);
	for (auto &filter : state.filters) {
		local->executors.push_back(make_uniq<ExpressionExecutor>(context.client, *filter.filter.expr));
	}
	return std::move(local);
}

//! Whether the entity is a member of table `t`, given the values read for every attribute
static bool IsMember(const MnemonicScanState &state, const vector<vector<string>> &values, idx_t t) {
	for (idx_t a = state.table_attributes[t].first; a < state.table_attributes[t].second; a++) {
		if (!values[a].empty()) {
			return true;
		}
	}
	return false;
}

//! Write one entity's values into row `row` of the scan chunk, or return false if it is not a member of every
//! required table. `values[a]` holds the encoded values of attribute a.
static bool WriteRow(MnemonicScanState &state, DataChunk &chunk, EntityId entity, const vector<vector<string>> &values,
                     idx_t row) {
	auto table_count = state.table_required.size();
	vector<bool> member(table_count);
	for (idx_t t = 0; t < table_count; t++) {
		member[t] = IsMember(state, values, t);
		if (state.table_required[t] && !member[t]) {
			return false;
		}
	}
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
		auto &attr = state.attributes[a].attr.get();
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
	for (idx_t t = 0; t < table_count; t++) {
		for (auto column : state.id_columns[t]) {
			if (member[t]) {
				FlatVector::GetDataMutable<uint64_t>(chunk.data[column])[row] = entity;
			} else {
				FlatVector::SetNull(chunk.data[column], row, true);
			}
		}
	}
	return true;
}

//! Produce the next entity from the AEVT cursors; false when exhausted
//! The next entity that is a member of every required table, skipping ahead (leapfrog) past entities missing from
//! any of them; false when no such entity remains
static bool NextRequiredEntity(MnemonicScanState &state, MnemonicScanLocalState &local, EntityId &entity) {
	auto table_count = state.table_required.size();
	while (true) {
		bool agreed = true;
		bool first = true;
		EntityId candidate = 0;
		for (idx_t t = 0; t < table_count; t++) {
			if (!state.table_required[t]) {
				continue;
			}
			// The table's next member is the smallest entity any of its attribute cursors is on
			bool any = false;
			EntityId next = 0;
			for (idx_t a = state.table_attributes[t].first; a < state.table_attributes[t].second; a++) {
				auto &cursor = local.cursors[a];
				if (cursor.valid && (!any || cursor.entity < next)) {
					next = cursor.entity;
					any = true;
				}
			}
			if (!any) {
				return false;
			}
			if (!first && next != candidate) {
				agreed = false;
			}
			candidate = first ? next : MaxValue(candidate, next);
			first = false;
		}
		if (agreed) {
			entity = candidate;
			return true;
		}
		for (idx_t t = 0; t < table_count; t++) {
			if (!state.table_required[t]) {
				continue;
			}
			for (idx_t a = state.table_attributes[t].first; a < state.table_attributes[t].second; a++) {
				local.cursors[a].SeekTo(candidate);
			}
		}
	}
}

static bool NextMerged(MnemonicScanState &state, MnemonicScanLocalState &local, EntityId &entity,
                       vector<vector<string>> &values) {
	bool has_required = false;
	for (auto required : state.table_required) {
		has_required = has_required || required;
	}
	if (has_required) {
		if (!NextRequiredEntity(state, local, entity)) {
			return false;
		}
		// Optional tables only need their cursors brought up to the entity
		for (idx_t t = 0; t < state.table_required.size(); t++) {
			if (state.table_required[t]) {
				continue;
			}
			for (idx_t a = state.table_attributes[t].first; a < state.table_attributes[t].second; a++) {
				local.cursors[a].SeekTo(entity);
			}
		}
	} else {
		bool any = false;
		for (auto &cursor : local.cursors) {
			if (cursor.valid && (!any || cursor.entity < entity)) {
				entity = cursor.entity;
				any = true;
			}
		}
		if (!any) {
			return false;
		}
	}
	for (idx_t a = 0; a < local.cursors.size(); a++) {
		auto &cursor = local.cursors[a];
		// Membership needs every attribute's presence, but values are only kept for projected ones
		bool wanted = !state.attribute_columns[a].empty();
		while (cursor.valid && cursor.entity == entity) {
			if (wanted || values[a].empty()) {
				values[a].emplace_back(cursor.Value());
			}
			cursor.Next();
		}
	}
	return true;
}

//! Produce the next candidate entity's values; false when exhausted
static bool NextCandidate(MnemonicScanState &state, MnemonicScanLocalState &local, optional_idx as_of,
                          EntityId &entity, vector<vector<string>> &values) {
	constexpr size_t head = mnemonicdb::PACKED_ID_SIZE + mnemonicdb::PACKED_ATTR_SIZE;
	while (local.next_candidate < local.task.candidate_end) {
		entity = state.candidates[local.next_candidate++];
		if (!mnemonicdb::IsWellFormed(entity)) {
			continue;
		}
		bool any = false;
		for (idx_t a = 0; a < state.attributes.size(); a++) {
			auto &attr = state.attributes[a].attr.get();
			auto prefix = mnemonicdb::EavtPrefix(entity, attr.Local());
			ForEachVisible(*local.reader, ColumnFamily::EAVT, ColumnFamily::EAVT_HIST, prefix, as_of, [&](std::string_view key) {
				any = true;
				values[a].emplace_back(key.substr(head, key.size() - head - mnemonicdb::PACKED_TX_SIZE));
			});
			// Values from the history index arrive after the current ones; keep them in value order
			std::sort(values[a].begin(), values[a].end());
		}
		if (any) {
			return true;
		}
	}
	return false;
}

//! Start the thread's next task; false when there is no work left
static bool StartTask(MnemonicScanState &state, MnemonicScanLocalState &local, mnemonicdb::ReadView &view,
                      optional_idx as_of) {
	if (!state.NextTask(local.task)) {
		return false;
	}
	local.active = true;
	if (state.lookup_mode) {
		local.next_candidate = local.task.candidate_begin;
		if (!local.reader) {
			local.reader = make_uniq<mnemonicdb::PrefixReader>(view);
		}
		return true;
	}
	local.cursors.clear();
	for (auto &attribute : state.attributes) {
		local.cursors.emplace_back(view, attribute.attr.get(), as_of, local.task.start, local.task.end);
	}
	return true;
}

static void MnemonicScan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<MnemonicScanBindData>();
	auto &state = input.global_state->Cast<MnemonicScanState>();
	auto &local = input.local_state->Cast<MnemonicScanLocalState>();
	auto &view = MnemonicTransaction::Get(context, bind_data.tables[0].table.get().catalog).GetReadView();
	auto &chunk = local.scan_chunk;

	vector<vector<string>> values(state.attributes.size());
	while (true) {
		chunk.Reset();
		idx_t count = 0;
		bool exhausted = false;
		while (count < STANDARD_VECTOR_SIZE) {
			if (!local.active && !StartTask(state, local, view, bind_data.as_of)) {
				exhausted = true;
				break;
			}
			for (auto &v : values) {
				v.clear();
			}
			EntityId entity = 0;
			bool found = state.lookup_mode ? NextCandidate(state, local, bind_data.as_of, entity, values)
			                               : NextMerged(state, local, entity, values);
			if (!found) {
				local.active = false;
				continue;
			}
			if (WriteRow(state, chunk, entity, values, count)) {
				count++;
			}
		}
		chunk.SetChildCardinality(count);

		// Apply every pushed filter exactly
		idx_t selected = count;
		for (idx_t f = 0; f < state.filters.size(); f++) {
			if (selected == 0) {
				break;
			}
			auto &filter = state.filters[f];
			DataChunk filter_input;
			filter_input.InitializeEmpty({state.column_types[filter.column]});
			filter_input.data[0].Reference(chunk.data[filter.column]);
			filter_input.SetCardinalityUnsafe(chunk.size());
			SelectionVector filter_sel(STANDARD_VECTOR_SIZE);
			auto passed = local.executors[f]->SelectExpression(filter_input, filter_sel);
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

//! Estimated rows of one table: the largest current-datom count of its single-valued attributes (a list column
//! has one datom per element), falling back to list columns for tables that only have those
static idx_t EstimateTableRows(MnemonicTableEntry &table) {
	auto &store = table.catalog.Cast<MnemonicCatalog>().GetStore();
	idx_t single = 0;
	idx_t many = 0;
	for (idx_t a = 1; a <= table.GetTableDef().attributes.size(); a++) {
		auto &attr = table.GetAttribute(a);
		auto count = store.GetAttributeCount(attr.Local());
		if (attr.many) {
			many = MaxValue<idx_t>(many, count);
		} else {
			single = MaxValue<idx_t>(single, count);
		}
	}
	return single > 0 ? single : many;
}

idx_t EstimateScanRows(const MnemonicScanBindData &bind_data) {
	optional_idx required_rows;
	idx_t any_rows = 0;
	for (auto &table : bind_data.tables) {
		auto rows = EstimateTableRows(table.table.get());
		any_rows = MaxValue(any_rows, rows);
		if (table.required) {
			required_rows = required_rows.IsValid() ? MinValue(required_rows.GetIndex(), rows) : rows;
		}
	}
	return required_rows.IsValid() ? required_rows.GetIndex() : any_rows;
}

static unique_ptr<NodeStatistics> MnemonicScanCardinality(ClientContext &, const FunctionData *bind_data) {
	return make_uniq<NodeStatistics>(EstimateScanRows(bind_data->Cast<MnemonicScanBindData>()));
}

static TableFunction BaseScanFunction() {
	TableFunction function("mnemonicdb_scan", {}, MnemonicScan, nullptr, MnemonicScanInit, MnemonicScanInitLocal);
	function.projection_pushdown = true;
	function.filter_pushdown = true;
	function.filter_prune = true;
	function.cardinality = MnemonicScanCardinality;
	return function;
}

TableFunction MnemonicScanFunction(ClientContext &context, MnemonicTableEntry &table,
                                   unique_ptr<FunctionData> &bind_data_p, optional_ptr<BoundAtClause> at_clause) {
	auto bind_data = make_uniq<MnemonicScanBindData>();
	bind_data->tables.push_back(MnemonicScanTable {table, true, 0});
	if (at_clause) {
		bind_data->as_of = ResolveAtClause(context, table, *at_clause);
	}
	bind_data_p = std::move(bind_data);
	auto function = BaseScanFunction();
	function.get_bind_info = [](const optional_ptr<FunctionData> data) {
		return BindInfo(data->Cast<MnemonicScanBindData>().tables[0].table.get());
	};
	function.get_virtual_columns = [](ClientContext &, optional_ptr<FunctionData> data) {
		return data->Cast<MnemonicScanBindData>().tables[0].table.get().GetVirtualColumns();
	};
	return function;
}

TableFunction MnemonicFusedScanFunction() {
	auto function = BaseScanFunction();
	function.get_virtual_columns = [](ClientContext &, optional_ptr<FunctionData> data) {
		virtual_column_map_t result;
		auto &bind_data = data->Cast<MnemonicScanBindData>();
		for (idx_t t = 0; t < bind_data.tables.size(); t++) {
			result.emplace(FusedRowIdColumn(t),
			               TableColumn(Identifier(bind_data.tables[t].table.get().name.GetIdentifierName() + ".rowid"),
			                           mnemonicdb::RefType()));
		}
		return result;
	};
	return function;
}

} // namespace duckdb
