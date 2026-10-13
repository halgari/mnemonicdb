#pragma once

#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {

class LogicalInsert;
class MnemonicTableEntry;
class PhysicalPlanGenerator;

PhysicalOperator &PlanMnemonicInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                     optional_ptr<PhysicalOperator> plan);

} // namespace duckdb
