#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

//! Registers the optimizer pass that fuses scans of tables joined on their
//! entity id into one scan, and the `mnemonicdb_fuse_scans` setting that turns
//! it off
void RegisterScanFusion(ExtensionLoader &loader);

} // namespace duckdb
