# Schema

In MnemonicDB, **schema is data**. Attributes and views are stored as datoms in the database itself, making the system self-describing and enabling dynamic schema operations via stored procedures.

## Attributes

An **attribute** defines a property that can be asserted about entities. Attributes are themselves entities in the `:db.part/db` partition.

### Attribute Properties

Each attribute has the following properties:

| Property | Type | Description |
|----------|------|-------------|
| `:db/ident` | string | Namespaced keyword identifier (e.g., `person/name`) |
| `:db/valueType` | ref | Reference to a value type entity |
| `:db/cardinality` | ref | `:db.cardinality/one` or `:db.cardinality/many` |
| `:db/unique` | ref? | `:db.unique/identity` or `:db.unique/value` (optional) |
| `:db/doc` | string? | Documentation string (optional) |

### Example: Defining an Attribute

As datoms:

```
[attr/1, :db/ident,       "person/name",           tx/1, null]
[attr/1, :db/valueType,   :db.type/text,           tx/1, null]
[attr/1, :db/cardinality, :db.cardinality/one,     tx/1, null]
[attr/1, :db/doc,         "A person's full name",  tx/1, null]
```

## Value Types

MnemonicDB supports one value type for each PostgreSQL data type, plus references:

| Value Type | PostgreSQL Type | Table |
|------------|-----------------|-------|
| `:db.type/text` | text | `datoms_text` |
| `:db.type/int4` | integer | `datoms_int4` |
| `:db.type/int8` | bigint | `datoms_int8` |
| `:db.type/float4` | real | `datoms_float4` |
| `:db.type/float8` | double precision | `datoms_float8` |
| `:db.type/numeric` | numeric | `datoms_numeric` |
| `:db.type/bool` | boolean | `datoms_bool` |
| `:db.type/timestamptz` | timestamp with time zone | `datoms_timestamptz` |
| `:db.type/date` | date | `datoms_date` |
| `:db.type/uuid` | uuid | `datoms_uuid` |
| `:db.type/bytea` | bytea | `datoms_bytea` |
| `:db.type/jsonb` | jsonb | `datoms_jsonb` |
| `:db.type/ref` | bigint (entity ID) | `datoms_ref` |

## Uniqueness

Attributes can have uniqueness constraints:

### `:db.unique/identity`

The attribute's value uniquely identifies an entity. Used for natural keys like email addresses or external IDs. Upserting by this value will find or create the entity.

### `:db.unique/value`

The attribute's value must be unique across all entities, but is not used for identity resolution. Used for constraints like "department code must be unique."

## Schema Migrations

Schema changes are made by asserting datoms directly via SQL. Use whatever migration system you prefer (Drizzle migrations, raw SQL files, etc.):

```sql
-- Migration: Add person attributes
DO $$
DECLARE
  tx_id BIGINT;
  attr_id BIGINT;
BEGIN
  tx_id := mnemonic_new_transaction();

  -- Create person/name attribute
  attr_id := mnemonic_allocate_entity('db');
  INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES
    (attr_id, attr_id('db/ident'), 'person/name', tx_id, NULL);
  INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES
    (attr_id, attr_id('db/valueType'), attr_id('db.type/text'), tx_id, NULL),
    (attr_id, attr_id('db/cardinality'), attr_id('db.cardinality/one'), tx_id, NULL);

  -- Create person/email attribute (with unique identity)
  attr_id := mnemonic_allocate_entity('db');
  INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES
    (attr_id, attr_id('db/ident'), 'person/email', tx_id, NULL);
  INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES
    (attr_id, attr_id('db/valueType'), attr_id('db.type/text'), tx_id, NULL),
    (attr_id, attr_id('db/cardinality'), attr_id('db.cardinality/one'), tx_id, NULL),
    (attr_id, attr_id('db/unique'), attr_id('db.unique/identity'), tx_id, NULL);

  -- ... more attributes
END $$;

-- Regenerate views after schema changes
CALL mnemonic_regenerate_views();
```

### Workflow

1. Write migration SQL that asserts new attribute/view datoms
2. Run migration with your preferred tool
3. Call `mnemonic_regenerate_views()` to update projection views
4. Run ORM introspection to generate/update types (e.g., `drizzle-kit pull`)

This approach:
- No custom schema format to learn
- Works with any migration tool
- ORMs introspect the generated views like regular tables
- Schema history is preserved as datoms

## Querying Schema

Since schema is data, you can query it:

```sql
-- Query all attributes
SELECT
  ident.v AS ident,
  vtype_ident.v AS value_type,
  card_ident.v AS cardinality
FROM datoms_text ident
JOIN datoms_ref vtype ON ident.e = vtype.e AND vtype.a = attr_id('db/valueType')
JOIN datoms_text vtype_ident ON vtype.v = vtype_ident.e AND vtype_ident.a = attr_id('db/ident')
JOIN datoms_ref card ON ident.e = card.e AND card.a = attr_id('db/cardinality')
JOIN datoms_text card_ident ON card.v = card_ident.e AND card_ident.a = attr_id('db/ident')
WHERE ident.a = attr_id('db/ident')
  AND ident.retracted_by IS NULL;
```

## View Definitions

Views are also entities with attributes:

| Attribute | Type | Description |
|-----------|------|-------------|
| `:db.view/ident` | text | View name (e.g., `persons`) |
| `:db.view/doc` | text | Documentation |
| `:db.view/attributes` | ref (many) | References to attribute entities |

```
[view/1, :db.view/ident,      "persons",            tx/1, null]
[view/1, :db.view/doc,        "Person entities",   tx/1, null]
[view/1, :db.view/attributes, attr/person-name,    tx/1, null]
[view/1, :db.view/attributes, attr/person-email,   tx/1, null]
[view/1, :db.view/attributes, attr/person-dept,    tx/1, null]
```

## Schema Evolution

Attributes can be added at any time. Modifications to existing attributes require migrations:

### Safe Changes (no migration needed)
- Adding new attributes
- Adding new views
- Changing `:db/doc`

### Changes Requiring Migration
- Changing `:db/valueType` (requires data conversion)
- Changing `:db/cardinality` from many to one (requires picking one value)
- Changing `:db/unique` (requires uniqueness validation)

### Removing Attributes

Attributes are never truly removed—they can be marked deprecated. Existing datoms remain queryable for historical analysis.

## Regenerating Views

A stored procedure reads view definitions from the database and regenerates the SQL views:

```sql
CALL mnemonic_regenerate_views();
```

This procedure:
1. Queries all view entities and their attributes
2. Drops existing projection views
3. Creates new views with appropriate columns and joins
4. Creates `INSTEAD OF` triggers for INSERT/UPDATE/DELETE

This enables runtime schema changes: add a new attribute to a view definition, call the procedure, and the view is updated.
