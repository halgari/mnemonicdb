#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

class MnemonicTableEntry;
class BoundAtClause;

//! The scan function for a MnemonicDB table, with bind data already filled in
TableFunction MnemonicScanFunction(ClientContext &context, MnemonicTableEntry &table,
                                   unique_ptr<FunctionData> &bind_data, optional_ptr<BoundAtClause> at_clause);

} // namespace duckdb
