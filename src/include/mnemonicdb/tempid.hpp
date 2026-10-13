#pragma once

#include "mnemonicdb/entity_id.hpp"

#include <string>

namespace mnemonicdb {

//! Tempid placeholders live in partition 255. The 16 middle bits, unused by real IDs, hold the partition the real
//! ID should be allocated in (0 = default), and the low 40 bits a hash of the label. Placeholders are therefore
//! deterministic: the same label gives the same placeholder anywhere in a transaction.
EntityId MakeTempid(const std::string &label, uint8_t target_partition);

inline bool IsTempid(EntityId id) {
	return PartitionOf(id) == partition::TEMP;
}

//! The partition a tempid asks to be allocated in
inline uint8_t TempidTargetPartition(EntityId id) {
	auto target = static_cast<uint8_t>((id >> LOCAL_BITS) & 0xFF);
	return target == 0 ? partition::DB : target;
}

} // namespace mnemonicdb
