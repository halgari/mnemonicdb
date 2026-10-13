#include "mnemonicdb/value_codec.hpp"
#include "mnemonicdb/entity_id.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "utf8proc_wrapper.hpp"

namespace mnemonicdb {

using duckdb::InvalidInputException;
using duckdb::LogicalTypeId;
using duckdb::NotImplementedException;
using duckdb::OrderByNullType;
using duckdb::OrderModifiers;
using duckdb::OrderType;
using duckdb::PhysicalType;
using duckdb::string_t;
using duckdb::UnifiedVectorFormat;

static const OrderModifiers MODIFIERS(OrderType::ASCENDING, OrderByNullType::NULLS_LAST);
//! With NULLS LAST at the top level, a non-NULL value's sort key starts with this byte
static constexpr char VALID_BYTE = 1;

ValueCodec::ValueCodec(LogicalType type_p, bool is_ref_p) : type(std::move(type_p)), is_ref(is_ref_p) {
	if (is_ref && type.id() != LogicalTypeId::UBIGINT) {
		throw duckdb::InternalException("REF values must be UBIGINT");
	}
	CheckSupported(type);
}

void ValueCodec::CheckSupported(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::BIGNUM:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::BIT:
	case LogicalTypeId::UUID:
	case LogicalTypeId::ENUM:
		return;
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::UNION:
		if (duckdb::StructType::GetChildTypes(type).empty()) {
			throw NotImplementedException("MnemonicDB cannot store values of an empty STRUCT type");
		}
		for (auto &child : duckdb::StructType::GetChildTypes(type)) {
			CheckSupported(child.second);
		}
		return;
	case LogicalTypeId::LIST:
	case LogicalTypeId::MAP:
		CheckSupported(duckdb::ListType::GetChildType(type));
		return;
	case LogicalTypeId::ARRAY:
		CheckSupported(duckdb::ArrayType::GetChildType(type));
		return;
	default:
		throw NotImplementedException("MnemonicDB cannot store values of type %s", type.ToString());
	}
}

//===--------------------------------------------------------------------===//
// Validation
//===--------------------------------------------------------------------===//
//! Bounds-checked walk over one encoded value, mirroring DuckDB's sort-key layout (ascending; NULLS LAST inside
//! nested values). Returns the offset just past the value. `data` is the full key including the validity byte.
static idx_t ValidateValue(const uint8_t *data, idx_t size, idx_t pos, const LogicalType &type);

static void Require(bool condition, const char *what) {
	if (!condition) {
		throw InvalidInputException("MnemonicDB: malformed encoded value (%s)", what);
	}
}

static constexpr uint8_t SORT_VALID = 1;
static constexpr uint8_t SORT_NULL = 2;
static constexpr uint8_t SORT_DELIMITER = 0;

//! A validity byte followed, unless NULL, by the payload. Struct children and array elements always follow.
static idx_t ValidateNested(const uint8_t *data, idx_t size, idx_t pos, const LogicalType &type) {
	Require(pos < size, "truncated validity byte");
	auto validity = data[pos++];
	Require(validity == SORT_VALID || validity == SORT_NULL, "bad validity byte");
	auto id = type.id();
	if (validity == SORT_NULL && id != LogicalTypeId::STRUCT && id != LogicalTypeId::UNION &&
	    id != LogicalTypeId::ARRAY) {
		return pos;
	}
	return ValidateValue(data, size, pos, type);
}

static idx_t ValidateValue(const uint8_t *data, idx_t size, idx_t pos, const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::UNION:
		for (auto &child : duckdb::StructType::GetChildTypes(type)) {
			pos = ValidateNested(data, size, pos, child.second);
		}
		return pos;
	case LogicalTypeId::LIST:
	case LogicalTypeId::MAP: {
		auto &child = duckdb::ListType::GetChildType(type);
		while (true) {
			Require(pos < size, "unterminated list");
			if (data[pos] == SORT_DELIMITER) {
				return pos + 1;
			}
			pos = ValidateNested(data, size, pos, child);
		}
	}
	case LogicalTypeId::ARRAY: {
		auto &child = duckdb::ArrayType::GetChildType(type);
		for (idx_t i = 0; i < duckdb::ArrayType::GetSize(type); i++) {
			pos = ValidateNested(data, size, pos, child);
		}
		Require(pos < size && data[pos] == SORT_DELIMITER, "unterminated array");
		return pos + 1;
	}
	case LogicalTypeId::VARCHAR: {
		// Bytes are stored incremented by one, so none of them is the delimiter
		auto start = pos;
		while (pos < size && data[pos] != SORT_DELIMITER) {
			pos++;
		}
		Require(pos < size, "unterminated string");
		string decoded;
		decoded.reserve(pos - start);
		for (idx_t i = start; i < pos; i++) {
			decoded.push_back(static_cast<char>(data[i] - 1));
		}
		Require(duckdb::Utf8Proc::Analyze(decoded.data(), decoded.size()) != duckdb::UnicodeType::INVALID,
		        "invalid UTF-8");
		return pos + 1;
	}
	case LogicalTypeId::BLOB:
	case LogicalTypeId::BIT:
	case LogicalTypeId::BIGNUM:
		// \x00 and \x01 are escaped with \x01
		while (true) {
			Require(pos < size, "unterminated blob");
			auto byte = data[pos++];
			if (byte == SORT_DELIMITER) {
				return pos;
			}
			if (byte == 1) {
				Require(pos < size && data[pos] <= 1, "bad escape");
				pos++;
			}
		}
	default:
		break;
	}
	auto width = duckdb::GetTypeIdSize(type.InternalType());
	Require(width > 0 && pos + width <= size, "truncated fixed-width value");
	if (type.id() == LogicalTypeId::BOOLEAN) {
		Require(data[pos] <= 1, "non-canonical boolean");
	} else if (type.id() == LogicalTypeId::ENUM) {
		uint64_t index = 0;
		for (idx_t i = 0; i < width; i++) {
			index = (index << 8) | data[pos + i];
		}
		Require(index < duckdb::EnumType::GetSize(type), "ENUM index out of range");
	}
	return pos + width;
}

bool ValueCodec::Validate(std::string_view encoded) const {
	if (is_ref) {
		return encoded.size() == PACKED_ID_SIZE && IsWellFormed(GetEntityId(encoded.data()));
	}
	auto data = reinterpret_cast<const uint8_t *>(encoded.data());
	return ValidateValue(data, encoded.size(), 0, type) == encoded.size();
}

void ValueCodec::EncodeVector(const Vector &input, idx_t count, duckdb::vector<string> &result) const {
	result.clear();
	result.resize(count);
	UnifiedVectorFormat format;
	input.ToUnifiedFormat(format);

	if (is_ref) {
		auto ids = UnifiedVectorFormat::GetData<uint64_t>(format);
		for (idx_t i = 0; i < count; i++) {
			auto idx = format.sel->get_index(i);
			if (format.validity.RowIsValid(idx)) {
				PutEntityId(result[i], ids[idx]);
			}
		}
		return;
	}

	Vector keys(LogicalType::BLOB, count);
	duckdb::CreateSortKeyHelpers::CreateSortKey(input, count, MODIFIERS, keys);
	auto key_data = duckdb::FlatVector::GetData<string_t>(keys);
	for (idx_t i = 0; i < count; i++) {
		if (!format.validity.RowIsValid(format.sel->get_index(i))) {
			continue;
		}
		auto &key = key_data[i];
		D_ASSERT(key.GetSize() > 1 && key.GetData()[0] == VALID_BYTE);
		result[i].assign(key.GetData() + 1, key.GetSize() - 1);
	}
}

string ValueCodec::EncodeValue(const Value &value) const {
	if (value.IsNull()) {
		throw InvalidInputException("MnemonicDB cannot encode NULL");
	}
	Vector input(value.DefaultCastAs(type), duckdb::count_t(1));
	duckdb::vector<string> result;
	EncodeVector(input, 1, result);
	return std::move(result[0]);
}

idx_t ValueCodec::DecodeInternal(std::string_view encoded, Vector &result, idx_t row) const {
	if (is_ref) {
		if (encoded.size() < PACKED_ID_SIZE) {
			throw InvalidInputException("MnemonicDB: truncated REF value");
		}
		duckdb::FlatVector::GetDataMutable<uint64_t>(result)[row] = GetEntityId(encoded.data());
		return PACKED_ID_SIZE;
	}
	// Restore the validity byte stripped on encode, reusing a per-thread buffer
	thread_local string key;
	key.clear();
	key.push_back(VALID_BYTE);
	key.append(encoded);
	auto consumed =
	    duckdb::CreateSortKeyHelpers::DecodeSortKey(string_t(key.data(), key.size()), result, row, MODIFIERS);
	if (consumed < 1 || consumed > key.size()) {
		throw InvalidInputException("MnemonicDB: malformed encoded value");
	}
	return consumed - 1;
}

void ValueCodec::Decode(std::string_view encoded, Vector &result, idx_t row) const {
	auto consumed = DecodeInternal(encoded, result, row);
	if (consumed != encoded.size()) {
		throw InvalidInputException("MnemonicDB: encoded value has %d trailing bytes", encoded.size() - consumed);
	}
}

Value ValueCodec::DecodeValue(std::string_view encoded) const {
	Vector result(type, 1);
	Decode(encoded, result, 0);
	duckdb::FlatVector::SetSize(result, 1);
	return result.GetValue(0);
}

idx_t ValueCodec::EncodedLength(std::string_view data) const {
	if (is_ref) {
		Require(data.size() >= PACKED_ID_SIZE, "truncated REF value");
		return PACKED_ID_SIZE;
	}
	return ValidateValue(reinterpret_cast<const uint8_t *>(data.data()), data.size(), 0, type);
}

} // namespace mnemonicdb
