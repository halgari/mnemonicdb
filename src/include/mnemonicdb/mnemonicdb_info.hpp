#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

//! mnemonicdb_info(catalog): format version, last tx and column families of an
//! attached database
TableFunction MnemonicInfoTableFunction();
//! mnemonicdb_compact(catalog): full compaction of every column family; returns
//! the size before and after
TableFunction MnemonicCompactTableFunction();

} // namespace duckdb
