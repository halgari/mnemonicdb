# Architecture

This document describes MnemonicDB's physical storage layout, indexing strategy, and stored procedures.

## Table Structure

### Value-Type Tables

Each PostgreSQL data type has a dedicated datom table:

```sql
CREATE TABLE datoms_text (
  e           BIGINT NOT NULL,      -- Entity ID
  a           BIGINT NOT NULL,      -- Attribute ID
  v           TEXT NOT NULL,        -- Value
  tx          BIGINT NOT NULL,      -- Transaction ID (assertion)
  retracted_by BIGINT,              -- Transaction ID (retraction) or NULL

  CONSTRAINT datoms_text_pk PRIMARY KEY (e, a, v, tx)
);

CREATE TABLE datoms_int8 (
  e           BIGINT NOT NULL,
  a           BIGINT NOT NULL,
  v           BIGINT NOT NULL,
  tx          BIGINT NOT NULL,
  retracted_by BIGINT,

  CONSTRAINT datoms_int8_pk PRIMARY KEY (e, a, v, tx)
);

-- Similar tables for: int4, float4, float8, numeric, bool,
-- timestamptz, date, uuid, bytea, jsonb, ref
```

The primary key `(e, a, v, tx)` ensures:
- No duplicate datoms (same fact asserted twice in same tx)
- Cardinality-many attributes work correctly (multiple v for same e, a)
- History is preserved (same e, a, v can exist with different tx)

### Reference Table

References store entity-to-entity relationships:

```sql
CREATE TABLE datoms_ref (
  e           BIGINT NOT NULL,      -- Source entity
  a           BIGINT NOT NULL,      -- Attribute ID
  v           BIGINT NOT NULL,      -- Target entity ID
  tx          BIGINT NOT NULL,
  retracted_by BIGINT,

  CONSTRAINT datoms_ref_pk PRIMARY KEY (e, a, v, tx)
);
```

### Transactions Table

```sql
CREATE TABLE transactions (
  id          BIGINT PRIMARY KEY,   -- Transaction ID (from :db.part/tx partition)
  tx_instant  TIMESTAMPTZ NOT NULL  -- When transaction was committed
);
```

Transaction metadata (user, reason, etc.) is stored as datoms referencing the transaction entity.

### Entity Allocation

```sql
CREATE TABLE partitions (
  id          INTEGER PRIMARY KEY,  -- Partition ID (encoded in high bits)
  ident       TEXT UNIQUE NOT NULL, -- e.g., 'db', 'tx', 'user'
  next_id     BIGINT NOT NULL       -- Next available entity ID
);
```

## Indexes

### EAVT Index (Primary)

The primary key on each datom table provides EAVT ordering:

```sql
-- Implicit from PRIMARY KEY (e, a, v, tx)
-- Supports: lookup by entity, entity+attribute, exact datom match
```

### AVET Index

For looking up entities by attribute value:

```sql
CREATE INDEX datoms_text_avet ON datoms_text (a, v, tx)
  WHERE retracted_by IS NULL;

CREATE INDEX datoms_int8_avet ON datoms_int8 (a, v, tx)
  WHERE retracted_by IS NULL;

-- Similar for other value types
```

The partial index (`WHERE retracted_by IS NULL`) keeps the index small by excluding historical data.

Use cases:
- Find entity by unique attribute: `WHERE a = :person/email AND v = 'alice@example.com'`
- Range queries: `WHERE a = :order/total AND v > 1000`

### VAET Index (References Only)

For reverse reference lookups:

```sql
CREATE INDEX datoms_ref_vaet ON datoms_ref (v, a, e, tx)
  WHERE retracted_by IS NULL;
```

Use cases:
- Find all entities referencing a given entity
- "What orders belong to this customer?"
- "What persons work in this department?"

### Transaction Index

For temporal queries:

```sql
CREATE INDEX datoms_text_tx ON datoms_text (tx);
CREATE INDEX datoms_text_retracted ON datoms_text (retracted_by)
  WHERE retracted_by IS NOT NULL;
```

## Stored Procedures

### Core Functions

#### `mnemonic_allocate_entity(partition_ident TEXT) -> BIGINT`

Allocates a new entity ID from the specified partition:

```sql
CREATE FUNCTION mnemonic_allocate_entity(partition_ident TEXT)
RETURNS BIGINT AS $$
DECLARE
  p RECORD;
  new_id BIGINT;
BEGIN
  SELECT * INTO p FROM partitions WHERE ident = partition_ident FOR UPDATE;
  new_id := p.next_id;
  UPDATE partitions SET next_id = next_id + 1 WHERE id = p.id;
  RETURN (p.id::BIGINT << 48) | new_id;
END;
$$ LANGUAGE plpgsql;
```

#### `mnemonic_new_transaction() -> BIGINT`

Creates a new transaction and returns its ID:

```sql
CREATE FUNCTION mnemonic_new_transaction()
RETURNS BIGINT AS $$
DECLARE
  tx_id BIGINT;
BEGIN
  tx_id := mnemonic_allocate_entity('tx');
  INSERT INTO transactions (id, tx_instant) VALUES (tx_id, NOW());
  -- Assert :db/txInstant datom
  INSERT INTO datoms_timestamptz (e, a, v, tx, retracted_by)
  VALUES (tx_id, attr_id('db/txInstant'), NOW(), tx_id, NULL);
  RETURN tx_id;
END;
$$ LANGUAGE plpgsql;
```

#### `attr_id(ident TEXT) -> BIGINT`

Returns the entity ID for an attribute by its ident:

```sql
CREATE FUNCTION attr_id(ident TEXT)
RETURNS BIGINT AS $$
  SELECT e FROM datoms_text
  WHERE a = (SELECT e FROM datoms_text WHERE v = 'db/ident' AND retracted_by IS NULL LIMIT 1)
    AND v = ident
    AND retracted_by IS NULL
  LIMIT 1;
$$ LANGUAGE sql STABLE;
```

### View Trigger Functions

#### `mnemonic_view_insert(view_name TEXT)`

Handles INSERT on projection views:

```sql
CREATE FUNCTION mnemonic_view_insert()
RETURNS TRIGGER AS $$
DECLARE
  view_def RECORD;
  attr RECORD;
  new_entity BIGINT;
  tx_id BIGINT;
BEGIN
  -- Get view definition
  SELECT * INTO view_def FROM mnemonic_view_definitions WHERE name = TG_ARGV[0];

  -- Allocate entity and transaction
  new_entity := mnemonic_allocate_entity('user');
  tx_id := mnemonic_new_transaction();

  -- Assert datoms for each non-null column
  FOR attr IN SELECT * FROM mnemonic_view_attributes WHERE view_name = TG_ARGV[0]
  LOOP
    -- Dynamic SQL to insert into appropriate datom table
    EXECUTE mnemonic_build_assert_sql(attr, new_entity, tx_id, NEW);
  END LOOP;

  -- Return the new row with allocated ID
  NEW.id := new_entity;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;
```

#### `mnemonic_view_update(view_name TEXT)`

Handles UPDATE on projection views:

```sql
CREATE FUNCTION mnemonic_view_update()
RETURNS TRIGGER AS $$
DECLARE
  attr RECORD;
  tx_id BIGINT;
BEGIN
  tx_id := mnemonic_new_transaction();

  FOR attr IN SELECT * FROM mnemonic_view_attributes WHERE view_name = TG_ARGV[0]
  LOOP
    -- Check if value changed
    IF mnemonic_value_changed(attr, OLD, NEW) THEN
      -- Retract old value (if not null)
      IF mnemonic_get_value(attr, OLD) IS NOT NULL THEN
        EXECUTE mnemonic_build_retract_sql(attr, OLD.id, tx_id);
      END IF;
      -- Assert new value (if not null)
      IF mnemonic_get_value(attr, NEW) IS NOT NULL THEN
        EXECUTE mnemonic_build_assert_sql(attr, OLD.id, tx_id, NEW);
      END IF;
    END IF;
  END LOOP;

  RETURN NEW;
END;
$$ LANGUAGE plpgsql;
```

#### `mnemonic_view_delete(view_name TEXT)`

Handles DELETE on projection views:

```sql
CREATE FUNCTION mnemonic_view_delete()
RETURNS TRIGGER AS $$
DECLARE
  attr RECORD;
  tx_id BIGINT;
BEGIN
  tx_id := mnemonic_new_transaction();

  -- Retract all attributes in this view
  FOR attr IN SELECT * FROM mnemonic_view_attributes WHERE view_name = TG_ARGV[0]
  LOOP
    EXECUTE mnemonic_build_retract_sql(attr, OLD.id, tx_id);
  END LOOP;

  RETURN OLD;
END;
$$ LANGUAGE plpgsql;
```

### View Generation

#### `mnemonic_regenerate_views()`

Main procedure to regenerate all projection views:

```sql
CREATE PROCEDURE mnemonic_regenerate_views()
AS $$
DECLARE
  view_rec RECORD;
BEGIN
  -- Iterate over all view definitions in the database
  FOR view_rec IN
    SELECT DISTINCT v.v AS view_name
    FROM datoms_text v
    WHERE v.a = attr_id('db.view/ident')
      AND v.retracted_by IS NULL
  LOOP
    CALL mnemonic_regenerate_view(view_rec.view_name);
  END LOOP;
END;
$$ LANGUAGE plpgsql;
```

#### `mnemonic_regenerate_view(view_name TEXT)`

Regenerates a single projection view:

```sql
CREATE PROCEDURE mnemonic_regenerate_view(view_name TEXT)
AS $$
DECLARE
  view_sql TEXT;
  attr_rec RECORD;
BEGIN
  -- Drop existing view and triggers
  EXECUTE format('DROP VIEW IF EXISTS %I CASCADE', view_name);

  -- Build SELECT statement
  view_sql := mnemonic_build_view_select(view_name);

  -- Create view
  EXECUTE format('CREATE VIEW %I AS %s', view_name, view_sql);

  -- Create triggers
  EXECUTE format(
    'CREATE TRIGGER %I_insert INSTEAD OF INSERT ON %I
     FOR EACH ROW EXECUTE FUNCTION mnemonic_view_insert(%L)',
    view_name, view_name, view_name
  );

  EXECUTE format(
    'CREATE TRIGGER %I_update INSTEAD OF UPDATE ON %I
     FOR EACH ROW EXECUTE FUNCTION mnemonic_view_update(%L)',
    view_name, view_name, view_name
  );

  EXECUTE format(
    'CREATE TRIGGER %I_delete INSTEAD OF DELETE ON %I
     FOR EACH ROW EXECUTE FUNCTION mnemonic_view_delete(%L)',
    view_name, view_name, view_name
  );
END;
$$ LANGUAGE plpgsql;
```

## Bootstrap Schema

System attributes are defined at database initialization:

```sql
-- Core attributes for defining attributes
INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES
  (1, 1, 'db/ident', 0, NULL),        -- :db/ident identifies itself
  (2, 1, 'db/valueType', 0, NULL),
  (3, 1, 'db/cardinality', 0, NULL),
  (4, 1, 'db/unique', 0, NULL),
  (5, 1, 'db/doc', 0, NULL);

-- Value type entities
INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES
  (100, 1, 'db.type/text', 0, NULL),
  (101, 1, 'db.type/int8', 0, NULL),
  (102, 1, 'db.type/ref', 0, NULL),
  -- ... etc

-- Cardinality entities
INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES
  (200, 1, 'db.cardinality/one', 0, NULL),
  (201, 1, 'db.cardinality/many', 0, NULL);

-- View definition attributes
INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES
  (10, 1, 'db.view/ident', 0, NULL),
  (11, 1, 'db.view/attributes', 0, NULL),
  (12, 1, 'db.view/doc', 0, NULL);
```

## Performance Verification

To verify query planner behavior without materialized views:

```sql
-- Check query plan for view access
EXPLAIN ANALYZE SELECT * FROM persons WHERE id = 12345;

-- Check query plan for value lookup
EXPLAIN ANALYZE SELECT * FROM persons WHERE email = 'test@example.com';

-- Check query plan for as-of query
EXPLAIN ANALYZE SELECT * FROM mnemonic_as_of('persons', 500);
```

Key things to verify:
- Index scans (not sequential scans) on datom tables
- Efficient join strategies for view projections
- Reasonable row estimates from the planner

If performance is insufficient, consider:
1. Adding more targeted indexes
2. Using materialized views for frequently-accessed projections
3. Caching as-of results for common timestamps
