#include "mnemonicdb/tempid.hpp"

#include "duckdb/common/types/hash.hpp"

namespace mnemonicdb {

EntityId MakeTempid(const std::string &label, uint8_t target_partition) {
	auto hash = duckdb::Hash(label.data(), label.size());
	return (static_cast<uint64_t>(partition::TEMP) << PARTITION_SHIFT) |
	       (static_cast<uint64_t>(target_partition) << LOCAL_BITS) | (hash & LOCAL_MASK);
}

} // namespace mnemonicdb
