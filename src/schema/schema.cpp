#include "mnemonicdb/schema.hpp"
#include "mnemonicdb/tx_writer.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace mnemonicdb {

using duckdb::InvalidInputException;
using duckdb::LogicalTypeId;

static constexpr const char *REF_ALIAS = "REF";

LogicalType RefType() {
	return LogicalType(LogicalTypeId::UBIGINT).WithAlias(REF_ALIAS);
}

bool IsRefType(const LogicalType &type) {
	return type.id() == LogicalTypeId::UBIGINT && type.HasAlias() && type.GetAlias() == REF_ALIAS;
}

const char *UniquenessName(Uniqueness unique) {
	switch (unique) {
	case Uniqueness::VALUE:
		return "value";
	case Uniqueness::IDENTITY:
		return "identity";
	default:
		return "none";
	}
}

static Uniqueness ParseUniqueness(const string &name) {
	if (name == "value") {
		return Uniqueness::VALUE;
	} else if (name == "identity") {
		return Uniqueness::IDENTITY;
	} else if (name == "none") {
		return Uniqueness::NONE;
	}
	throw InvalidInputException("MnemonicDB: unknown uniqueness \"%s\"", name);
}

static LogicalType ValueTypeOf(const LogicalType &type) {
	return type.id() == LogicalTypeId::LIST ? duckdb::ListType::GetChildType(type) : type;
}

AttributeDef::AttributeDef(EntityId id_p, EntityId table_p, string name_p, LogicalType type_p, Uniqueness unique_p,
                           bool indexed_p, string index_name_p)
    : id(id_p), table(table_p), name(std::move(name_p)), type(std::move(type_p)), unique(unique_p), indexed(indexed_p),
      index_name(std::move(index_name_p)), many(type.id() == LogicalTypeId::LIST), value_type(ValueTypeOf(type)),
      is_ref(IsRefType(value_type)), codec(is_ref ? LogicalType(LogicalTypeId::UBIGINT) : value_type, is_ref) {
	if (PartitionOf(id) != partition::ATTR || LocalOf(id) > MAX_ATTR_LOCAL) {
		throw duckdb::InternalException("attribute ID %d is not in the attribute partition", id);
	}
	if (many && unique != Uniqueness::NONE) {
		throw InvalidInputException("MnemonicDB: cardinality-many column \"%s\" cannot be UNIQUE", name);
	}
}

//===--------------------------------------------------------------------===//
// Built-in definitions
//===--------------------------------------------------------------------===//
struct BuiltinAttribute {
	AttrId local;
	EntityId table;
	const char *name;
	LogicalType type;
	Uniqueness unique;
	bool indexed;
};

static vector<BuiltinAttribute> BuiltinAttributes() {
	auto varchar = LogicalType(LogicalTypeId::VARCHAR);
	return {
	    {sys::TABLES_SCHEMA, sys::TABLES, "schema", varchar, Uniqueness::NONE, false},
	    {sys::TABLES_NAME, sys::TABLES, "name", varchar, Uniqueness::NONE, true},
	    {sys::TABLES_DOC, sys::TABLES, "doc", varchar, Uniqueness::NONE, false},
	    {sys::ATTRIBUTES_TABLE, sys::ATTRIBUTES, "table_id", RefType(), Uniqueness::NONE, false},
	    {sys::ATTRIBUTES_NAME, sys::ATTRIBUTES, "name", varchar, Uniqueness::NONE, false},
	    {sys::ATTRIBUTES_TYPE, sys::ATTRIBUTES, "type", varchar, Uniqueness::NONE, false},
	    {sys::ATTRIBUTES_UNIQUE, sys::ATTRIBUTES, "uniqueness", varchar, Uniqueness::NONE, false},
	    {sys::ATTRIBUTES_INDEXED, sys::ATTRIBUTES, "indexed", LogicalType(LogicalTypeId::BOOLEAN), Uniqueness::NONE,
	     false},
	    {sys::ATTRIBUTES_DOC, sys::ATTRIBUTES, "doc", varchar, Uniqueness::NONE, false},
	    {sys::TX_INSTANT, sys::TX, "instant", LogicalType(LogicalTypeId::TIMESTAMP_TZ), Uniqueness::NONE, true},
	    {sys::ATTRIBUTES_INDEX, sys::ATTRIBUTES, "index", varchar, Uniqueness::NONE, false},
	    {sys::ASSERTIONS_NAME, sys::ASSERTIONS, "name", varchar, Uniqueness::IDENTITY, false},
	    {sys::ASSERTIONS_QUERY, sys::ASSERTIONS, "query", varchar, Uniqueness::NONE, false},
	    {sys::ASSERTIONS_DOC, sys::ASSERTIONS, "doc", varchar, Uniqueness::NONE, false},
	};
}

static vector<TableDef> BuiltinTables() {
	return {{sys::TABLES, "sys", "tables", "Table definitions", {}},
	        {sys::ATTRIBUTES, "sys", "attributes", "Attribute (column) definitions", {}},
	        {sys::TX, "sys", "tx", "Transactions", {}},
	        {sys::ASSERTIONS, "sys", "assertions", "Commit assertions: queries that must return no rows", {}}};
}

shared_ptr<Schema> Schema::Builtin() {
	auto schema = duckdb::make_shared_ptr<Schema>();
	for (auto &table : BuiltinTables()) {
		schema->AddTable(table);
	}
	for (auto &attr : BuiltinAttributes()) {
		schema->AddAttribute(
		    AttributeDef(AttrEntity(attr.local), attr.table, attr.name, attr.type, attr.unique, attr.indexed));
	}
	return schema;
}

//===--------------------------------------------------------------------===//
// Lookup
//===--------------------------------------------------------------------===//
void Schema::AddTable(TableDef table) {
	auto &by_name = tables_by_name[table.schema];
	if (by_name.find(table.name) != by_name.end()) {
		throw InvalidInputException("MnemonicDB: table \"%s.%s\" is defined twice", table.schema, table.name);
	}
	by_name[table.name] = table.id;
	auto id = table.id;
	tables.emplace(id, std::move(table));
}

void Schema::AddAttribute(AttributeDef attribute) {
	auto table = tables.find(attribute.table);
	if (table == tables.end()) {
		throw InvalidInputException("MnemonicDB: attribute \"%s\" refers to unknown table %d", attribute.name,
		                            attribute.table);
	}
	if (FindAttribute(table->second, attribute.name)) {
		throw InvalidInputException("MnemonicDB: table \"%s\" has two attributes named \"%s\"", table->second.name,
		                            attribute.name);
	}
	auto local = attribute.Local();
	table->second.attributes.push_back(local);
	attributes.emplace(local, std::move(attribute));
}

const TableDef *Schema::FindTable(const string &schema, const string &name) const {
	auto by_name = tables_by_name.find(schema);
	if (by_name == tables_by_name.end()) {
		return nullptr;
	}
	auto entry = by_name->second.find(name);
	return entry == by_name->second.end() ? nullptr : FindTable(entry->second);
}

const TableDef *Schema::FindTable(EntityId id) const {
	auto entry = tables.find(id);
	return entry == tables.end() ? nullptr : &entry->second;
}

const AttributeDef &Schema::GetAttribute(AttrId local) const {
	auto entry = attributes.find(local);
	if (entry == attributes.end()) {
		throw duckdb::InternalException("MnemonicDB: unknown attribute %d", local);
	}
	return entry->second;
}

const AttributeDef *Schema::FindAttributeById(AttrId local) const {
	auto entry = attributes.find(local);
	return entry == attributes.end() ? nullptr : &entry->second;
}

const AttributeDef *Schema::FindAttribute(const TableDef &table, const string &name) const {
	for (auto local : table.attributes) {
		auto &attr = GetAttribute(local);
		if (duckdb::StringUtil::CIEquals(attr.name, name)) {
			return &attr;
		}
	}
	return nullptr;
}

vector<const TableDef *> Schema::TablesInSchema(const string &schema) const {
	vector<const TableDef *> result;
	auto by_name = tables_by_name.find(schema);
	if (by_name != tables_by_name.end()) {
		for (auto &entry : by_name->second) {
			result.push_back(FindTable(entry.second));
		}
	}
	std::sort(result.begin(), result.end(), [](const TableDef *a, const TableDef *b) { return a->id < b->id; });
	return result;
}

shared_ptr<Schema> Schema::WithTable(TableDef table, vector<AttributeDef> new_attributes) const {
	auto result = duckdb::make_shared_ptr<Schema>(*this);
	table.attributes.clear();
	result->AddTable(std::move(table));
	for (auto &attr : new_attributes) {
		result->AddAttribute(std::move(attr));
	}
	return result;
}

shared_ptr<Schema> Schema::WithAttribute(AttributeDef attribute) const {
	auto result = duckdb::make_shared_ptr<Schema>(*this);
	auto local = attribute.Local();
	auto entry = result->attributes.find(local);
	if (entry == result->attributes.end()) {
		throw duckdb::InternalException("MnemonicDB: replacing unknown attribute %d", local);
	}
	entry->second = std::move(attribute);
	return result;
}

shared_ptr<Schema> Schema::WithNewAttribute(AttributeDef attribute) const {
	auto result = duckdb::make_shared_ptr<Schema>(*this);
	result->AddAttribute(std::move(attribute));
	return result;
}

shared_ptr<Schema> Schema::WithoutAttribute(AttrId local) const {
	auto result = duckdb::make_shared_ptr<Schema>(*this);
	auto entry = result->attributes.find(local);
	if (entry == result->attributes.end()) {
		throw duckdb::InternalException("MnemonicDB: removing unknown attribute %d", local);
	}
	auto &table = result->tables.at(entry->second.table);
	table.attributes.erase(std::remove(table.attributes.begin(), table.attributes.end(), local),
	                       table.attributes.end());
	result->attributes.erase(entry);
	return result;
}

shared_ptr<Schema> Schema::WithTableName(EntityId table_id, const string &name) const {
	auto result = duckdb::make_shared_ptr<Schema>(*this);
	auto &table = result->tables.at(table_id);
	auto &by_name = result->tables_by_name[table.schema];
	if (by_name.find(name) != by_name.end()) {
		throw InvalidInputException("MnemonicDB: table \"%s\" already exists", name);
	}
	by_name.erase(table.name);
	by_name[name] = table_id;
	table.name = name;
	return result;
}

const AttributeDef *Schema::FindIndex(const string &name) const {
	for (auto &entry : attributes) {
		if (!entry.second.index_name.empty() && duckdb::StringUtil::CIEquals(entry.second.index_name, name)) {
			return &entry.second;
		}
	}
	return nullptr;
}

vector<const AttributeDef *> Schema::AllAttributes() const {
	vector<const AttributeDef *> result;
	for (auto &entry : attributes) {
		result.push_back(&entry.second);
	}
	return result;
}

vector<const AttributeDef *> Schema::IndexedAttributes() const {
	vector<const AttributeDef *> result;
	for (auto &entry : attributes) {
		if (!entry.second.index_name.empty()) {
			result.push_back(&entry.second);
		}
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Loading and bootstrap
//===--------------------------------------------------------------------===//
//! The single current value of a cardinality-one attribute, if any
static duckdb::unique_ptr<Value> CurrentValue(ReadView &view, const Schema &schema, EntityId e, AttrId a) {
	auto datoms = view.ScanPrefix(ColumnFamily::EAVT, EavtPrefix(e, a));
	if (datoms.empty()) {
		return nullptr;
	}
	return duckdb::make_uniq<Value>(schema.GetAttribute(a).codec.DecodeValue(datoms[0].v));
}

static string RequiredString(ReadView &view, const Schema &schema, EntityId e, AttrId a, const char *what) {
	auto value = CurrentValue(view, schema, e, a);
	if (!value) {
		throw duckdb::IOException("MnemonicDB: entity %d has no %s", e, what);
	}
	return value->ToString();
}

shared_ptr<Schema> Schema::Load(ReadView &view, duckdb::ClientContext &context) {
	auto schema = Builtin();

	vector<TableDef> user_tables;
	for (auto &datom : view.ScanPrefix(ColumnFamily::AEVT, AevtPrefix(sys::TABLES_NAME))) {
		if (schema->FindTable(datom.e)) {
			continue;
		}
		TableDef table;
		table.id = datom.e;
		table.name = schema->GetAttribute(sys::TABLES_NAME).codec.DecodeValue(datom.v).ToString();
		table.schema = RequiredString(view, *schema, datom.e, sys::TABLES_SCHEMA, "table schema");
		auto doc = CurrentValue(view, *schema, datom.e, sys::TABLES_DOC);
		table.doc = doc ? doc->ToString() : string();
		user_tables.push_back(std::move(table));
	}
	for (auto &table : user_tables) {
		schema->AddTable(std::move(table));
	}

	vector<AttributeDef> user_attributes;
	auto &ref_codec = schema->GetAttribute(sys::ATTRIBUTES_TABLE).codec;
	for (auto &datom : view.ScanPrefix(ColumnFamily::AEVT, AevtPrefix(sys::ATTRIBUTES_TABLE))) {
		if (LocalOf(datom.e) < 64) {
			continue;
		}
		auto table = ref_codec.DecodeValue(datom.v).GetValue<uint64_t>();
		auto name = RequiredString(view, *schema, datom.e, sys::ATTRIBUTES_NAME, "attribute name");
		auto type_name = RequiredString(view, *schema, datom.e, sys::ATTRIBUTES_TYPE, "attribute type");
		auto unique = CurrentValue(view, *schema, datom.e, sys::ATTRIBUTES_UNIQUE);
		auto indexed = CurrentValue(view, *schema, datom.e, sys::ATTRIBUTES_INDEXED);
		auto index_name = CurrentValue(view, *schema, datom.e, sys::ATTRIBUTES_INDEX);
		user_attributes.emplace_back(datom.e, table, name, duckdb::TransformStringToLogicalType(type_name, context),
		                             unique ? ParseUniqueness(unique->ToString()) : Uniqueness::NONE,
		                             indexed && indexed->GetValue<bool>(),
		                             index_name ? index_name->ToString() : string());
	}
	std::sort(user_attributes.begin(), user_attributes.end(),
	          [](const AttributeDef &a, const AttributeDef &b) { return a.id < b.id; });
	for (auto &attr : user_attributes) {
		schema->AddAttribute(std::move(attr));
	}
	return schema;
}

static void AssertValue(TxWriter &writer, const Schema &schema, EntityId e, AttrId a, const Value &value) {
	auto &attr = schema.GetAttribute(a);
	writer.Assert(attr, e, attr.codec.EncodeValue(value));
}

void Schema::WriteBootstrap(TxWriter &writer) {
	auto schema = Builtin();
	for (auto &table : BuiltinTables()) {
		AssertValue(writer, *schema, table.id, sys::TABLES_SCHEMA, Value(table.schema));
		AssertValue(writer, *schema, table.id, sys::TABLES_NAME, Value(table.name));
		AssertValue(writer, *schema, table.id, sys::TABLES_DOC, Value(table.doc));
	}
	for (auto &attr : BuiltinAttributes()) {
		WriteAttribute(
		    writer, *schema,
		    AttributeDef(AttrEntity(attr.local), attr.table, attr.name, attr.type, attr.unique, attr.indexed));
	}
}

void Schema::WriteTable(TxWriter &writer, const Schema &schema, const TableDef &table) {
	AssertValue(writer, schema, table.id, sys::TABLES_SCHEMA, Value(table.schema));
	AssertValue(writer, schema, table.id, sys::TABLES_NAME, Value(table.name));
	if (!table.doc.empty()) {
		AssertValue(writer, schema, table.id, sys::TABLES_DOC, Value(table.doc));
	}
}

void Schema::WriteAttribute(TxWriter &writer, const Schema &schema, const AttributeDef &attr) {
	AssertValue(writer, schema, attr.id, sys::ATTRIBUTES_TABLE, Value::UBIGINT(attr.table));
	AssertValue(writer, schema, attr.id, sys::ATTRIBUTES_NAME, Value(attr.name));
	AssertValue(writer, schema, attr.id, sys::ATTRIBUTES_TYPE, Value(attr.type.ToString()));
	if (attr.unique != Uniqueness::NONE) {
		AssertValue(writer, schema, attr.id, sys::ATTRIBUTES_UNIQUE, Value(UniquenessName(attr.unique)));
	}
	if (attr.indexed) {
		AssertValue(writer, schema, attr.id, sys::ATTRIBUTES_INDEXED, Value::BOOLEAN(true));
	}
	if (!attr.index_name.empty()) {
		AssertValue(writer, schema, attr.id, sys::ATTRIBUTES_INDEX, Value(attr.index_name));
	}
}

void Schema::WriteTxEntity(TxWriter &writer, const Schema &schema, TxId tx, int64_t instant) {
	AssertValue(writer, schema, TxEntity(tx), sys::TX_INSTANT, Value::TIMESTAMPTZ(duckdb::timestamp_tz_t(instant)));
}

} // namespace mnemonicdb
