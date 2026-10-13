#pragma once

#include "mnemonicdb/entity_id.hpp"
#include "mnemonicdb/column_family.hpp"

#include <string>
#include <string_view>

namespace mnemonicdb {

//! A datom as stored in an index key. `v` is the encoded value (see
//! ValueCodec), or for a REF attribute the 6-byte packed entity ID. `t` is the
//! asserting transaction.
struct Datom {
	EntityId e = 0;
	AttrId a = 0;
	std::string v;
	TxId t = 0;

	bool operator==(const Datom &other) const = default;
};

//! Operation stored as the value of a txlog entry
enum class TxOp : uint8_t { RETRACT = 0, ASSERT = 1 };

//! Every index key has at most one variable-length segment (V), so keys can be
//! split without decoding V:
//!   eavt: E6 A2 V T5    aevt: A2 E6 V T5    avet: A2 V E6 T5    vaet: V6 A2 E6
//!   T5    txlog: T5 E6 A2 V
//! The `_hist` variants share the key layout of their current index.
enum class IndexKind : uint8_t { EAVT, AEVT, AVET, VAET, TXLOG };

IndexKind IndexKindOf(ColumnFamily cf);

//! Encode a datom as a key for the given index
std::string EncodeKey(IndexKind index, const Datom &datom);
//! Decode a key of the given index; throws on a malformed key
Datom DecodeKey(IndexKind index, std::string_view key);

//! Prefixes for range scans
std::string EavtPrefix(EntityId e);
std::string EavtPrefix(EntityId e, AttrId a);
std::string AevtPrefix(AttrId a);
std::string AevtPrefix(AttrId a, EntityId e);
std::string AvetPrefix(AttrId a);
std::string AvetPrefix(AttrId a, std::string_view v);
std::string VaetPrefix(EntityId v);
std::string VaetPrefix(EntityId v, AttrId a);
std::string TxlogPrefix(TxId t);

//! History entries store the retracting transaction as their value
std::string EncodeHistoryValue(TxId retracted_by);
TxId DecodeHistoryValue(std::string_view value);

std::string EncodeTxOp(TxOp op);
TxOp DecodeTxOp(std::string_view value);

} // namespace mnemonicdb
