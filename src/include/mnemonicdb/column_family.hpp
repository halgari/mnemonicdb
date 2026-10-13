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

//! Indexes whose keys start with two fixed-width components (E6 A2, A2 E6, V6 A2) have an 8-byte prefix extractor
//! with prefix Bloom filters
static constexpr unsigned BLOOM_PREFIX_LENGTH = 8;
bool HasPrefixBloom(ColumnFamily cf);

} // namespace mnemonicdb
