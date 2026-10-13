#include "mnemonicdb/codec_functions.hpp"
#include "mnemonicdb/entity_id.hpp"
#include "mnemonicdb/keys.hpp"
#include "mnemonicdb/schema.hpp"
#include "mnemonicdb/tempid.hpp"
#include "mnemonicdb/value_codec.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

using mnemonicdb::ValueCodec;

struct CodecBindData : public FunctionData {
	explicit CodecBindData(LogicalType type) : codec(std::move(type)) {
	}
	ValueCodec codec;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<CodecBindData>(codec.GetType());
	}
	bool Equals(const FunctionData &other) const override {
		return codec.GetType() == other.Cast<CodecBindData>().codec.GetType();
	}
};

static const ValueCodec &GetCodec(ExpressionState &state) {
	return state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<CodecBindData>().codec;
}

//===--------------------------------------------------------------------===//
// mnemonicdb_encode(value) -> BLOB
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> EncodeBind(BindScalarFunctionInput &input) {
	auto &type = input.GetArguments()[0]->GetReturnType();
	if (type.id() == LogicalTypeId::UNKNOWN) {
		throw ParameterNotResolvedException();
	}
	return make_uniq<CodecBindData>(type);
}

static void EncodeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &codec = GetCodec(state);
	auto count = args.size();
	vector<string> encoded;
	codec.EncodeVector(args.data[0], count, encoded);
	auto input_validity = args.data[0].Validity();
	auto writer = FlatVector::Writer<string_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		if (!input_validity.IsValid(i)) {
			writer.WriteNull();
		} else {
			writer.WriteValue(string_t(encoded[i].data(), UnsafeNumericCast<uint32_t>(encoded[i].size())));
		}
	}
}

//===--------------------------------------------------------------------===//
// mnemonicdb_decode(blob, 'TYPE') -> TYPE
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> DecodeBind(BindScalarFunctionInput &input) {
	auto type_name = input.GetNonNullConstant(1).ToString();
	auto type = TransformStringToLogicalType(type_name, input.GetClientContext());
	input.GetBoundFunction().SetReturnType(type);
	return make_uniq<CodecBindData>(type);
}

static void DecodeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &codec = GetCodec(state);
	auto count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto blobs = args.data[0].Values<string_t>();
	for (idx_t i = 0; i < count; i++) {
		auto entry = blobs[i];
		if (!entry.IsValid()) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		auto blob = entry.GetValue();
		std::string_view encoded(blob.GetData(), blob.GetSize());
		auto length = codec.EncodedLength(encoded);
		if (length != encoded.size()) {
			throw InvalidInputException("mnemonicdb_decode: encoded value has %d trailing bytes",
			                            encoded.size() - length);
		}
		codec.Decode(encoded, result, i);
	}
	FlatVector::SetSize(result, count);
}

//===--------------------------------------------------------------------===//
// Entity IDs
//===--------------------------------------------------------------------===//
static void MakeIdFunction(DataChunk &args, ExpressionState &, Vector &result) {
	BinaryExecutor::Execute<uint8_t, uint64_t, uint64_t>(args.data[0], args.data[1], result, args.size(),
	                                                     [](uint8_t part, uint64_t local) {
		                                                     if (local > mnemonicdb::LOCAL_MASK) {
			                                                     throw InvalidInputException(
			                                                         "entity local number %d exceeds 40 bits", local);
		                                                     }
		                                                     return mnemonicdb::MakeEntityId(part, local);
	                                                     });
}

static void CheckWellFormed(uint64_t id) {
	if (!mnemonicdb::IsWellFormed(id)) {
		throw InvalidInputException("%d is not a valid entity ID: bits 40-55 must be zero", id);
	}
}

static void IdPartitionFunction(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<uint64_t, uint8_t>(args.data[0], result, args.size(), [](uint64_t id) {
		CheckWellFormed(id);
		return mnemonicdb::PartitionOf(id);
	});
}

static void IdLocalFunction(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<uint64_t, uint64_t>(args.data[0], result, args.size(), [](uint64_t id) {
		CheckWellFormed(id);
		return mnemonicdb::LocalOf(id);
	});
}

//===--------------------------------------------------------------------===//
// Index keys
//===--------------------------------------------------------------------===//
static mnemonicdb::IndexKind ParseIndexKind(const string &name) {
	auto lower = StringUtil::Lower(name);
	if (lower == "eavt") {
		return mnemonicdb::IndexKind::EAVT;
	} else if (lower == "aevt") {
		return mnemonicdb::IndexKind::AEVT;
	} else if (lower == "avet") {
		return mnemonicdb::IndexKind::AVET;
	} else if (lower == "vaet") {
		return mnemonicdb::IndexKind::VAET;
	} else if (lower == "txlog") {
		return mnemonicdb::IndexKind::TXLOG;
	}
	throw InvalidInputException("unknown index \"%s\": expected eavt, aevt, avet, vaet or txlog", name);
}

static string ToStdString(const string_t &str) {
	return string(str.GetData(), str.GetSize());
}

static void EncodeKeyFunction(DataChunk &args, ExpressionState &, Vector &result) {
	auto count = args.size();
	auto index = args.data[0].Values<string_t>();
	auto e = args.data[1].Values<uint64_t>();
	auto a = args.data[2].Values<uint16_t>();
	auto v = args.data[3].Values<string_t>();
	auto t = args.data[4].Values<uint64_t>();
	auto writer = FlatVector::Writer<string_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		if (!index[i].IsValid() || !e[i].IsValid() || !a[i].IsValid() || !v[i].IsValid() || !t[i].IsValid()) {
			writer.WriteNull();
			continue;
		}
		mnemonicdb::Datom datom;
		datom.e = e[i].GetValue();
		datom.a = a[i].GetValue();
		datom.v = ToStdString(v[i].GetValue());
		datom.t = t[i].GetValue();
		string key;
		try {
			key = mnemonicdb::EncodeKey(ParseIndexKind(ToStdString(index[i].GetValue())), datom);
		} catch (Exception &) {
			throw;
		} catch (std::exception &ex) {
			throw InvalidInputException("mnemonicdb_encode_key: %s", ex.what());
		}
		writer.WriteValue(string_t(key.data(), UnsafeNumericCast<uint32_t>(key.size())));
	}
}

static LogicalType DatomType() {
	return LogicalType::STRUCT({{"e", LogicalType::UBIGINT},
	                            {"a", LogicalType::USMALLINT},
	                            {"v", LogicalType::BLOB},
	                            {"t", LogicalType::UBIGINT}});
}

static void DecodeKeyFunction(DataChunk &args, ExpressionState &, Vector &result) {
	auto count = args.size();
	auto index = args.data[0].Values<string_t>();
	auto keys = args.data[1].Values<string_t>();
	for (idx_t i = 0; i < count; i++) {
		if (!index[i].IsValid() || !keys[i].IsValid()) {
			result.SetValue(i, Value(DatomType()));
			continue;
		}
		auto key = keys[i].GetValue();
		mnemonicdb::Datom datom;
		try {
			datom = mnemonicdb::DecodeKey(ParseIndexKind(ToStdString(index[i].GetValue())),
			                              std::string_view(key.GetData(), key.GetSize()));
		} catch (Exception &) {
			throw;
		} catch (std::exception &ex) {
			throw InvalidInputException("mnemonicdb_decode_key: %s", ex.what());
		}
		child_list_t<Value> fields;
		fields.emplace_back("e", Value::UBIGINT(datom.e));
		fields.emplace_back("a", Value::USMALLINT(datom.a));
		fields.emplace_back("v", Value::BLOB(const_data_ptr_cast(datom.v.data()), datom.v.size()));
		fields.emplace_back("t", Value::UBIGINT(datom.t));
		result.SetValue(i, Value::STRUCT(std::move(fields)));
	}
}

//===--------------------------------------------------------------------===//
// tempid(label [, partition]) -> REF
//===--------------------------------------------------------------------===//
static void TempidFunction(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<string_t, uint64_t>(args.data[0], result, args.size(), [](string_t label) {
		return mnemonicdb::MakeTempid(label.GetString(), 0);
	});
}

static void TempidPartitionFunction(DataChunk &args, ExpressionState &, Vector &result) {
	BinaryExecutor::Execute<string_t, uint8_t, uint64_t>(
	    args.data[0], args.data[1], result, args.size(), [](string_t label, uint8_t part) {
		    if (part < mnemonicdb::partition::FIRST_USER || part > mnemonicdb::partition::LAST_USER) {
			    throw InvalidInputException("tempid: partition must be between %d and %d, got %d",
			                                mnemonicdb::partition::FIRST_USER, mnemonicdb::partition::LAST_USER, part);
		    }
		    return mnemonicdb::MakeTempid(label.GetString(), part);
	    });
}

void RegisterCodecFunctions(ExtensionLoader &loader) {
	loader.RegisterType("REF", mnemonicdb::RefType());
	// REF and UBIGINT share a representation; both directions are cheap implicit casts
	loader.RegisterCastFunction(mnemonicdb::RefType(), LogicalType::UBIGINT, BoundCastInfo(DefaultCasts::ReinterpretCast),
	                            1);
	loader.RegisterCastFunction(LogicalType::UBIGINT, mnemonicdb::RefType(), BoundCastInfo(DefaultCasts::ReinterpretCast),
	                            1);
	ScalarFunctionSet tempid("tempid");
	ScalarFunction tempid_label({LogicalType::VARCHAR}, mnemonicdb::RefType(), TempidFunction);
	ScalarFunction tempid_partition({LogicalType::VARCHAR, LogicalType::UTINYINT}, mnemonicdb::RefType(),
	                                TempidPartitionFunction);
	tempid_partition.SetFallible();
	tempid.AddFunction(tempid_label);
	tempid.AddFunction(tempid_partition);
	loader.RegisterFunction(tempid);

	vector<ScalarFunction> functions;
	functions.emplace_back("mnemonicdb_encode", vector<LogicalType> {LogicalType::ANY}, LogicalType::BLOB,
	                       EncodeFunction, EncodeBind);
	functions.emplace_back("mnemonicdb_decode", vector<LogicalType> {LogicalType::BLOB, LogicalType::VARCHAR},
	                       LogicalType::ANY, DecodeFunction, DecodeBind);
	functions.emplace_back("mnemonicdb_make_id", vector<LogicalType> {LogicalType::UTINYINT, LogicalType::UBIGINT},
	                       LogicalType::UBIGINT, MakeIdFunction);
	functions.emplace_back("mnemonicdb_id_partition", vector<LogicalType> {LogicalType::UBIGINT},
	                       LogicalType::UTINYINT, IdPartitionFunction);
	functions.emplace_back("mnemonicdb_id_local", vector<LogicalType> {LogicalType::UBIGINT}, LogicalType::UBIGINT,
	                       IdLocalFunction);
	functions.emplace_back("mnemonicdb_encode_key",
	                       vector<LogicalType> {LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::USMALLINT,
	                                            LogicalType::BLOB, LogicalType::UBIGINT},
	                       LogicalType::BLOB, EncodeKeyFunction);
	functions.emplace_back("mnemonicdb_decode_key", vector<LogicalType> {LogicalType::VARCHAR, LogicalType::BLOB},
	                       DatomType(), DecodeKeyFunction);
	for (auto &function : functions) {
		function.SetFallible();
		loader.RegisterFunction(function);
	}
}

} // namespace duckdb
