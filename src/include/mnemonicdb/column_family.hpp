#pragma once

#include <cstdint>

namespace mnemonicdb {

//! The RocksDB column families making up a database
enum class ColumnFamily : uint8_t {
	EAVT,
	EAVT_HIST,
	AEVT,
	AEVT_HIST,
	AVET,
	AVET_HIST,
	VAET,
	VAET_HIST,
	TXLOG,
	META,
	COUNT
};

const char *ColumnFamilyName(ColumnFamily cf);

} // namespace mnemonicdb
