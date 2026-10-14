#include "mnemonicdb/schema.hpp"
#include "mnemonicdb/tx_writer.hpp"

#include "mnemonicdb/store.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/type_expression.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/binder.hpp"

#include <algorithm>

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
    : id(id_p), table(table_p), name(std::move(name_p)), declared(std::move(type_p)),
      type(StripNamedTypeAliases(declared)), unique(unique_p), indexed(indexed_p), index_name(std::move(index_name_p)),
      many(type.id() == LogicalTypeId::LIST), value_type(ValueTypeOf(type)), is_ref(IsRefType(value_type)),
      codec(is_ref ? LogicalType(LogicalTypeId::UBIGINT) : value_type, is_ref) {
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
	    {sys::SCHEMAS_NAME, sys::SCHEMAS, "name", varchar, Uniqueness::IDENTITY, false},
	    {sys::SCHEMAS_DOC, sys::SCHEMAS, "doc", varchar, Uniqueness::NONE, false},
	    {sys::TYPES_SCHEMA, sys::TYPES, "schema", varchar, Uniqueness::NONE, false},
	    {sys::TYPES_NAME, sys::TYPES, "name", varchar, Uniqueness::NONE, false},
	    {sys::TYPES_DEFINITION, sys::TYPES, "definition", varchar, Uniqueness::NONE, false},
	    {sys::TYPES_DOC, sys::TYPES, "doc", varchar, Uniqueness::NONE, false},
	    {sys::TX_REENCODED, sys::TX, "reencoded", LogicalType::LIST(RefType()), Uniqueness::NONE, false},
	};
}

static vector<TableDef> BuiltinTables() {
	return {{sys::TABLES, "sys", "tables", "Table definitions", {}},
	        {sys::ATTRIBUTES, "sys", "attributes", "Attribute (column) definitions", {}},
	        {sys::TX, "sys", "tx", "Transactions", {}},
	        {sys::ASSERTIONS, "sys", "assertions", "Commit assertions: queries that must return no rows", {}},
	        {sys::SCHEMAS, "sys", "schemas", "Schemas: namespaces of tables and types", {}},
	        {sys::TYPES, "sys", "types", "Named types (CREATE TYPE)", {}}};
}

static vector<SchemaDef> BuiltinSchemas() {
	return {{sys::MAIN_SCHEMA, "main", "The default schema"}, {sys::SYS_SCHEMA, "sys", "The schema as data"}};
}

shared_ptr<Schema> Schema::Builtin() {
	auto schema = duckdb::make_shared_ptr<Schema>();
	for (auto &def : BuiltinSchemas()) {
		schema->AddSchema(def);
	}
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
string NamedTypeAlias(const string &schema, const string &name) {
	return duckdb::KeywordHelper::WriteOptionallyQuoted(schema) + "." +
	       duckdb::KeywordHelper::WriteOptionallyQuoted(name);
}

bool TypeReferences(const LogicalType &type, const string &alias) {
	if (type.HasAlias() && type.GetAlias() == alias) {
		return true;
	}
	switch (type.id()) {
	case LogicalTypeId::LIST:
		return TypeReferences(duckdb::ListType::GetChildType(type), alias);
	case LogicalTypeId::ARRAY:
		return TypeReferences(duckdb::ArrayType::GetChildType(type), alias);
	case LogicalTypeId::MAP:
		return TypeReferences(duckdb::MapType::KeyType(type), alias) ||
		       TypeReferences(duckdb::MapType::ValueType(type), alias);
	case LogicalTypeId::STRUCT:
		for (auto &child : duckdb::StructType::GetChildTypes(type)) {
			if (TypeReferences(child.second, alias)) {
				return true;
			}
		}
		return false;
	case LogicalTypeId::UNION:
		for (idx_t i = 0; i < duckdb::UnionType::GetMemberCount(type); i++) {
			if (TypeReferences(duckdb::UnionType::GetMemberType(type, i), alias)) {
				return true;
			}
		}
		return false;
	default:
		return false;
	}
}

static constexpr const char *DATABASE_PROPERTY = "mnemonicdb.database";

string DatabaseTag(const Store &store) {
	return store.GetTag();
}

LogicalType MakeNamedType(const LogicalType &type, const string &schema, const string &name,
                          const string &database_tag) {
	auto info = duckdb::make_uniq<duckdb::ExtensionTypeInfo>();
	info->properties[DATABASE_PROPERTY] = Value(database_tag);
	return type.WithAlias(NamedTypeAlias(schema, name)).WithExtensionInfo(std::move(info));
}

string NamedTypeDatabase(const LogicalType &type) {
	if (!type.HasExtensionInfo()) {
		return string();
	}
	auto &properties = type.GetExtensionInfo()->properties;
	auto entry = properties.find(DATABASE_PROPERTY);
	return entry == properties.end() ? string() : entry->second.ToString();
}

LogicalType WithoutNamedTypeTag(const LogicalType &type) {
	if (!type.HasAlias() || !IsNamedTypeAlias(type.GetAlias())) {
		return type;
	}
	auto result = type.WithAlias(string());
	return result.HasExtensionInfo() ? result.WithExtensionInfo(nullptr) : result;
}

bool IsNamedTypeAlias(const string &alias) {
	return !alias.empty() && duckdb::QualifiedName::ParseComponents(alias).size() == 2;
}

LogicalType StripNamedTypeAliases(const LogicalType &type) {
	LogicalType result;
	switch (type.id()) {
	case LogicalTypeId::LIST:
		result = LogicalType::LIST(StripNamedTypeAliases(duckdb::ListType::GetChildType(type)));
		break;
	case LogicalTypeId::ARRAY:
		result = LogicalType::ARRAY(StripNamedTypeAliases(duckdb::ArrayType::GetChildType(type)),
		                            duckdb::ArrayType::GetSize(type));
		break;
	case LogicalTypeId::MAP:
		result = LogicalType::MAP(StripNamedTypeAliases(duckdb::MapType::KeyType(type)),
		                          StripNamedTypeAliases(duckdb::MapType::ValueType(type)));
		break;
	case LogicalTypeId::STRUCT: {
		duckdb::child_list_t<LogicalType> children;
		for (auto &child : duckdb::StructType::GetChildTypes(type)) {
			children.emplace_back(child.first, StripNamedTypeAliases(child.second));
		}
		result = LogicalType::STRUCT(std::move(children));
		break;
	}
	case LogicalTypeId::UNION: {
		duckdb::child_list_t<LogicalType> members;
		for (idx_t i = 0; i < duckdb::UnionType::GetMemberCount(type); i++) {
			members.emplace_back(duckdb::UnionType::GetMemberName(type, i),
			                     StripNamedTypeAliases(duckdb::UnionType::GetMemberType(type, i)));
		}
		result = LogicalType::UNION(std::move(members));
		break;
	}
	default:
		return WithoutNamedTypeTag(type);
	}
	if (type.HasAlias() && !IsNamedTypeAlias(type.GetAlias())) {
		result = result.WithAlias(type.GetAlias());
	}
	return result;
}

void Schema::AddSchema(SchemaDef def) {
	if (schemas.find(def.name) != schemas.end()) {
		throw InvalidInputException("MnemonicDB: schema \"%s\" is defined twice", def.name);
	}
	auto name = def.name;
	schemas.emplace(name, std::move(def));
}

void Schema::AddType(TypeDef def) {
	auto &by_name = types_by_name[def.schema];
	if (by_name.find(def.name) != by_name.end()) {
		throw InvalidInputException("MnemonicDB: type \"%s.%s\" is defined twice", def.schema, def.name);
	}
	by_name[def.name] = def.id;
	auto id = def.id;
	types.emplace(id, std::move(def));
}

const SchemaDef *Schema::FindSchema(const string &name) const {
	auto entry = schemas.find(name);
	return entry == schemas.end() ? nullptr : &entry->second;
}

vector<const SchemaDef *> Schema::AllSchemas() const {
	vector<const SchemaDef *> result;
	for (auto &entry : schemas) {
		result.push_back(&entry.second);
	}
	std::sort(result.begin(), result.end(), [](const SchemaDef *a, const SchemaDef *b) { return a->id < b->id; });
	return result;
}

const TypeDef *Schema::FindType(const string &schema, const string &name) const {
	auto by_name = types_by_name.find(schema);
	if (by_name == types_by_name.end()) {
		return nullptr;
	}
	auto entry = by_name->second.find(name);
	return entry == by_name->second.end() ? nullptr : &types.at(entry->second);
}

vector<const TypeDef *> Schema::TypesInSchema(const string &schema) const {
	vector<const TypeDef *> result;
	for (auto &entry : types) {
		if (duckdb::StringUtil::CIEquals(entry.second.schema, schema)) {
			result.push_back(&entry.second);
		}
	}
	std::sort(result.begin(), result.end(), [](const TypeDef *a, const TypeDef *b) { return a->id < b->id; });
	return result;
}

vector<const TypeDef *> Schema::AllTypes() const {
	vector<const TypeDef *> result;
	for (auto &entry : types) {
		result.push_back(&entry.second);
	}
	std::sort(result.begin(), result.end(), [](const TypeDef *a, const TypeDef *b) { return a->id < b->id; });
	return result;
}

//! The named type of this database a type expression names, if any: a
//! two-part `schema.name` with no type parameters
static const TypeDef *NamedTypeOf(const duckdb::TypeExpression &type_expr, const Schema &schema) {
	auto &path = type_expr.GetQualifiedName().Path();
	if (path.size() != 2 || !type_expr.GetChildren().empty()) {
		return nullptr;
	}
	return schema.FindType(path[0].GetIdentifierName(), path[1].GetIdentifierName());
}

static bool MentionsNamedType(const duckdb::ParsedExpression &expr, const Schema &schema) {
	if (expr.GetExpressionClass() != duckdb::ExpressionClass::TYPE) {
		return false;
	}
	auto &type_expr = expr.Cast<duckdb::TypeExpression>();
	if (NamedTypeOf(type_expr, schema)) {
		return true;
	}
	for (auto &child : type_expr.GetChildren()) {
		if (MentionsNamedType(*child, schema)) {
			return true;
		}
	}
	return false;
}

//! Resolve a parsed type expression. This database's named types are resolved
//! here, as are the nested types that contain them; everything else binds
//! through DuckDB (built-in types, REF, ENUM(...), DECIMAL(p,s), ...)
//! A schema-qualified type name in `expr` that is not a named type of this
//! database, if any
static const duckdb::TypeExpression *UnknownNamedType(const duckdb::ParsedExpression &expr, const Schema &schema) {
	if (expr.GetExpressionClass() != duckdb::ExpressionClass::TYPE) {
		return nullptr;
	}
	auto &type_expr = expr.Cast<duckdb::TypeExpression>();
	if (type_expr.GetQualifiedName().Path().size() >= 2 && !NamedTypeOf(type_expr, schema)) {
		return &type_expr;
	}
	for (auto &child : type_expr.GetChildren()) {
		if (auto unknown = UnknownNamedType(*child, schema)) {
			return unknown;
		}
	}
	return nullptr;
}

static LogicalType ResolveTypeExpression(const duckdb::ParsedExpression &expr, const Schema &schema,
                                         duckdb::ClientContext &context) {
	if (auto unknown = UnknownNamedType(expr, schema)) {
		// Stored types only ever name this database's own types: never fall back
		// to the binder, which would find another catalog's type of that name
		throw duckdb::IOException("MnemonicDB: stored type \"%s\" uses named type \"%s\", which this database "
		                          "does not define",
		                          expr.ToString(), unknown->GetQualifiedName().ToString());
	}
	if (!MentionsNamedType(expr, schema)) {
		auto binder = duckdb::Binder::CreateBinder(context, nullptr);
		auto result = LogicalType::UNBOUND(expr.Copy());
		binder->BindLogicalType(result);
		return result;
	}
	auto &type_expr = expr.Cast<duckdb::TypeExpression>();
	if (auto def = NamedTypeOf(type_expr, schema)) {
		return def->type;
	}
	auto &children = type_expr.GetChildren();
	auto name = duckdb::StringUtil::Lower(type_expr.GetTypeName().GetIdentifierName());
	bool unqualified = type_expr.GetQualifiedName().Path().size() == 1;
	if (unqualified && (name == "struct" || name == "row" || name == "union")) {
		duckdb::child_list_t<LogicalType> members;
		for (auto &child : children) {
			members.emplace_back(child->GetAlias(), ResolveTypeExpression(*child, schema, context));
		}
		return name == "union" ? LogicalType::UNION(std::move(members)) : LogicalType::STRUCT(std::move(members));
	}
	if (unqualified && name == "list" && children.size() == 1) {
		return LogicalType::LIST(ResolveTypeExpression(*children[0], schema, context));
	}
	if (unqualified && name == "map" && children.size() == 2) {
		return LogicalType::MAP(ResolveTypeExpression(*children[0], schema, context),
		                        ResolveTypeExpression(*children[1], schema, context));
	}
	int64_t size;
	if (unqualified && name == "array" && children.size() == 2 &&
	    children[1]->GetExpressionClass() == duckdb::ExpressionClass::CONSTANT &&
	    children[1]->Cast<duckdb::ConstantExpression>().GetLiteral().TryGetInt64(size) && size > 0) {
		return LogicalType::ARRAY(ResolveTypeExpression(*children[0], schema, context), idx_t(size));
	}
	throw duckdb::IOException("MnemonicDB: cannot resolve stored type \"%s\"", expr.ToString());
}

LogicalType Schema::ParseType(const string &type_string, duckdb::ClientContext &context) const {
	auto columns = duckdb::Parser::ParseColumnList("dummy " + type_string);
	auto type = columns.GetColumn(duckdb::LogicalIndex(0)).Type();
	if (type.id() != LogicalTypeId::UNBOUND) {
		return type;
	}
	return ResolveTypeExpression(*duckdb::UnboundType::GetTypeExpression(type), *this, context);
}

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
	if (auto existing = FindAttribute(table->second, attribute.name)) {
		if (existing->Local() < 64 && attribute.Local() >= 64) {
			// A built-in column added by a newer version to a user-extensible sys
			// table (sys.tx) collides with a user column of an older database: the
			// user column keeps the name and the built-in is shadowed (hidden from
			// SQL, still written) until the user column is renamed
			auto &list = table->second.attributes;
			list.erase(std::remove(list.begin(), list.end(), existing->Local()), list.end());
			list.push_back(attribute.Local());
			auto local = attribute.Local();
			attributes.emplace(local, std::move(attribute));
			return;
		}
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

shared_ptr<Schema> Schema::WithSchema(SchemaDef def) const {
	auto result = duckdb::make_shared_ptr<Schema>(*this);
	result->AddSchema(std::move(def));
	return result;
}

shared_ptr<Schema> Schema::WithoutSchema(const string &name) const {
	auto result = duckdb::make_shared_ptr<Schema>(*this);
	result->schemas.erase(name);
	result->tables_by_name.erase(name);
	result->types_by_name.erase(name);
	return result;
}

shared_ptr<Schema> Schema::WithType(TypeDef def) const {
	auto result = duckdb::make_shared_ptr<Schema>(*this);
	result->AddType(std::move(def));
	return result;
}

shared_ptr<Schema> Schema::WithoutType(EntityId id) const {
	auto result = duckdb::make_shared_ptr<Schema>(*this);
	auto entry = result->types.find(id);
	if (entry == result->types.end()) {
		throw duckdb::InternalException("MnemonicDB: removing unknown type %d", id);
	}
	result->types_by_name[entry->second.schema].erase(entry->second.name);
	result->types.erase(entry);
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

const AttributeDef *Schema::FindIndex(const string &schema, const string &name) const {
	for (auto &entry : attributes) {
		if (entry.second.index_name.empty() || !duckdb::StringUtil::CIEquals(entry.second.index_name, name)) {
			continue;
		}
		auto table = FindTable(entry.second.table);
		if (table && duckdb::StringUtil::CIEquals(table->schema, schema)) {
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
	std::sort(result.begin(), result.end(), [](const AttributeDef *a, const AttributeDef *b) { return a->id < b->id; });
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

	auto &varchar_codec = schema->GetAttribute(sys::SCHEMAS_NAME).codec;
	for (auto &datom : view.ScanPrefix(ColumnFamily::AEVT, AevtPrefix(sys::SCHEMAS_NAME))) {
		if (LocalOf(datom.e) < 16) {
			continue;
		}
		SchemaDef def;
		def.id = datom.e;
		def.name = varchar_codec.DecodeValue(datom.v).ToString();
		auto doc = CurrentValue(view, *schema, datom.e, sys::SCHEMAS_DOC);
		def.doc = doc ? doc->ToString() : string();
		schema->AddSchema(std::move(def));
	}

	// Named types, in creation order: a type can only use types created before it
	vector<TypeDef> user_types;
	for (auto &datom : view.ScanPrefix(ColumnFamily::AEVT, AevtPrefix(sys::TYPES_NAME))) {
		TypeDef def;
		def.id = datom.e;
		def.name = varchar_codec.DecodeValue(datom.v).ToString();
		def.schema = RequiredString(view, *schema, datom.e, sys::TYPES_SCHEMA, "type schema");
		def.definition = RequiredString(view, *schema, datom.e, sys::TYPES_DEFINITION, "type definition");
		auto doc = CurrentValue(view, *schema, datom.e, sys::TYPES_DOC);
		def.doc = doc ? doc->ToString() : string();
		user_types.push_back(std::move(def));
	}
	std::sort(user_types.begin(), user_types.end(), [](const TypeDef &a, const TypeDef &b) { return a.id < b.id; });
	for (auto &def : user_types) {
		def.type = MakeNamedType(schema->ParseType(def.definition, context), def.schema, def.name,
		                         DatabaseTag(view.GetStore()));
		schema->AddType(std::move(def));
	}

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
		user_attributes.emplace_back(datom.e, table, name, schema->ParseType(type_name, context),
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
	for (auto &def : BuiltinSchemas()) {
		WriteSchema(writer, *schema, def);
	}
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

void Schema::WriteSchema(TxWriter &writer, const Schema &schema, const SchemaDef &def) {
	AssertValue(writer, schema, def.id, sys::SCHEMAS_NAME, Value(def.name));
	if (!def.doc.empty()) {
		AssertValue(writer, schema, def.id, sys::SCHEMAS_DOC, Value(def.doc));
	}
}

void Schema::WriteType(TxWriter &writer, const Schema &schema, const TypeDef &def) {
	AssertValue(writer, schema, def.id, sys::TYPES_SCHEMA, Value(def.schema));
	AssertValue(writer, schema, def.id, sys::TYPES_NAME, Value(def.name));
	AssertValue(writer, schema, def.id, sys::TYPES_DEFINITION, Value(def.definition));
	if (!def.doc.empty()) {
		AssertValue(writer, schema, def.id, sys::TYPES_DOC, Value(def.doc));
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
	AssertValue(writer, schema, attr.id, sys::ATTRIBUTES_TYPE, Value(attr.declared.ToString()));
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
