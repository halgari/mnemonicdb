#include "mnemonicdb/mnemonic_fusion.hpp"
#include "mnemonicdb/mnemonic_scan.hpp"
#include "mnemonicdb/mnemonic_table_entry.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/table_filter_set.hpp"

namespace duckdb {

static constexpr const char *FUSE_SETTING = "mnemonicdb_fuse_scans";

static optional_ptr<MnemonicScanBindData> ScanBindData(LogicalOperator &op) {
	if (op.type != LogicalOperatorType::LOGICAL_GET) {
		return nullptr;
	}
	auto &get = op.Cast<LogicalGet>();
	if (get.function.name != "mnemonicdb_scan" || !get.bind_data) {
		return nullptr;
	}
	return &get.bind_data->Cast<MnemonicScanBindData>();
}

static bool ContainsDML(LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_INSERT:
	case LogicalOperatorType::LOGICAL_UPDATE:
	case LogicalOperatorType::LOGICAL_DELETE:
	case LogicalOperatorType::LOGICAL_MERGE_INTO:
	case LogicalOperatorType::LOGICAL_CREATE_TABLE:
		return true;
	default:
		break;
	}
	for (auto &child : op.children) {
		if (ContainsDML(*child)) {
			return true;
		}
	}
	return false;
}

//! The table of the scan whose entity id column `binding` is, if it is one
static optional_idx IdColumnTable(LogicalGet &get, const MnemonicScanBindData &bind_data,
                                  const ColumnBinding &binding) {
	if (binding.table_index != get.table_index) {
		return optional_idx();
	}
	auto column = get.GetColumnIndex(binding).GetPrimaryIndex();
	if (!bind_data.fused) {
		if (column == MNEMONIC_ROWID_COLUMN || column == MNEMONIC_ID_COLUMN_INDEX) {
			return 0;
		}
		return optional_idx();
	}
	for (idx_t t = 0; t < bind_data.tables.size(); t++) {
		if (column == FusedRowIdColumn(t) || column == bind_data.tables[t].column_offset) {
			return t;
		}
	}
	return optional_idx();
}

//! Maps the column ids of one side's scan into the fused scan's column space
static column_t MapColumn(const MnemonicScanBindData &old_data, idx_t table_base, idx_t column_base, column_t column) {
	if (column == COLUMN_IDENTIFIER_EMPTY) {
		return column;
	}
	if (!old_data.fused) {
		if (column == MNEMONIC_ROWID_COLUMN) {
			return FusedRowIdColumn(table_base);
		}
		return column_base + column;
	}
	if (column >= FusedRowIdColumn(0) && column < FusedRowIdColumn(old_data.tables.size())) {
		return FusedRowIdColumn(table_base + column - FusedRowIdColumn(0));
	}
	return column_base + column;
}

namespace {

//! Join filters (a parent join's key set, pushed into a scan at runtime) that
//! targeted one side of a fused join, to be redirected to the fused scan
struct JoinFilterRemap {
	shared_ptr<DynamicTableFilterSet> old_filters;
	TableIndex old_table;
	idx_t column_offset;
	shared_ptr<DynamicTableFilterSet> new_filters;
	TableIndex new_table;
};

} // namespace

//! Point every join filter target of `remap` at the fused scan, shifting its
//! columns to the fused scan's positions
static void RemapJoinFilters(LogicalOperator &op, const JoinFilterRemap &remap) {
	if (op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		auto &join = op.Cast<LogicalComparisonJoin>();
		if (join.filter_pushdown) {
			for (auto &probe : join.filter_pushdown->probe_info) {
				if (probe.dynamic_filters != remap.old_filters) {
					continue;
				}
				probe.dynamic_filters = remap.new_filters;
				for (auto &column : probe.columns) {
					if (column.probe_column_index.table_index == remap.old_table) {
						column.probe_column_index = ColumnBinding(
						    remap.new_table,
						    ProjectionIndex(column.probe_column_index.column_index.GetIndex() + remap.column_offset));
					}
				}
			}
		}
	}
	for (auto &child : op.children) {
		RemapJoinFilters(*child, remap);
	}
}

//! Try to replace `op`, a join of two MnemonicDB scans on their entity ids,
//! with a single fused scan
static bool TryFuse(Binder &binder, unique_ptr<LogicalOperator> &op, vector<ReplacementBinding> &replacements,
                    vector<JoinFilterRemap> &remaps) {
	if (op->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return false;
	}
	auto &join = op->Cast<LogicalComparisonJoin>();
	auto join_type = join.join_type;
	if ((join_type != JoinType::INNER && join_type != JoinType::LEFT && join_type != JoinType::RIGHT) ||
	    join.conditions.size() != 1 || !join.duplicate_eliminated_columns.empty()) {
		return false;
	}
	auto left_data = ScanBindData(*join.children[0]);
	auto right_data = ScanBindData(*join.children[1]);
	if (!left_data || !right_data) {
		return false;
	}
	auto &left = join.children[0]->Cast<LogicalGet>();
	auto &right = join.children[1]->Cast<LogicalGet>();
	if (&left_data->tables[0].table.get().catalog != &right_data->tables[0].table.get().catalog ||
	    left_data->as_of != right_data->as_of) {
		return false;
	}
	// An outer join keeps rows whose other side is missing: only a single table
	// can then be optional, and filters pushed into the optional side restrict
	// which of its rows match, not which output rows exist, so they cannot become
	// filters of the fused scan
	if ((join_type == JoinType::LEFT && (right_data->tables.size() != 1 || right.table_filters.HasFilters())) ||
	    (join_type == JoinType::RIGHT && (left_data->tables.size() != 1 || left.table_filters.HasFilters()))) {
		return false;
	}
	auto &condition = join.conditions[0];
	if (!condition.IsComparison() ||
	    (condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL &&
	     condition.GetComparisonType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM) ||
	    condition.GetLHS().GetExpressionType() != ExpressionType::BOUND_COLUMN_REF ||
	    condition.GetRHS().GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return false;
	}
	auto left_table = IdColumnTable(left, *left_data, condition.GetLHS().Cast<BoundColumnRefExpression>().Binding());
	auto right_table = IdColumnTable(right, *right_data, condition.GetRHS().Cast<BoundColumnRefExpression>().Binding());
	if (!left_table.IsValid() || !right_table.IsValid()) {
		return false;
	}

	// Fused bind data: the left tables, then the right tables
	auto fused = make_uniq<MnemonicScanBindData>();
	fused->fused = true;
	fused->as_of = left_data->as_of;
	idx_t column_offset = 0;
	auto add_tables = [&](const MnemonicScanBindData &data, bool keep_required) {
		for (auto &table : data.tables) {
			fused->tables.push_back(MnemonicScanTable {table.table, keep_required && table.required, column_offset});
			column_offset += table.table.get().GetColumns().PhysicalColumnCount();
		}
	};
	add_tables(*left_data, join_type != JoinType::RIGHT);
	auto right_table_base = fused->tables.size();
	auto right_column_base = column_offset;
	add_tables(*right_data, join_type != JoinType::LEFT);
	// The joined id must exist on each side whose rows are not preserved (a NULL
	// id never matches)
	if (join_type != JoinType::RIGHT) {
		fused->tables[left_table.GetIndex()].required = true;
	}
	if (join_type != JoinType::LEFT) {
		fused->tables[right_table_base + right_table.GetIndex()].required = true;
	}

	vector<LogicalType> types;
	vector<Identifier> names;
	for (auto &table : fused->tables) {
		for (auto &column : table.table.get().GetColumns().Physical()) {
			types.push_back(column.Type());
			names.emplace_back(table.table.get().name.GetIdentifierName() + "." + column.Name().GetIdentifierName());
		}
	}
	virtual_column_map_t virtual_columns;
	for (idx_t t = 0; t < fused->tables.size(); t++) {
		virtual_columns.emplace(
		    FusedRowIdColumn(t),
		    TableColumn(Identifier(fused->tables[t].table.get().name.GetIdentifierName() + ".rowid"),
		                mnemonicdb::RefType()));
	}

	auto table_index = binder.GenerateTableIndex();
	auto get = make_uniq<LogicalGet>(table_index, MnemonicFusedScanFunction(), std::move(fused), std::move(types),
	                                 std::move(names), std::move(virtual_columns));
	auto &fused_data = get->bind_data->Cast<MnemonicScanBindData>();

	vector<ColumnIndex> column_ids;
	for (auto &column : left.GetColumnIds()) {
		column_ids.emplace_back(MapColumn(*left_data, 0, 0, column.GetPrimaryIndex()));
	}
	auto left_count = left.GetColumnIds().size();
	for (auto &column : right.GetColumnIds()) {
		column_ids.emplace_back(MapColumn(*right_data, right_table_base, right_column_base, column.GetPrimaryIndex()));
	}
	get->SetColumnIds(std::move(column_ids));
	(void)fused_data;

	// The fused scan must output exactly what the join did, in the same
	// positions: parents (e.g. a join's projection map) may refer to the join's
	// output by position
	column_binding_map_t<idx_t> new_position;
	for (idx_t i = 0; i < left.GetColumnIds().size(); i++) {
		new_position[ColumnBinding(left.table_index, ProjectionIndex(i))] = i;
	}
	for (idx_t i = 0; i < right.GetColumnIds().size(); i++) {
		new_position[ColumnBinding(right.table_index, ProjectionIndex(i))] = left_count + i;
	}
	for (auto &binding : join.GetColumnBindings()) {
		auto entry = new_position.find(binding);
		if (entry == new_position.end()) {
			return false;
		}
		get->projection_ids.emplace_back(entry->second);
	}

	auto move_filters = [&](LogicalGet &side, idx_t base) {
		for (auto &entry : side.table_filters) {
			get->table_filters.PushFilter(ProjectionIndex(base + entry.GetIndex().GetIndex()), entry.TakeFilter());
		}
	};
	move_filters(left, 0);
	move_filters(right, left_count);

	// Every column of either side keeps its position, now under the fused scan's
	// table index
	for (idx_t i = 0; i < left.GetColumnIds().size(); i++) {
		replacements.emplace_back(ColumnBinding(left.table_index, ProjectionIndex(i)),
		                          ColumnBinding(table_index, ProjectionIndex(i)));
	}
	for (idx_t i = 0; i < right.GetColumnIds().size(); i++) {
		replacements.emplace_back(ColumnBinding(right.table_index, ProjectionIndex(i)),
		                          ColumnBinding(table_index, ProjectionIndex(left_count + i)));
	}
	// Key sets that parent joins push into either side at runtime now go to the
	// fused scan
	if (left.dynamic_filters || right.dynamic_filters) {
		get->dynamic_filters = make_shared_ptr<DynamicTableFilterSet>();
		if (left.dynamic_filters) {
			remaps.push_back(
			    JoinFilterRemap {left.dynamic_filters, left.table_index, 0, get->dynamic_filters, table_index});
		}
		if (right.dynamic_filters) {
			remaps.push_back(
			    JoinFilterRemap {right.dynamic_filters, right.table_index, left_count, get->dynamic_filters, table_index});
		}
	}
	get->SetEstimatedCardinality(EstimateScanRows(get->bind_data->Cast<MnemonicScanBindData>()));
	get->ResolveOperatorTypes();
	op = std::move(get);
	return true;
}

//! Fuse the first fusable join found bottom-up; returns whether one was fused
static bool FuseOne(Binder &binder, unique_ptr<LogicalOperator> &op, vector<ReplacementBinding> &replacements,
                    vector<JoinFilterRemap> &remaps) {
	for (auto &child : op->children) {
		if (FuseOne(binder, child, replacements, remaps)) {
			return true;
		}
	}
	return TryFuse(binder, op, replacements, remaps);
}

//! Same type, ignoring aliases (REF is UBIGINT with an alias)
static bool SameStorageType(const LogicalType &a, const LogicalType &b) {
	if (a.id() != b.id()) {
		return false;
	}
	if (a.id() == LogicalTypeId::DECIMAL || a.id() == LogicalTypeId::ENUM || a.IsNested()) {
		return a == b;
	}
	return true;
}

//! The MnemonicDB scan reached from `op` through projections and filters only,
//! i.e. one that runs in the same pipeline as the operator above `op`
static optional_ptr<LogicalGet> PipelineScan(LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_FILTER:
		return op.children.size() == 1 ? PipelineScan(*op.children[0]) : nullptr;
	case LogicalOperatorType::LOGICAL_GET:
		if (!ScanBindData(op)) {
			return nullptr;
		}
		return &op.Cast<LogicalGet>();
	default:
		return nullptr;
	}
}

static void FuseScansInternal(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);

//! Mark scans that a hash join feeds a key set at runtime, so that a scan which
//! receives none knows the join's build side was empty (or all NULL) and reads
//! nothing. Only where this is certain:
//! - the join is an equality join, so it is planned as a hash join, which pushes
//!   a filter whenever its build side has a non-NULL key;
//! - its type drops probe rows without a match (INNER, SEMI, RIGHT, RIGHT_SEMI);
//! - the key and the scanned column have the same type, so pushing never fails
//!   on a cast;
//! - the scan runs in the join's probe pipeline (only projections and filters in
//!   between), so the build side has finished when the scan starts;
//! - not inside a recursive CTE, whose joins run repeatedly.
static void MarkJoinKeyScans(LogicalOperator &op, bool in_recursive_cte) {
	if (op.type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE) {
		in_recursive_cte = true;
	}
	if (op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN && !in_recursive_cte) {
		auto &join = op.Cast<LogicalComparisonJoin>();
		auto join_type = join.join_type;
		bool drops_unmatched = join_type == JoinType::INNER || join_type == JoinType::SEMI ||
		                       join_type == JoinType::RIGHT || join_type == JoinType::RIGHT_SEMI;
		bool has_equality = false;
		for (auto &condition : join.conditions) {
			if (condition.IsComparison() && condition.GetComparisonType() == ExpressionType::COMPARE_EQUAL) {
				has_equality = true;
			}
		}
		auto scan = join.children.empty() ? nullptr : PipelineScan(*join.children[0]);
		if (join.filter_pushdown && drops_unmatched && has_equality && scan && scan->dynamic_filters) {
			auto &info = *join.filter_pushdown;
			for (auto &probe : info.probe_info) {
				if (probe.dynamic_filters != scan->dynamic_filters || probe.columns.empty()) {
					continue;
				}
				bool certain = true;
				for (auto &column : probe.columns) {
					auto &condition = join.conditions[info.join_condition[column.join_filter_idx]];
					if (!condition.IsComparison() ||
					    condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL ||
					    !column.runtime_filter_casts.empty() ||
					    column.probe_column_index.table_index != scan->table_index ||
					    !SameStorageType(column.storage_type, condition.GetRHS().GetReturnType())) {
						certain = false;
					}
				}
				if (certain) {
					scan->bind_data->Cast<MnemonicScanBindData>().join_key_filters = scan->dynamic_filters;
				}
			}
		}
	}
	for (auto &child : op.children) {
		MarkJoinKeyScans(*child, in_recursive_cte);
	}
}

static void FuseScans(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	FuseScansInternal(input, plan);
	MarkJoinKeyScans(*plan, false);
}

static void FuseScansInternal(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	Value enabled;
	if (input.context.TryGetCurrentSetting(Identifier(FUSE_SETTING), enabled) && !enabled.IsNull() &&
	    !enabled.GetValue<bool>()) {
		return;
	}
	if (ContainsDML(*plan)) {
		return;
	}
	// One join at a time: the parent of a fused join must see the fused scan's
	// bindings before it can be fused
	while (true) {
		vector<ReplacementBinding> replacements;
		vector<JoinFilterRemap> remaps;
		if (!FuseOne(input.optimizer.binder, plan, replacements, remaps)) {
			break;
		}
		for (auto &remap : remaps) {
			RemapJoinFilters(*plan, remap);
		}
		ColumnBindingReplacer replacer;
		replacer.replacement_bindings = std::move(replacements);
		replacer.VisitOperator(*plan);
	}
}

void RegisterScanFusion(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption(Identifier(FUSE_SETTING),
	                          "Fuse scans of MnemonicDB tables joined on their entity id into one scan",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));
	OptimizerExtension extension;
	extension.optimize_function = FuseScans;
	OptimizerExtension::Register(config, std::move(extension));
}

} // namespace duckdb
