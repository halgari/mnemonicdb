#include "mnemonicdb/keys.hpp"

#include <stdexcept>

namespace mnemonicdb {

static constexpr size_t EAVT_FIXED = PACKED_ID_SIZE + PACKED_ATTR_SIZE + PACKED_TX_SIZE;

IndexKind IndexKindOf(ColumnFamily cf) {
	switch (cf) {
	case ColumnFamily::EAVT:
	case ColumnFamily::EAVT_HIST:
		return IndexKind::EAVT;
	case ColumnFamily::AEVT:
	case ColumnFamily::AEVT_HIST:
		return IndexKind::AEVT;
	case ColumnFamily::AVET:
	case ColumnFamily::AVET_HIST:
		return IndexKind::AVET;
	case ColumnFamily::VAET:
	case ColumnFamily::VAET_HIST:
		return IndexKind::VAET;
	case ColumnFamily::TXLOG:
		return IndexKind::TXLOG;
	default:
		throw std::invalid_argument("column family is not an index");
	}
}

std::string EncodeKey(IndexKind index, const Datom &d) {
	std::string key;
	key.reserve(EAVT_FIXED + d.v.size());
	switch (index) {
	case IndexKind::EAVT:
		PutEntityId(key, d.e);
		PutAttrId(key, d.a);
		key += d.v;
		PutTxId(key, d.t);
		break;
	case IndexKind::AEVT:
		PutAttrId(key, d.a);
		PutEntityId(key, d.e);
		key += d.v;
		PutTxId(key, d.t);
		break;
	case IndexKind::AVET:
		PutAttrId(key, d.a);
		key += d.v;
		PutEntityId(key, d.e);
		PutTxId(key, d.t);
		break;
	case IndexKind::VAET:
		if (d.v.size() != PACKED_ID_SIZE) {
			throw std::invalid_argument("VAET value must be a packed entity ID");
		}
		key += d.v;
		PutAttrId(key, d.a);
		PutEntityId(key, d.e);
		PutTxId(key, d.t);
		break;
	case IndexKind::TXLOG:
		PutTxId(key, d.t);
		PutEntityId(key, d.e);
		PutAttrId(key, d.a);
		key += d.v;
		break;
	}
	return key;
}

Datom DecodeKey(IndexKind index, std::string_view key) {
	if (key.size() < EAVT_FIXED || (index == IndexKind::VAET && key.size() != EAVT_FIXED + PACKED_ID_SIZE)) {
		throw std::invalid_argument("malformed index key");
	}
	const char *p = key.data();
	const size_t v_size = key.size() - EAVT_FIXED;
	Datom d;
	switch (index) {
	case IndexKind::EAVT:
		d.e = GetEntityId(p);
		d.a = GetAttrId(p + PACKED_ID_SIZE);
		d.v.assign(p + PACKED_ID_SIZE + PACKED_ATTR_SIZE, v_size);
		d.t = GetTxId(p + key.size() - PACKED_TX_SIZE);
		break;
	case IndexKind::AEVT:
		d.a = GetAttrId(p);
		d.e = GetEntityId(p + PACKED_ATTR_SIZE);
		d.v.assign(p + PACKED_ATTR_SIZE + PACKED_ID_SIZE, v_size);
		d.t = GetTxId(p + key.size() - PACKED_TX_SIZE);
		break;
	case IndexKind::AVET:
		d.a = GetAttrId(p);
		d.v.assign(p + PACKED_ATTR_SIZE, v_size);
		d.e = GetEntityId(p + PACKED_ATTR_SIZE + v_size);
		d.t = GetTxId(p + key.size() - PACKED_TX_SIZE);
		break;
	case IndexKind::VAET:
		d.v.assign(p, PACKED_ID_SIZE);
		d.a = GetAttrId(p + PACKED_ID_SIZE);
		d.e = GetEntityId(p + PACKED_ID_SIZE + PACKED_ATTR_SIZE);
		d.t = GetTxId(p + key.size() - PACKED_TX_SIZE);
		break;
	case IndexKind::TXLOG:
		d.t = GetTxId(p);
		d.e = GetEntityId(p + PACKED_TX_SIZE);
		d.a = GetAttrId(p + PACKED_TX_SIZE + PACKED_ID_SIZE);
		d.v.assign(p + EAVT_FIXED, v_size);
		break;
	}
	return d;
}

std::string EavtPrefix(EntityId e) {
	std::string key;
	PutEntityId(key, e);
	return key;
}

std::string EavtPrefix(EntityId e, AttrId a) {
	auto key = EavtPrefix(e);
	PutAttrId(key, a);
	return key;
}

std::string AevtPrefix(AttrId a) {
	std::string key;
	PutAttrId(key, a);
	return key;
}

std::string AevtPrefix(AttrId a, EntityId e) {
	auto key = AevtPrefix(a);
	PutEntityId(key, e);
	return key;
}

std::string AvetPrefix(AttrId a) {
	return AevtPrefix(a);
}

std::string AvetPrefix(AttrId a, std::string_view v) {
	auto key = AvetPrefix(a);
	key += v;
	return key;
}

std::string VaetPrefix(EntityId v) {
	return EavtPrefix(v);
}

std::string VaetPrefix(EntityId v, AttrId a) {
	return EavtPrefix(v, a);
}

std::string TxlogPrefix(TxId t) {
	std::string key;
	PutTxId(key, t);
	return key;
}

std::string EncodeHistoryValue(TxId retracted_by) {
	std::string value;
	PutTxId(value, retracted_by);
	return value;
}

TxId DecodeHistoryValue(std::string_view value) {
	if (value.size() != PACKED_TX_SIZE) {
		throw std::invalid_argument("malformed history value");
	}
	return GetTxId(value.data()); // NOLINT: size checked above
}

std::string EncodeTxOp(TxOp op) {
	return std::string(1, static_cast<char>(op));
}

TxOp DecodeTxOp(std::string_view value) {
	if (value.size() != 1 || static_cast<uint8_t>(value[0]) > 1) {
		throw std::invalid_argument("malformed txlog value");
	}
	return static_cast<TxOp>(value[0]);
}

} // namespace mnemonicdb
