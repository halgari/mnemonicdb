#include "mnemonicdb/mnemonic_scan.hpp"
#include "mnemonicdb/mnemonic_catalog.hpp"
#include "mnemonicdb/mnemonic_table_entry.hpp"
#include "mnemonicdb/mnemonic_transaction.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"
#include "duckdb/main/profiler/profiling_node.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
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
	result->join_key_filters = join_key_filters;
	result->join_key_columns = join_key_columns;
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

namespace {

//! Iterates the datoms of one attribute in AEVT order. Reading the current
//! state uses the current index only; reading as of transaction T merges in the
//! history index and keeps datoms with Tassert <= T < Tretract.
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
	//! The iterator positioned on the datom this cursor is at, or null when
	//! exhausted
	rocksdb::Iterator *at = nullptr;
	bool valid = false;
	EntityId entity = 0;

	void Next() {
		at->Next();
		Settle();
	}
	//! Move to the first datom of an entity >= target: a few steps forward first,
	//! since the target is often close
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

//! Whether a datom read from a current (or, with `history_value`, a history)
//! index is visible as of `as_of`
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

//! Iterate the visible entries of `cf` (and, when reading as of a tx, its
//! history index) under `prefix`
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

//! A unit of scan work: an entity range (merge mode) or a slice of the
//! candidates (lookup mode)
struct ScanTask {
	EntityId start = 0;
	EntityId end = 0;
	idx_t candidate_begin = 0;
	idx_t candidate_end = 0;
};

//! An encoded value range [lower, upper] on an AVET-indexed attribute; an empty
//! bound is unbounded
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
	//! Every attribute of every table, and per attribute the scanned columns it
	//! fills (possibly several)
	vector<ScanAttribute> attributes;
	vector<vector<idx_t>> attribute_columns;
	//! Per table, its attributes' range in `attributes`, whether it is required,
	//! and its id columns
	vector<std::pair<idx_t, idx_t>> table_attributes;
	vector<bool> table_required;
	//! Attributes the scan reads. A table whose rows must have some attribute (a
	//! NULL-rejecting filter on it) needs only that "driving" attribute for
	//! membership, plus its projected and filtered ones.
	vector<bool> attribute_active;
	vector<optional_idx> table_driver;
	vector<vector<idx_t>> id_columns;

	//! Lookup mode: the candidate entities, sorted; otherwise AEVT cursors merge
	//! each task's entity range
	bool lookup_mode = false;
	vector<EntityId> candidates;
	//! Only entities in [range_lower, range_upper] can qualify (from filters on
	//! the id); empty when range_lower > range_upper
	EntityId range_lower = 0;
	EntityId range_upper = UINT64_MAX;

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
	//! Per attribute; null for attributes the scan does not read
	vector<unique_ptr<AttributeCursor>> cursors;
	bool active = false;
	//! Entities this thread read (merged or looked up), reported as the
	//! operator's rows_scanned
	idx_t entities_read = 0;
	ScanTask task;
	idx_t next_candidate = 0;
};

//! What the pushed filters on one column say about its values: rows whose value
//! is outside the key set can be skipped. Exact filters are applied to every
//! row anyway; optional and dynamic ones (how a join passes its key set: an IN
//! list, min/max bounds or a key bitmap) are hints, but a row they reject is one
//! the join drops too, so they may narrow what the scan reads.
static bool ValueLess(const Value &a, const Value &b) {
	return a < b;
}

static void SortUnique(vector<Value> &values) {
	std::sort(values.begin(), values.end(), ValueLess);
	values.erase(std::unique(values.begin(), values.end(), [](const Value &a, const Value &b) { return a == b; }),
	             values.end());
}

struct KeySet {
	//! The value is one of `points` (cast to the column type, no NULLs)
	bool has_points = false;
	vector<Value> points;
	bool has_lower = false;
	bool lower_inclusive = true;
	Value lower;
	bool has_upper = false;
	bool upper_inclusive = true;
	Value upper;
	//! The value is a key of each of these join key bitmaps
	vector<reference<const PrefixRangeFunctionData>> key_filters;
	//! Whether the points, and the bounds, come from a filter that is not
	//! optional (written by the user, so worth following however many keys it
	//! has). Points intersected with an exact filter's points are no more than
	//! those; a join's key list intersected with an exact range is still a hint.
	bool exact_points = false;
	bool exact_bounds = false;

	//! Whether what drives the scan (the points if any, else the bounds) is
	//! exact
	bool Exact() const {
		return has_points ? exact_points : exact_bounds;
	}
	bool Constrains() const {
		return has_points || has_lower || has_upper || !key_filters.empty();
	}
	bool InBounds(const Value &value) const {
		if (has_lower && (value < lower || (!lower_inclusive && value == lower))) {
			return false;
		}
		if (has_upper && (value > upper || (!upper_inclusive && value == upper))) {
			return false;
		}
		return true;
	}
	void SetLower(Value value, bool inclusive) {
		if (!has_lower || value > lower || (value == lower && !inclusive)) {
			lower = std::move(value);
			lower_inclusive = inclusive;
			has_lower = true;
		}
	}
	void SetUpper(Value value, bool inclusive) {
		if (!has_upper || value < upper || (value == upper && !inclusive)) {
			upper = std::move(value);
			upper_inclusive = inclusive;
			has_upper = true;
		}
	}
	//! Both this and `other` hold
	void Intersect(KeySet &other) {
		if (other.has_points) {
			if (!has_points) {
				points = std::move(other.points);
				has_points = true;
				exact_points = other.exact_points;
			} else {
				exact_points = exact_points || other.exact_points;
				SortUnique(points);
				SortUnique(other.points);
				vector<Value> both;
				std::set_intersection(points.begin(), points.end(), other.points.begin(), other.points.end(),
				                      std::back_inserter(both), ValueLess);
				points = std::move(both);
			}
		}
		if (other.has_lower) {
			SetLower(std::move(other.lower), other.lower_inclusive);
		}
		if (other.has_upper) {
			SetUpper(std::move(other.upper), other.upper_inclusive);
		}
		for (auto &filter : other.key_filters) {
			key_filters.push_back(filter);
		}
		exact_bounds = exact_bounds || other.exact_bounds;
	}
	//! Drop points outside the bounds, and duplicates
	void Normalize() {
		if (!has_points) {
			return;
		}
		vector<Value> result;
		for (auto &point : points) {
			if (InBounds(point)) {
				result.push_back(point);
			}
		}
		SortUnique(result);
		points = std::move(result);
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

//! A filter constant as a value of the column's type
static bool CastConstant(const Expression &constant, const LogicalType &type, Value &result) {
	auto cast = constant.Cast<BoundConstantExpression>().GetValue().DefaultTryCastAs(type);
	if (!cast) {
		return false;
	}
	result = std::move(*cast);
	return true;
}

//! Fill `keys` with what `expr`, a filter over BoundReference(0) of type `type`,
//! says about the column's values; false when it says nothing usable. Under
//! `optional` the filter is a hint (see KeySet).
static bool AnalyzeFilter(const Expression &expr, const LogicalType &type, KeySet &keys, bool optional) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION && !BoundComparisonExpression::IsComparison(expr) &&
	    expr.GetExpressionType() != ExpressionType::COMPARE_BETWEEN) {
		auto &function = expr.Cast<BoundFunctionExpression>();
		auto &name = function.Function().GetName();
		if (name == OptionalFilterScalarFun::NAME && function.BindInfo()) {
			auto &child = function.BindInfo()->Cast<OptionalFilterFunctionData>().child_filter_expr;
			return child && AnalyzeFilter(*child, type, keys, true);
		}
		if (name == SelectivityOptionalFilterScalarFun::NAME && function.BindInfo()) {
			auto &child = function.BindInfo()->Cast<SelectivityOptionalFilterFunctionData>().child_filter_expr;
			return child && AnalyzeFilter(*child, type, keys, true);
		}
		if (name == PrefixRangeScalarFun::NAME && function.BindInfo()) {
			// A join's key bitmap: only over the raw column, for 64-bit unsigned keys
			// (ids and REFs), which can be enumerated
			auto &data = function.BindInfo()->Cast<PrefixRangeFunctionData>();
			if (function.GetChildren().empty() ||
			    function.GetChildren()[0]->GetExpressionType() != ExpressionType::BOUND_REF || !data.filter ||
			    !data.filter->IsInitialized() || data.key_type.InternalType() != PhysicalType::UINT64 ||
			    type.InternalType() != PhysicalType::UINT64) {
				return false;
			}
			keys.key_filters.push_back(data);
			return true;
		}
		return false;
	}
	switch (expr.GetExpressionType()) {
	case ExpressionType::CONJUNCTION_AND: {
		bool any = false;
		for (auto &child : expr.Cast<BoundConjunctionExpression>().GetChildren()) {
			KeySet child_keys;
			if (AnalyzeFilter(*child, type, child_keys, optional)) {
				keys.Intersect(child_keys);
				any = true;
			}
		}
		return any;
	}
	case ExpressionType::CONJUNCTION_OR: {
		// A union of point sets only
		vector<Value> points;
		bool exact = true;
		for (auto &child : expr.Cast<BoundConjunctionExpression>().GetChildren()) {
			KeySet child_keys;
			if (!AnalyzeFilter(*child, type, child_keys, optional) || !child_keys.has_points ||
			    !child_keys.key_filters.empty()) {
				return false;
			}
			child_keys.Normalize();
			points.insert(points.end(), child_keys.points.begin(), child_keys.points.end());
			// The union is as exact as its least exact part
			exact = exact && child_keys.exact_points;
		}
		keys.has_points = true;
		keys.points = std::move(points);
		keys.exact_points = exact;
		return true;
	}
	case ExpressionType::COMPARE_IN: {
		auto &children = expr.Cast<BoundOperatorExpression>().GetChildren();
		if (children.empty() || children[0]->GetExpressionType() != ExpressionType::BOUND_REF) {
			return false;
		}
		vector<Value> points;
		for (idx_t i = 1; i < children.size(); i++) {
			if (children[i]->GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
				return false;
			}
			Value value;
			if (!CastConstant(*children[i], type, value)) {
				return false;
			}
			if (!value.IsNull()) {
				points.push_back(std::move(value));
			}
		}
		keys.has_points = true;
		keys.points = std::move(points);
		keys.exact_points = !optional;
		return true;
	}
	case ExpressionType::COMPARE_BETWEEN: {
		if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
			return false;
		}
		auto &between = expr.Cast<BoundFunctionExpression>();
		if (!BoundBetweenExpression::HasValidBindData(between) ||
		    BoundBetweenExpression::Input(between).GetExpressionType() != ExpressionType::BOUND_REF ||
		    BoundBetweenExpression::LowerBound(between).GetExpressionType() != ExpressionType::VALUE_CONSTANT ||
		    BoundBetweenExpression::UpperBound(between).GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
			return false;
		}
		Value lower, upper;
		if (!CastConstant(BoundBetweenExpression::LowerBound(between), type, lower) ||
		    !CastConstant(BoundBetweenExpression::UpperBound(between), type, upper)) {
			return false;
		}
		keys.exact_points = keys.exact_bounds = !optional;
		if (lower.IsNull() || upper.IsNull()) {
			// Never true
			keys.has_points = true;
			return true;
		}
		keys.SetLower(std::move(lower), BoundBetweenExpression::LowerInclusive(between));
		keys.SetUpper(std::move(upper), BoundBetweenExpression::UpperInclusive(between));
		return true;
	}
	default:
		break;
	}
	if (!BoundComparisonExpression::IsComparison(expr)) {
		return false;
	}
	auto &function = expr.Cast<BoundFunctionExpression>();
	auto &left = BoundComparisonExpression::Left(function);
	auto comparison = expr.GetExpressionType();
	optional_ptr<const Expression> constant;
	auto column = ColumnSide(left, BoundComparisonExpression::Right(function), constant);
	if (!column) {
		return false;
	}
	if (column.get() != &left) {
		comparison = FlipComparisonExpression(comparison);
	}
	Value value;
	if (!CastConstant(*constant, type, value)) {
		return false;
	}
	switch (comparison) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		break;
	default:
		return false;
	}
	keys.exact_points = keys.exact_bounds = !optional;
	if (value.IsNull()) {
		// Never true
		keys.has_points = true;
		return true;
	}
	switch (comparison) {
	case ExpressionType::COMPARE_EQUAL:
		keys.has_points = true;
		keys.points.push_back(std::move(value));
		break;
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		keys.SetUpper(std::move(value), comparison == ExpressionType::COMPARE_LESSTHANOREQUALTO);
		break;
	default:
		keys.SetLower(std::move(value), comparison == ExpressionType::COMPARE_GREATERTHANOREQUALTO);
		break;
	}
	return true;
}

//! The smallest well-formed entity id >= id (UINT64_MAX if none)
static EntityId WellFormedAtOrAfter(EntityId id) {
	if (mnemonicdb::IsWellFormed(id)) {
		return id;
	}
	auto part = mnemonicdb::PartitionOf(id);
	return part == UINT8_MAX ? UINT64_MAX : mnemonicdb::MakeEntityId(part + 1, 0);
}

//! The largest well-formed entity id <= id
static EntityId WellFormedAtOrBefore(EntityId id) {
	if (mnemonicdb::IsWellFormed(id)) {
		return id;
	}
	return mnemonicdb::MakeEntityId(mnemonicdb::PartitionOf(id), mnemonicdb::LOCAL_MASK);
}

//! A key set over entity ids (the id column, or a REF value): explicit points,
//! or an inclusive range [lower, upper], possibly with key bitmaps
struct EntityKeys {
	bool has_points = false;
	vector<EntityId> points;
	EntityId lower = 0;
	EntityId upper = UINT64_MAX;
	vector<reference<const PrefixRangeFunctionData>> key_filters;
	//! Whether the points come from a filter that is not optional
	bool exact_points = false;

	bool Empty() const {
		return lower > upper || (has_points && points.empty());
	}
	bool Bounded() const {
		return lower > 0 || upper < UINT64_MAX;
	}
	bool Contains(EntityId id) const {
		return id >= lower && id <= upper;
	}
};

static EntityKeys ToEntityKeys(const KeySet &keys) {
	EntityKeys result;
	result.exact_points = keys.has_points && keys.exact_points;
	result.key_filters = keys.key_filters;
	if (keys.has_lower) {
		auto lower = keys.lower.GetValue<uint64_t>();
		if (!keys.lower_inclusive) {
			if (lower == UINT64_MAX) {
				result.lower = 1;
				result.upper = 0;
				return result;
			}
			lower++;
		}
		result.lower = WellFormedAtOrAfter(lower);
	}
	if (keys.has_upper) {
		auto upper = keys.upper.GetValue<uint64_t>();
		if (!keys.upper_inclusive) {
			if (upper == 0) {
				result.lower = 1;
				result.upper = 0;
				return result;
			}
			upper--;
		}
		result.upper = WellFormedAtOrBefore(upper);
	}
	if (keys.has_points) {
		result.has_points = true;
		for (auto &point : keys.points) {
			auto id = point.GetValue<uint64_t>();
			if (mnemonicdb::IsWellFormed(id) && result.Contains(id)) {
				result.points.push_back(id);
			}
		}
	}
	return result;
}

//! Both hold
static void IntersectEntityKeys(EntityKeys &keys, EntityKeys &other) {
	keys.lower = MaxValue(keys.lower, other.lower);
	keys.upper = MinValue(keys.upper, other.upper);
	if (other.has_points) {
		if (keys.has_points) {
			std::sort(other.points.begin(), other.points.end());
			vector<EntityId> both;
			for (auto id : keys.points) {
				if (std::binary_search(other.points.begin(), other.points.end(), id)) {
					both.push_back(id);
				}
			}
			keys.points = std::move(both);
			keys.exact_points = keys.exact_points || other.exact_points;
		} else {
			keys.points = std::move(other.points);
			keys.has_points = true;
			keys.exact_points = other.exact_points;
		}
	}
	for (auto &filter : other.key_filters) {
		keys.key_filters.push_back(filter);
	}
	if (keys.has_points) {
		vector<EntityId> kept;
		for (auto id : keys.points) {
			if (keys.Contains(id)) {
				kept.push_back(id);
			}
		}
		keys.points = std::move(kept);
	}
}

//! Whether some key of every bitmap may lie in [lower, upper]
static bool KeysMayExist(const vector<reference<const PrefixRangeFunctionData>> &filters, EntityId lower,
                         EntityId upper) {
	for (auto &filter : filters) {
		if (filter.get().filter->LookupRange(Value::UBIGINT(lower), Value::UBIGINT(upper)) ==
		    FilterPropagateResult::FILTER_ALWAYS_FALSE) {
			return false;
		}
	}
	return true;
}

//! List the keys of the bitmaps in [lower, upper] by bisection. Returns false
//! if there are more than `limit` (a bitmap that is not exact covers whole
//! buckets of keys).
static bool EnumerateKeys(const vector<reference<const PrefixRangeFunctionData>> &filters, EntityId lower,
                          EntityId upper, idx_t limit, vector<EntityId> &result) {
	if (lower > upper || !KeysMayExist(filters, lower, upper)) {
		return true;
	}
	if (lower == upper) {
		if (mnemonicdb::IsWellFormed(lower)) {
			result.push_back(lower);
		}
		return result.size() <= limit;
	}
	auto middle = lower + (upper - lower) / 2;
	return EnumerateKeys(filters, lower, middle, limit, result) &&
	       EnumerateKeys(filters, middle + 1, upper, limit, result);
}

//! Resolve the key bitmaps of `keys` into points, if there are at most `limit`
static void ResolveKeyFilters(EntityKeys &keys, idx_t limit) {
	if (keys.has_points || keys.key_filters.empty() || keys.Empty()) {
		return;
	}
	vector<EntityId> points;
	if (EnumerateKeys(keys.key_filters, keys.lower, keys.upper, limit, points)) {
		keys.points = std::move(points);
		keys.has_points = true;
	}
}

//! Encoded value ranges of an AVET-indexed attribute for a key set; false if
//! the key set says nothing about the value
static bool EncodedRanges(const AttributeDef &attr, const KeySet &keys, vector<ValueRange> &ranges) {
	if (keys.has_points) {
		for (auto &point : keys.points) {
			if (attr.is_ref && !mnemonicdb::IsWellFormed(point.GetValue<uint64_t>())) {
				continue;
			}
			ValueRange range;
			range.lower = range.upper = attr.codec.EncodeValue(point);
			range.has_lower = range.has_upper = true;
			ranges.push_back(std::move(range));
		}
		return true;
	}
	if (!keys.has_lower && !keys.has_upper) {
		return false;
	}
	ValueRange range;
	if (attr.is_ref) {
		auto bounds = ToEntityKeys(keys);
		if (bounds.Empty()) {
			return true;
		}
		range.lower = attr.codec.EncodeValue(Value::UBIGINT(bounds.lower));
		range.has_lower = true;
		if (bounds.upper != UINT64_MAX) {
			range.upper = attr.codec.EncodeValue(Value::UBIGINT(WellFormedAtOrBefore(bounds.upper)));
			range.has_upper = true;
		}
		ranges.push_back(std::move(range));
		return true;
	}
	if (keys.has_lower) {
		range.lower = attr.codec.EncodeValue(keys.lower);
		range.has_lower = true;
		range.lower_inclusive = keys.lower_inclusive;
	}
	if (keys.has_upper) {
		range.upper = attr.codec.EncodeValue(keys.upper);
		range.has_upper = true;
		range.upper_inclusive = keys.upper_inclusive;
	}
	if (range.has_lower && range.has_upper &&
	    (range.lower > range.upper ||
	     (range.lower == range.upper && !(range.lower_inclusive && range.upper_inclusive)))) {
		return true;
	}
	ranges.push_back(std::move(range));
	return true;
}

//! Entities with a visible AVET entry for `attr` inside one of `ranges`; false
//! once there are more than `limit`
static bool EntitiesInRanges(mnemonicdb::ReadView &view, const AttributeDef &attr, const vector<ValueRange> &ranges,
                             optional_idx as_of, optional_idx limit, vector<EntityId> &result) {
	constexpr size_t tail = mnemonicdb::PACKED_ID_SIZE + mnemonicdb::PACKED_TX_SIZE;
	auto prefix = mnemonicdb::AvetPrefix(attr.Local());
	for (auto &range : ranges) {
		auto scan = [&](ColumnFamily family, bool is_history) {
			// A point range covers one whole A+V prefix: let RocksDB use its bounds
			auto point = range.has_lower && range.has_upper && range.lower == range.upper && range.lower_inclusive &&
			             range.upper_inclusive;
			auto it = view.NewIterator(family, point ? prefix + range.lower : prefix);
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
					if (limit.IsValid() && result.size() > limit.GetIndex()) {
						return false;
					}
				}
			}
			if (!it->status().ok()) {
				throw IOException("MnemonicDB: RocksDB error: %s", it->status().ToString());
			}
			return true;
		};
		if (!scan(ColumnFamily::AVET, false)) {
			return false;
		}
		if (as_of.IsValid() && !scan(ColumnFamily::AVET_HIST, true)) {
			return false;
		}
	}
	return true;
}

//! Entities whose REF attribute `attr` has a visible value among `targets`,
//! from VAET (V6 A2 E6 T5); false once there are more than `limit`
static bool EntitiesReferencing(mnemonicdb::ReadView &view, const AttributeDef &attr,
                                const vector<EntityId> &targets, optional_idx as_of, optional_idx limit,
                                vector<EntityId> &result) {
	mnemonicdb::PrefixReader reader(view);
	constexpr size_t head = mnemonicdb::PACKED_ID_SIZE + mnemonicdb::PACKED_ATTR_SIZE;
	for (auto target : targets) {
		ForEachVisible(reader, ColumnFamily::VAET, ColumnFamily::VAET_HIST, mnemonicdb::VaetPrefix(target, attr.Local()),
		               as_of, [&](std::string_view key) { result.push_back(mnemonicdb::GetEntityId(key.data() + head)); });
		if (limit.IsValid() && result.size() > limit.GetIndex()) {
			return false;
		}
	}
	return true;
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

//! Whether the filter is false or NULL for a NULL column value, so rows without
//! the attribute never pass it
static bool IsNullRejecting(const Expression &expr) {
	auto is_column = [](const Expression &child) {
		return child.GetExpressionType() == ExpressionType::BOUND_REF;
	};
	switch (expr.GetExpressionType()) {
	case ExpressionType::CONJUNCTION_AND:
		for (auto &child : expr.Cast<BoundConjunctionExpression>().GetChildren()) {
			if (IsNullRejecting(*child)) {
				return true;
			}
		}
		return false;
	case ExpressionType::CONJUNCTION_OR:
		for (auto &child : expr.Cast<BoundConjunctionExpression>().GetChildren()) {
			if (!IsNullRejecting(*child)) {
				return false;
			}
		}
		return true;
	case ExpressionType::OPERATOR_IS_NOT_NULL:
	case ExpressionType::COMPARE_IN: {
		auto &children = expr.Cast<BoundOperatorExpression>().GetChildren();
		return !children.empty() && is_column(*children[0]);
	}
	case ExpressionType::COMPARE_BETWEEN:
		if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
			return false;
		}
		return BoundBetweenExpression::HasValidBindData(expr.Cast<BoundFunctionExpression>()) &&
		       is_column(BoundBetweenExpression::Input(expr.Cast<BoundFunctionExpression>()));
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOTEQUAL:
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO: {
		if (!BoundComparisonExpression::IsComparison(expr)) {
			return false;
		}
		auto &function = expr.Cast<BoundFunctionExpression>();
		return is_column(BoundComparisonExpression::Left(function)) ||
		       is_column(BoundComparisonExpression::Right(function));
	}
	default:
		return false;
	}
}

//! Point lookups for candidates from optional (join) filters are used while
//! there are at most rows / ROWS_PER_HINT_KEY of them (and at least
//! MIN_HINT_KEYS); beyond that a merge scan is cheaper
static constexpr idx_t ROWS_PER_HINT_KEY = 64;
static constexpr idx_t MIN_HINT_KEYS = 1024;
//! Below this many estimated rows a scan runs as a single task
static constexpr idx_t PARALLEL_SCAN_THRESHOLD = 100000;
//! Rough number of rows per task in a parallel scan
static constexpr idx_t ROWS_PER_TASK = 50000;
static constexpr idx_t MAX_SCAN_TASKS = 256;

//! The smallest and largest entity with a datom (visible or historical) of any
//! of the scan's attributes
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
		idx_t per_task =
		    count < 4096 || threads <= 1 ? MaxValue<idx_t>(count, 1) : MaxValue<idx_t>(1024, count / (threads * 4));
		for (idx_t begin = 0; begin < count || begin == 0; begin += per_task) {
			ScanTask task;
			task.candidate_begin = begin;
			task.candidate_end = MinValue<idx_t>(count, begin + per_task);
			state.tasks.push_back(task);
			if (count == 0) {
				break;
			}
		}
		return;
	}
	EntityId min_entity = 0;
	EntityId max_entity = 0;
	if (state.range_lower > state.range_upper) {
		// An empty key set: read nothing
		return;
	}
	if (!EntityBounds(view, state, bind_data.as_of.IsValid(), min_entity, max_entity)) {
		state.tasks.push_back(ScanTask {});
		return;
	}
	min_entity = MaxValue(min_entity, state.range_lower);
	max_entity = MinValue(max_entity, state.range_upper);
	if (min_entity > max_entity) {
		return;
	}
	// Work is proportional to what the scan reads, not what it returns: size
	// tasks by the largest table
	idx_t rows = 0;
	for (auto &table : bind_data.tables) {
		rows = MaxValue(rows, EstimateTableRows(table.table.get()));
	}
	idx_t task_count = 1;
	if (threads > 1 && rows >= PARALLEL_SCAN_THRESHOLD) {
		task_count = MinValue<idx_t>(MAX_SCAN_TASKS, MaxValue<idx_t>(threads, rows / ROWS_PER_TASK));
	}
	// Entity ids are allocated in order, so an even split of the id range is an
	// even split of the rows
	auto span = max_entity - min_entity + 1;
	task_count = MinValue<idx_t>(task_count, span);
	for (idx_t i = 0; i < task_count; i++) {
		ScanTask task;
		// A split point between partitions is not an encodable id: start at the
		// next one that is
		auto split = [&](idx_t n) {
			return min_entity + EntityId(static_cast<unsigned __int128>(span) * n / task_count);
		};
		task.start = WellFormedAtOrAfter(split(i));
		task.end = i + 1 == task_count ? max_entity + 1 : split(i + 1);
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

	// Every pushed filter that is not purely optional is applied exactly to each
	// row. Filters of every kind (exact, optional, dynamic join filters) may also
	// narrow the scan: to candidate entities (lookup mode) or to a range of
	// entities (merge mode).
	EntityKeys id_keys;
	bool id_constrained = false;
	struct AttributeKeys {
		idx_t attribute;
		KeySet keys;
	};
	vector<AttributeKeys> attribute_keys;
	if (input.filters) {
		for (auto &entry : *input.filters) {
			auto &filter = entry.Filter();
			auto &expression_filter = ExpressionFilter::GetExpressionFilter(filter, "mnemonicdb scan");
			auto column = entry.GetIndex().GetIndex();
			if (!ExpressionFilter::IsOptionalFilter(filter)) {
				state->filters.push_back(ScanFilter {column, expression_filter});
			}
			auto &scan_column = state->columns[column];
			if (scan_column.kind == ScanColumn::Kind::EMPTY) {
				continue;
			}
			KeySet keys;
			if (!AnalyzeFilter(*expression_filter.expr, state->column_types[column], keys, false) ||
			    !keys.Constrains()) {
				continue;
			}
			keys.Normalize();
			if (scan_column.kind == ScanColumn::Kind::ID) {
				// Every id column of a (fused) scan holds the same entity, or NULL,
				// which no key set contains
				auto entity_keys = ToEntityKeys(keys);
				if (id_constrained) {
					IntersectEntityKeys(id_keys, entity_keys);
				} else {
					id_keys = std::move(entity_keys);
					id_constrained = true;
				}
				continue;
			}
			auto &attr = state->attributes[scan_column.attribute].attr.get();
			if (attr.many || (!attr.HasAvet() && !attr.is_ref)) {
				continue;
			}
			attribute_keys.push_back(AttributeKeys {scan_column.attribute, std::move(keys)});
		}
	}

	// Candidates found through hints (not exact filters) are only worth point
	// lookups when they are few compared to the table
	idx_t rows = 0;
	for (auto &table : bind_data.tables) {
		rows = MaxValue(rows, EstimateTableRows(table.table.get()));
	}
	idx_t hint_limit = MaxValue<idx_t>(MIN_HINT_KEYS, rows / ROWS_PER_HINT_KEY);
#ifdef DEBUG
	// The join that marked this scan pushes into every one of its columns, or
	// (empty build side) into none
	if (bind_data.join_key_filters && bind_data.join_key_filters->HasFilters()) {
		for (auto column : bind_data.join_key_columns) {
			D_ASSERT(input.filters && input.filters->HasFilter(ProjectionIndex(column)));
		}
	}
#endif
	if (bind_data.join_key_filters && !bind_data.join_key_filters->HasFilters()) {
		// The join feeding this scan its key set has an empty build side: no row
		// can match
		state->lookup_mode = true;
	} else if (id_constrained) {
		if (id_keys.Empty()) {
			state->lookup_mode = true;
		} else {
			if (!id_keys.has_points && !id_keys.key_filters.empty()) {
				// Enumerate a join's key bitmap within the entities the scan could see
				EntityId min_entity = 0;
				EntityId max_entity = 0;
				if (EntityBounds(view, *state, bind_data.as_of.IsValid(), min_entity, max_entity)) {
					id_keys.lower = MaxValue(id_keys.lower, min_entity);
					id_keys.upper = MinValue(id_keys.upper, max_entity);
					ResolveKeyFilters(id_keys, hint_limit);
				} else {
					id_keys.has_points = true;
				}
			}
			if (id_keys.has_points && !id_keys.exact_points && id_keys.points.size() > hint_limit) {
				// A long hint (dynamic_or_filter_threshold raised): its bounds still help
				auto bounds = std::minmax_element(id_keys.points.begin(), id_keys.points.end());
				id_keys.lower = MaxValue(id_keys.lower, *bounds.first);
				id_keys.upper = MinValue(id_keys.upper, *bounds.second);
				id_keys.has_points = false;
			}
			if (id_keys.has_points) {
				state->lookup_mode = true;
				state->candidates = std::move(id_keys.points);
			}
		}
		state->range_lower = id_keys.lower;
		state->range_upper = id_keys.upper;
	}
	if (!state->lookup_mode) {
		// Exact filters first: they were written for this
		std::stable_sort(attribute_keys.begin(), attribute_keys.end(),
		                 [](const AttributeKeys &a, const AttributeKeys &b) { return a.keys.Exact() > b.keys.Exact(); });
		for (auto &entry : attribute_keys) {
			auto &attr = state->attributes[entry.attribute].attr.get();
			auto limit = entry.keys.Exact() ? optional_idx() : optional_idx(hint_limit);
			vector<EntityId> candidates;
			bool found = false;
			if (attr.HasAvet()) {
				KeySet keys = entry.keys;
				if (attr.is_ref && !keys.has_points && !keys.key_filters.empty()) {
					auto values = ToEntityKeys(keys);
					ResolveKeyFilters(values, hint_limit);
					if (values.has_points) {
						keys.has_points = true;
						for (auto id : values.points) {
							keys.points.push_back(Value::UBIGINT(id));
						}
					}
				}
				vector<ValueRange> ranges;
				found = EncodedRanges(attr, keys, ranges) &&
				        EntitiesInRanges(view, attr, ranges, bind_data.as_of, limit, candidates);
			} else {
				// A REF without AVET: point lookups in VAET
				auto values = ToEntityKeys(entry.keys);
				ResolveKeyFilters(values, hint_limit);
				found = values.has_points &&
				        EntitiesReferencing(view, attr, values.points, bind_data.as_of, limit, candidates);
			}
			if (found) {
				state->lookup_mode = true;
				state->candidates = std::move(candidates);
				break;
			}
		}
		if (state->lookup_mode && state->range_lower <= state->range_upper) {
			vector<EntityId> kept;
			for (auto id : state->candidates) {
				if (id >= state->range_lower && id <= state->range_upper) {
					kept.push_back(id);
				}
			}
			state->candidates = std::move(kept);
		} else if (state->lookup_mode) {
			state->candidates.clear();
		}
	}
	// Choose which attributes to read
	auto table_count = bind_data.tables.size();
	state->table_driver.resize(table_count);
	vector<bool> filtered(state->attributes.size());
	for (auto &filter : state->filters) {
		auto &column = state->columns[filter.column];
		if (column.kind != ScanColumn::Kind::ATTRIBUTE) {
			continue;
		}
		filtered[column.attribute] = true;
		auto t = state->attributes[column.attribute].table;
		if (!state->table_driver[t].IsValid() && IsNullRejecting(*filter.filter.expr)) {
			state->table_driver[t] = column.attribute;
		}
	}
	state->attribute_active.resize(state->attributes.size());
	for (idx_t a = 0; a < state->attributes.size(); a++) {
		auto t = state->attributes[a].table;
		state->attribute_active[a] = !state->table_driver[t].IsValid() || !state->attribute_columns[a].empty() ||
		                             filtered[a] || state->table_driver[t].GetIndex() == a;
	}
	PlanTasks(context, bind_data, *state, view);
	return std::move(state);
}

static unique_ptr<LocalTableFunctionState> MnemonicScanInitLocal(ExecutionContext &context, TableFunctionInitInput &,
                                                                 GlobalTableFunctionState *global_state) {
	auto &state = global_state->Cast<MnemonicScanState>();
	auto local = make_uniq<MnemonicScanLocalState>();
	local->scan_chunk.Initialize(context.client, state.column_types);
	for (auto &filter : state.filters) {
		local->executors.push_back(make_uniq<ExpressionExecutor>(context.client, *filter.filter.expr));
	}
	return std::move(local);
}

//! Whether the entity is a member of table `t`, given the values read for every
//! attribute Lookup mode: whether the entity is a member of table `t`, given
//! the values read for its attributes
static bool IsMember(const MnemonicScanState &state, const vector<vector<string>> &values, idx_t t) {
	if (state.table_driver[t].IsValid()) {
		return !values[state.table_driver[t].GetIndex()].empty();
	}
	for (idx_t a = state.table_attributes[t].first; a < state.table_attributes[t].second; a++) {
		if (!values[a].empty()) {
			return true;
		}
	}
	return false;
}

//! Appends decoded list elements to row `row` of a LIST vector, writing
//! straight into its child vector
struct ListAppender {
	ListAppender(Vector &vector, idx_t row) : vector(vector), row(row), start(ListVector::GetListSize(vector)) {
	}
	void Append(const mnemonicdb::ValueCodec &codec, std::string_view encoded) {
		auto size = start + length;
		ListVector::Reserve(vector, size + 1);
		codec.Decode(encoded, ListVector::GetChildMutable(vector), size);
		length++;
		ListVector::SetListSize(vector, size + 1);
	}
	void Finish() {
		FlatVector::GetDataMutable<list_entry_t>(vector)[row] = list_entry_t(start, length);
	}

	Vector &vector;
	idx_t row;
	idx_t start;
	idx_t length = 0;
};

static void WriteIds(MnemonicScanState &state, DataChunk &chunk, EntityId entity, const vector<bool> &member,
                     idx_t row) {
	for (idx_t t = 0; t < member.size(); t++) {
		for (auto column : state.id_columns[t]) {
			if (member[t]) {
				FlatVector::GetDataMutable<uint64_t>(chunk.data[column])[row] = entity;
			} else {
				FlatVector::SetNull(chunk.data[column], row, true);
			}
		}
	}
}

//! Lookup mode: write one entity's values into row `row`, or return false if it
//! is not a member of every required table. `values[a]` holds the encoded
//! values of attribute a.
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
		for (auto column : columns) {
			if (!attr.many) {
				attr.codec.Decode(values[a][0], chunk.data[column], row);
				continue;
			}
			ListAppender list(chunk.data[column], row);
			for (auto &v : values[a]) {
				list.Append(attr.codec, v);
			}
			list.Finish();
		}
	}
	WriteIds(state, chunk, entity, member, row);
	return true;
}

//! The next entity that is a member of every required table, skipping ahead
//! (leapfrog) past entities missing from any of them; false when no such entity
//! remains. A table with a driving attribute is a member exactly where that
//! attribute has a value; otherwise where any of its attributes does.
static bool NextRequiredEntity(MnemonicScanState &state, MnemonicScanLocalState &local, EntityId &entity) {
	auto table_count = state.table_required.size();
	auto next_member = [&](idx_t t, EntityId &next) {
		if (state.table_driver[t].IsValid()) {
			auto &cursor = *local.cursors[state.table_driver[t].GetIndex()];
			next = cursor.entity;
			return cursor.valid;
		}
		bool any = false;
		for (idx_t a = state.table_attributes[t].first; a < state.table_attributes[t].second; a++) {
			auto &cursor = *local.cursors[a];
			if (cursor.valid && (!any || cursor.entity < next)) {
				next = cursor.entity;
				any = true;
			}
		}
		return any;
	};
	while (true) {
		bool agreed = true;
		bool first = true;
		EntityId candidate = 0;
		for (idx_t t = 0; t < table_count; t++) {
			if (!state.table_required[t]) {
				continue;
			}
			EntityId next = 0;
			if (!next_member(t, next)) {
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
			if (state.table_driver[t].IsValid()) {
				local.cursors[state.table_driver[t].GetIndex()]->SeekTo(candidate);
				continue;
			}
			for (idx_t a = state.table_attributes[t].first; a < state.table_attributes[t].second; a++) {
				local.cursors[a]->SeekTo(candidate);
			}
		}
	}
}

//! Merge mode: produce the next row straight from the cursors, decoding values
//! from the keys they are on. Returns false when the task's range is exhausted.
static bool NextMergedRow(MnemonicScanState &state, MnemonicScanLocalState &local, DataChunk &chunk, idx_t row) {
	EntityId entity = 0;
	if (!NextRequiredEntity(state, local, entity)) {
		return false;
	}
	local.entities_read++;
	auto table_count = state.table_required.size();
	// Bring every other cursor (projected, filtered, optional tables) up to the
	// entity
	for (auto &cursor : local.cursors) {
		if (cursor) {
			cursor->SeekTo(entity);
		}
	}
	vector<bool> member(table_count);
	for (idx_t t = 0; t < table_count; t++) {
		auto at_entity = [&](idx_t a) {
			auto &cursor = local.cursors[a];
			return cursor && cursor->valid && cursor->entity == entity;
		};
		if (state.table_driver[t].IsValid()) {
			member[t] = at_entity(state.table_driver[t].GetIndex());
			continue;
		}
		for (idx_t a = state.table_attributes[t].first; a < state.table_attributes[t].second && !member[t]; a++) {
			member[t] = at_entity(a);
		}
	}
	for (idx_t a = 0; a < local.cursors.size(); a++) {
		if (!local.cursors[a]) {
			continue;
		}
		auto &cursor = *local.cursors[a];
		auto &columns = state.attribute_columns[a];
		bool present = cursor.valid && cursor.entity == entity;
		if (!present) {
			for (auto column : columns) {
				FlatVector::SetNull(chunk.data[column], row, true);
			}
			continue;
		}
		auto &attr = cursor.attr;
		if (columns.empty()) {
			// Read only for membership or a filter on another attribute; skip its
			// values
		} else if (!attr.many) {
			for (auto column : columns) {
				attr.codec.Decode(cursor.Value(), chunk.data[column], row);
			}
		} else {
			vector<ListAppender> lists;
			for (auto column : columns) {
				lists.emplace_back(chunk.data[column], row);
			}
			while (cursor.valid && cursor.entity == entity) {
				for (auto &list : lists) {
					list.Append(attr.codec, cursor.Value());
				}
				cursor.Next();
			}
			for (auto &list : lists) {
				list.Finish();
			}
			continue;
		}
		while (cursor.valid && cursor.entity == entity) {
			cursor.Next();
		}
	}
	WriteIds(state, chunk, entity, member, row);
	return true;
}

//! Lookup mode: produce the next candidate entity's values; false when
//! exhausted
static bool NextCandidate(MnemonicScanState &state, MnemonicScanLocalState &local, optional_idx as_of, EntityId &entity,
                          vector<vector<string>> &values) {
	constexpr size_t head = mnemonicdb::PACKED_ID_SIZE + mnemonicdb::PACKED_ATTR_SIZE;
	while (local.next_candidate < local.task.candidate_end) {
		entity = state.candidates[local.next_candidate++];
		local.entities_read++;
		if (!mnemonicdb::IsWellFormed(entity)) {
			continue;
		}
		bool any = false;
		for (idx_t a = 0; a < state.attributes.size(); a++) {
			if (!state.attribute_active[a]) {
				continue;
			}
			auto &attr = state.attributes[a].attr.get();
			auto prefix = mnemonicdb::EavtPrefix(entity, attr.Local());
			ForEachVisible(*local.reader, ColumnFamily::EAVT, ColumnFamily::EAVT_HIST, prefix, as_of,
			               [&](std::string_view key) {
				               any = true;
				               values[a].emplace_back(key.substr(head, key.size() - head - mnemonicdb::PACKED_TX_SIZE));
			               });
			// Values from the history index arrive after the current ones; keep them
			// in value order
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
	local.cursors.resize(state.attributes.size());
	for (idx_t a = 0; a < state.attributes.size(); a++) {
		if (state.attribute_active[a]) {
			local.cursors[a] = make_uniq<AttributeCursor>(view, state.attributes[a].attr.get(), as_of, local.task.start,
			                                              local.task.end);
		}
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
			if (!state.lookup_mode) {
				if (NextMergedRow(state, local, chunk, count)) {
					count++;
				} else {
					local.active = false;
				}
				continue;
			}
			for (auto &v : values) {
				v.clear();
			}
			EntityId entity = 0;
			if (!NextCandidate(state, local, bind_data.as_of, entity, values)) {
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

//! The latest transaction whose instant is at or before `ts`, or 0 if there is
//! none
static mnemonicdb::TxId TxAtInstant(MnemonicTransaction &transaction, timestamp_tz_t ts) {
	auto &instant = transaction.GetSchema().GetAttribute(mnemonicdb::sys::TX_INSTANT);
	auto prefix = mnemonicdb::AvetPrefix(instant.Local());
	// AVET keys are A2 V E6 T5: seek to just past every key whose V is `ts`, then
	// step back
	auto upper = mnemonicdb::AvetPrefix(instant.Local(), instant.codec.EncodeValue(Value::TIMESTAMPTZ(ts))) +
	             string(mnemonicdb::PACKED_ID_SIZE + mnemonicdb::PACKED_TX_SIZE + 1, '\xff');
	auto it = transaction.GetCommittedView().NewIterator(ColumnFamily::AVET);
	it->SeekForPrev(upper);
	if (!it->Valid() || !it->key().starts_with(prefix)) {
		return 0;
	}
	auto datom =
	    mnemonicdb::DecodeKey(mnemonicdb::IndexKind::AVET, std::string_view(it->key().data(), it->key().size()));
	return mnemonicdb::LocalOf(datom.e);
}

static optional_idx ResolveAtClause(ClientContext &context, MnemonicTableEntry &table, BoundAtClause &at_clause) {
	auto &transaction = MnemonicTransaction::Get(context, table.catalog);
	auto unit = StringUtil::Upper(at_clause.Unit().GetIdentifierName());
	auto &value = at_clause.GetValue();
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
			throw BinderException("MnemonicDB: AT VERSION %d is later than the last "
			                      "committed transaction (%d)",
			                      tx, transaction.GetBasisTx());
		}
	} else if (unit == "TIMESTAMP") {
		tx = TxAtInstant(transaction, value.DefaultCastAs(LogicalType::TIMESTAMP_TZ).GetValue<timestamp_tz_t>());
	} else {
		throw BinderException("MnemonicDB: unsupported AT unit \"%s\": use VERSION or TIMESTAMP", unit);
	}
	return optional_idx(tx);
}

//! Estimated rows of one table: the largest current-datom count of its
//! single-valued attributes (a list column has one datom per element), falling
//! back to list columns for tables that only have those
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

} // namespace

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

//! Column statistics for the planner: only distinct counts (the id is unique,
//! and so are UNIQUE / PRIMARY KEY columns), never min/max, which DuckDB would
//! trust to prune filters
static unique_ptr<BaseStatistics> MnemonicScanStatistics(ClientContext &, const FunctionData *bind_data_p,
                                                         column_t column_id) {
	auto &bind_data = bind_data_p->Cast<MnemonicScanBindData>();
	if (bind_data.fused) {
		return nullptr;
	}
	auto &table = bind_data.tables[0].table.get();
	if (column_id == MNEMONIC_ROWID_COLUMN || column_id == MNEMONIC_ID_COLUMN_INDEX) {
		auto stats = BaseStatistics::CreateUnknown(mnemonicdb::RefType());
		stats.SetDistinctCount(MaxValue<idx_t>(EstimateTableRows(table), 1));
		return stats.ToUnique();
	}
	if (column_id == COLUMN_IDENTIFIER_EMPTY || column_id >= VIRTUAL_COLUMN_START ||
	    column_id > table.GetTableDef().attributes.size()) {
		return nullptr;
	}
	auto &attr = table.GetAttribute(column_id);
	if (attr.unique == mnemonicdb::Uniqueness::NONE || attr.many) {
		return nullptr;
	}
	auto &store = table.catalog.Cast<MnemonicCatalog>().GetStore();
	auto stats = BaseStatistics::CreateUnknown(attr.type);
	stats.SetDistinctCount(MaxValue<idx_t>(store.GetAttributeCount(attr.Local()), 1));
	return stats.ToUnique();
}

static void MnemonicScanMetrics(TableFunctionGetMetricsInput &input) {
	if (input.local_state) {
		input.operator_metrics.rows_scanned = input.local_state->Cast<MnemonicScanLocalState>().entities_read;
	}
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
	function.statistics = MnemonicScanStatistics;
	function.get_metrics = MnemonicScanMetrics;
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
