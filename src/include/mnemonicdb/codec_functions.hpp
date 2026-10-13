#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

//! Register the mnemonicdb_* functions exposing the ID, value and key codecs
void RegisterCodecFunctions(ExtensionLoader &loader);

} // namespace duckdb
