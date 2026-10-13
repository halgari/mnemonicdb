#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

//! mnemonicdb_history(db, table_name :=, column_name :=, entity :=, since :=,
//! until :=): every assertion and retraction, with values as VARIANT
void RegisterHistoryFunction(ExtensionLoader &loader);

} // namespace duckdb
