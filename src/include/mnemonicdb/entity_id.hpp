#pragma once

#include "mnemonicdb/encoding.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace mnemonicdb {

//! An entity ID: partition in the top byte, a 40-bit local number in the low
//! bits, the 16 bits between always zero
using EntityId = uint64_t;
//! The local number of an attribute entity (partition 0), as stored in the A
//! position of keys
using AttrId = uint16_t;
//! The local number of a transaction entity (partition 2), as stored in the T
//! position of keys
using TxId = uint64_t;

namespace partition {
static constexpr uint8_t ATTR = 0;
static constexpr uint8_t TABLE = 1;
static constexpr uint8_t TX = 2;
static constexpr uint8_t DB = 3;
static constexpr uint8_t FIRST_RESERVED = 4;
static constexpr uint8_t FIRST_USER = 16;
static constexpr uint8_t LAST_USER = 254;
//! Tempid placeholders; never persisted
static constexpr uint8_t TEMP = 255;
} // namespace partition

static constexpr int PARTITION_SHIFT = 56;
static constexpr int LOCAL_BITS = 40;
static constexpr uint64_t LOCAL_MASK = (uint64_t(1) << LOCAL_BITS) - 1;
static constexpr uint64_t MAX_ATTR_LOCAL = UINT16_MAX;
static constexpr uint64_t MAX_TABLE_LOCAL = UINT16_MAX;

//! Packed widths in keys
static constexpr int PACKED_ID_SIZE = 6;
static constexpr int PACKED_ATTR_SIZE = 2;
static constexpr int PACKED_TX_SIZE = 5;

inline uint8_t PartitionOf(EntityId id) {
	return static_cast<uint8_t>(id >> PARTITION_SHIFT);
}

inline uint64_t LocalOf(EntityId id) {
	return id & LOCAL_MASK;
}

//! Whether the middle 16 bits are zero, i.e. the ID has a 6-byte packed form
inline bool IsWellFormed(EntityId id) {
	return ((id >> LOCAL_BITS) & 0xFFFF) == 0;
}

inline EntityId MakeEntityId(uint8_t part, uint64_t local) {
	if (local > LOCAL_MASK) {
		throw std::out_of_range("entity local number exceeds 40 bits");
	}
	return (static_cast<uint64_t>(part) << PARTITION_SHIFT) | local;
}

inline EntityId AttrEntity(AttrId attr) {
	return MakeEntityId(partition::ATTR, attr);
}

inline EntityId TxEntity(TxId tx) {
	return MakeEntityId(partition::TX, tx);
}

//! Append the 6-byte packed form: partition byte, then the 40-bit local number
inline void PutEntityId(std::string &out, EntityId id) {
	if (!IsWellFormed(id)) {
		throw std::invalid_argument("entity ID has non-zero bits between partition and local number");
	}
	out.push_back(static_cast<char>(PartitionOf(id)));
	PutBigEndian(out, LocalOf(id), 5);
}

inline EntityId GetEntityId(const char *data) {
	return MakeEntityId(static_cast<uint8_t>(data[0]), GetBigEndian(data + 1, 5));
}

inline void PutAttrId(std::string &out, AttrId attr) {
	PutBigEndian(out, attr, PACKED_ATTR_SIZE);
}

inline AttrId GetAttrId(const char *data) {
	return static_cast<AttrId>(GetBigEndian(data, PACKED_ATTR_SIZE));
}

inline void PutTxId(std::string &out, TxId tx) {
	if (tx > LOCAL_MASK) {
		throw std::out_of_range("transaction number exceeds 40 bits");
	}
	PutBigEndian(out, tx, PACKED_TX_SIZE);
}

inline TxId GetTxId(const char *data) {
	return GetBigEndian(data, PACKED_TX_SIZE);
}

} // namespace mnemonicdb
