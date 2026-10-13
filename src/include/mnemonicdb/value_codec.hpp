#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

#include <string>
#include <string_view>

namespace mnemonicdb {

using duckdb::idx_t;
using duckdb::LogicalType;
using duckdb::string;
using duckdb::Value;
using duckdb::Vector;

//! Encodes values of one attribute type into memcmp-ordered bytes and back.
//!
//! Generic values use DuckDB's sort-key encoding (ascending, nulls last) with
//! the top-level validity byte stripped, since stored values are never NULL.
//! Encodings are prefix-free, so a key may continue after V. REF values use the
//! 6-byte packed entity ID instead.
class ValueCodec {
public:
	explicit ValueCodec(LogicalType type, bool is_ref = false);

	//! Throws if values of `type` cannot be stored
	static void CheckSupported(const LogicalType &type);

	const LogicalType &GetType() const {
		return type;
	}
	bool IsRef() const {
		return is_ref;
	}

	//! Encode rows [0, count) of `input`. NULL rows produce an empty string and
	//! must not be stored.
	void EncodeVector(const Vector &input, idx_t count, duckdb::vector<string> &result) const;
	string EncodeValue(const Value &value) const;

	//! Decode one encoded value into row `row` of `result`; throws unless exactly
	//! all of `encoded` is consumed
	void Decode(std::string_view encoded, Vector &result, idx_t row) const;
	Value DecodeValue(std::string_view encoded) const;

	//! Number of bytes the encoded value at the start of `data` occupies; throws
	//! if it is malformed
	idx_t EncodedLength(std::string_view data) const;
	//! Bounds-checked check that `encoded` is exactly one well-formed value.
	//! Decode trusts its input (values read back from the store were produced by
	//! this codec), so validate anything that comes from elsewhere first. Throws
	//! a descriptive error for malformed input; returns false if there are
	//! trailing bytes.
	bool Validate(std::string_view encoded) const;

private:
	idx_t DecodeInternal(std::string_view encoded, Vector &result, idx_t row) const;

private:
	LogicalType type;
	bool is_ref;
};

} // namespace mnemonicdb
