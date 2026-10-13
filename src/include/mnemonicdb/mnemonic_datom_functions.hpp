#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

//! mnemonicdb_add / mnemonicdb_retract: assert or retract one datom without reading the current value set
void RegisterDatomFunctions(ExtensionLoader &loader);

} // namespace duckdb
