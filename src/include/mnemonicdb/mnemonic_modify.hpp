#pragma once

#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {

class LogicalDelete;
class LogicalUpdate;
class PhysicalPlanGenerator;

PhysicalOperator &PlanMnemonicUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                     PhysicalOperator &plan);
PhysicalOperator &PlanMnemonicDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                     PhysicalOperator &plan);

} // namespace duckdb
