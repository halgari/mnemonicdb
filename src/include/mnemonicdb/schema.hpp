#pragma once

#include "mnemonicdb/entity_id.hpp"
#include "mnemonicdb/value_codec.hpp"

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace duckdb {
class ClientContext;
}

namespace mnemonicdb {

using duckdb::shared_ptr;
using duckdb::vector;

class TxWriter;
class ReadView;

enum class Uniqueness : uint8_t { NONE, VALUE, IDENTITY };

const char *UniquenessName(Uniqueness unique);

//! The REF logical type: an entity ID, distinguished from UBIGINT by its alias
LogicalType RefType();
bool IsRefType(const LogicalType &type);

//! One attribute: a column of a table
struct AttributeDef {
	AttributeDef(EntityId id, EntityId table, string name, LogicalType type, Uniqueness unique, bool indexed,
	             string index_name = string());

	EntityId id;
	EntityId table;
	string name;
	//! The declared column type; a top-level LIST means cardinality-many
	LogicalType type;
	Uniqueness unique;
	//! Whether CREATE INDEX added AVET entries, and the index's name
	bool indexed;
	string index_name;

	bool many;
	//! The type of one stored value: the list element type when `many`, else
	//! `type`
	LogicalType value_type;
	bool is_ref;
	ValueCodec codec;

	AttrId Local() const {
		return static_cast<AttrId>(LocalOf(id));
	}
	//! Whether the attribute has AVET entries
	bool HasAvet() const {
		return indexed || unique != Uniqueness::NONE;
	}
};

//! One table: a namespace of attributes
struct TableDef {
	EntityId id;
	string schema;
	string name;
	string doc;
	//! Attribute locals, in column order
	vector<AttrId> attributes;
};

//! An immutable snapshot of all table and attribute definitions
class Schema {
public:
	//! The built-in definitions of the `sys` tables, without any user tables
	static shared_ptr<Schema> Builtin();
	//! Read all definitions visible in `view`
	static shared_ptr<Schema> Load(ReadView &view, duckdb::ClientContext &context);
	//! Write the datoms describing the built-in `sys` tables (the bootstrap
	//! transaction)
	static void WriteBootstrap(TxWriter &writer);
	//! Write the datoms defining a table or an attribute
	static void WriteTable(TxWriter &writer, const Schema &schema, const TableDef &table);
	static void WriteAttribute(TxWriter &writer, const Schema &schema, const AttributeDef &attr);
	//! Write the transaction entity of transaction `tx`
	static void WriteTxEntity(TxWriter &writer, const Schema &schema, TxId tx, int64_t instant);

	const TableDef *FindTable(const string &schema, const string &name) const;
	const TableDef *FindTable(EntityId id) const;
	const AttributeDef &GetAttribute(AttrId local) const;
	//! The attribute with this local number, if it is part of the schema (dropped
	//! columns are not)
	const AttributeDef *FindAttributeById(AttrId local) const;
	const AttributeDef *FindAttribute(const TableDef &table, const string &name) const;
	vector<const TableDef *> TablesInSchema(const string &schema) const;

	//! A copy of this schema with one more table; used for DDL inside a
	//! transaction
	shared_ptr<Schema> WithTable(TableDef table, vector<AttributeDef> attributes) const;
	//! A copy of this schema with one attribute definition replaced
	shared_ptr<Schema> WithAttribute(AttributeDef attribute) const;
	//! A copy of this schema with an attribute appended to its table
	shared_ptr<Schema> WithNewAttribute(AttributeDef attribute) const;
	//! A copy of this schema without an attribute
	shared_ptr<Schema> WithoutAttribute(AttrId local) const;
	//! A copy of this schema with a table renamed
	shared_ptr<Schema> WithTableName(EntityId table, const string &name) const;
	//! The attribute carrying the index named `name`, if any
	const AttributeDef *FindIndex(const string &name) const;
	vector<const AttributeDef *> IndexedAttributes() const;
	vector<const AttributeDef *> AllAttributes() const;

private:
	void AddTable(TableDef table);
	void AddAttribute(AttributeDef attribute);

private:
	duckdb::unordered_map<EntityId, TableDef> tables;
	duckdb::unordered_map<AttrId, AttributeDef> attributes;
	duckdb::case_insensitive_map_t<duckdb::case_insensitive_map_t<EntityId>> tables_by_name;
};

//! IDs of the built-in schema entities
namespace sys {
static constexpr EntityId TABLES = (uint64_t(partition::TABLE) << PARTITION_SHIFT) | 1;
static constexpr EntityId ATTRIBUTES = (uint64_t(partition::TABLE) << PARTITION_SHIFT) | 2;
static constexpr EntityId TX = (uint64_t(partition::TABLE) << PARTITION_SHIFT) | 3;

static constexpr AttrId TABLES_SCHEMA = 1;
static constexpr AttrId TABLES_NAME = 2;
static constexpr AttrId TABLES_DOC = 3;
static constexpr AttrId ATTRIBUTES_TABLE = 4;
static constexpr AttrId ATTRIBUTES_NAME = 5;
static constexpr AttrId ATTRIBUTES_TYPE = 6;
static constexpr AttrId ATTRIBUTES_UNIQUE = 7;
static constexpr AttrId ATTRIBUTES_INDEXED = 8;
static constexpr AttrId ATTRIBUTES_DOC = 9;
static constexpr AttrId TX_INSTANT = 10;
static constexpr AttrId ATTRIBUTES_INDEX = 11;
} // namespace sys

} // namespace mnemonicdb
