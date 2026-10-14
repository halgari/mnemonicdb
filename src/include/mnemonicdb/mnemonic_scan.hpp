#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

class MnemonicTableEntry;
class BoundAtClause;
class DynamicTableFilterSet;

//! One table read by a scan. A fused scan (several tables joined on their
//! entity id) reads several; an entity is produced when it is a member of every
//! required table, and the columns of optional tables it is not a member of are
//! NULL (LEFT JOIN).
struct MnemonicScanTable {
	reference<MnemonicTableEntry> table;
	bool required;
	//! First column of this table in the fused scan's column space
	idx_t column_offset;
};

struct MnemonicScanBindData : public TableFunctionData {
	vector<MnemonicScanTable> tables;
	//! Whether column ids use the fused column space (see FusedRowIdColumn)
	//! instead of the single table's
	bool fused = false;
	//! The transaction to read as of (AT clause); unset reads the current state
	optional_idx as_of;
	//! Set by the optimizer when a hash join in the scan's pipeline will push its
	//! key set into these filters at runtime, and pushes nothing only when its
	//! build side has no key: the scan then reads nothing
	shared_ptr<DynamicTableFilterSet> join_key_filters;
	//! The scan columns that join pushes into, when it is the only one (checked
	//! in debug builds)
	vector<idx_t> join_key_columns;

	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;
	//! Total number of physical columns over all tables (the size of the fused
	//! column space)
	idx_t ColumnCount() const;
};

//! Estimated number of rows the scan produces (from per-attribute datom counts)
idx_t EstimateScanRows(const MnemonicScanBindData &bind_data);

//! The row-id column of table `index` in a fused scan
column_t FusedRowIdColumn(idx_t index);

//! The scan function for a MnemonicDB table, with bind data already filled in
TableFunction MnemonicScanFunction(ClientContext &context, MnemonicTableEntry &table,
                                   unique_ptr<FunctionData> &bind_data, optional_ptr<BoundAtClause> at_clause);
//! The scan function for a fused scan built by the optimizer
TableFunction MnemonicFusedScanFunction();

} // namespace duckdb
