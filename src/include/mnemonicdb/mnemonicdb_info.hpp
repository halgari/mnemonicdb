#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

//! mnemonicdb_info(catalog): format version, last tx and column families of an attached database
TableFunction MnemonicInfoTableFunction();

} // namespace duckdb
