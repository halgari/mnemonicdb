#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

class ClientContext;
class MnemonicTransaction;

//! Evaluate every assertion in sys.assertions against the pending state of a
//! committing transaction (its own writes included). Throws a
//! ConstraintException naming each failing assertion, with its first rows, if
//! any assertion returns rows or cannot run.
void EvaluateAssertions(ClientContext &context, MnemonicTransaction &transaction);

} // namespace duckdb
