-- MnemonicDB Bootstrap Schema
-- Initializes the database with system tables, functions, and schema

--------------------------------------------------------------------------------
-- PARTITIONS
--------------------------------------------------------------------------------

CREATE TABLE partitions (
  id        INTEGER PRIMARY KEY,
  ident     TEXT UNIQUE NOT NULL,
  next_id   BIGINT NOT NULL DEFAULT 1
);

-- System partitions
-- Partition IDs are encoded in the high 16 bits of entity IDs
INSERT INTO partitions (id, ident, next_id) VALUES
  (0, 'db', 1),      -- System schema (attributes, types, etc.)
  (1, 'tx', 1),      -- Transactions
  (2, 'user', 1);    -- User data

--------------------------------------------------------------------------------
-- TRANSACTIONS TABLE
--------------------------------------------------------------------------------

CREATE TABLE transactions (
  id          BIGINT PRIMARY KEY,
  tx_instant  TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

--------------------------------------------------------------------------------
-- DATOM TABLES (one per value type)
--------------------------------------------------------------------------------

-- Text values
CREATE TABLE datoms_text (
  e            BIGINT NOT NULL,
  a            BIGINT NOT NULL,
  v            TEXT NOT NULL,
  tx           BIGINT NOT NULL,
  retracted_by BIGINT,
  CONSTRAINT datoms_text_pk PRIMARY KEY (e, a, v, tx)
);

-- 32-bit integers
CREATE TABLE datoms_int4 (
  e            BIGINT NOT NULL,
  a            BIGINT NOT NULL,
  v            INTEGER NOT NULL,
  tx           BIGINT NOT NULL,
  retracted_by BIGINT,
  CONSTRAINT datoms_int4_pk PRIMARY KEY (e, a, v, tx)
);

-- 64-bit integers
CREATE TABLE datoms_int8 (
  e            BIGINT NOT NULL,
  a            BIGINT NOT NULL,
  v            BIGINT NOT NULL,
  tx           BIGINT NOT NULL,
  retracted_by BIGINT,
  CONSTRAINT datoms_int8_pk PRIMARY KEY (e, a, v, tx)
);

-- 32-bit floats
CREATE TABLE datoms_float4 (
  e            BIGINT NOT NULL,
  a            BIGINT NOT NULL,
  v            REAL NOT NULL,
  tx           BIGINT NOT NULL,
  retracted_by BIGINT,
  CONSTRAINT datoms_float4_pk PRIMARY KEY (e, a, v, tx)
);

-- 64-bit floats
CREATE TABLE datoms_float8 (
  e            BIGINT NOT NULL,
  a            BIGINT NOT NULL,
  v            DOUBLE PRECISION NOT NULL,
  tx           BIGINT NOT NULL,
  retracted_by BIGINT,
  CONSTRAINT datoms_float8_pk PRIMARY KEY (e, a, v, tx)
);

-- Arbitrary precision numeric
CREATE TABLE datoms_numeric (
  e            BIGINT NOT NULL,
  a            BIGINT NOT NULL,
  v            NUMERIC NOT NULL,
  tx           BIGINT NOT NULL,
  retracted_by BIGINT,
  CONSTRAINT datoms_numeric_pk PRIMARY KEY (e, a, v, tx)
);

-- Boolean
CREATE TABLE datoms_bool (
  e            BIGINT NOT NULL,
  a            BIGINT NOT NULL,
  v            BOOLEAN NOT NULL,
  tx           BIGINT NOT NULL,
  retracted_by BIGINT,
  CONSTRAINT datoms_bool_pk PRIMARY KEY (e, a, v, tx)
);

-- Timestamp with timezone
CREATE TABLE datoms_timestamptz (
  e            BIGINT NOT NULL,
  a            BIGINT NOT NULL,
  v            TIMESTAMPTZ NOT NULL,
  tx           BIGINT NOT NULL,
  retracted_by BIGINT,
  CONSTRAINT datoms_timestamptz_pk PRIMARY KEY (e, a, v, tx)
);

-- Date
CREATE TABLE datoms_date (
  e            BIGINT NOT NULL,
  a            BIGINT NOT NULL,
  v            DATE NOT NULL,
  tx           BIGINT NOT NULL,
  retracted_by BIGINT,
  CONSTRAINT datoms_date_pk PRIMARY KEY (e, a, v, tx)
);

-- UUID
CREATE TABLE datoms_uuid (
  e            BIGINT NOT NULL,
  a            BIGINT NOT NULL,
  v            UUID NOT NULL,
  tx           BIGINT NOT NULL,
  retracted_by BIGINT,
  CONSTRAINT datoms_uuid_pk PRIMARY KEY (e, a, v, tx)
);

-- Binary data
CREATE TABLE datoms_bytea (
  e            BIGINT NOT NULL,
  a            BIGINT NOT NULL,
  v            BYTEA NOT NULL,
  tx           BIGINT NOT NULL,
  retracted_by BIGINT,
  CONSTRAINT datoms_bytea_pk PRIMARY KEY (e, a, v, tx)
);

-- JSON
CREATE TABLE datoms_jsonb (
  e            BIGINT NOT NULL,
  a            BIGINT NOT NULL,
  v            JSONB NOT NULL,
  tx           BIGINT NOT NULL,
  retracted_by BIGINT,
  CONSTRAINT datoms_jsonb_pk PRIMARY KEY (e, a, v, tx)
);

-- References (entity ID pointing to another entity)
CREATE TABLE datoms_ref (
  e            BIGINT NOT NULL,
  a            BIGINT NOT NULL,
  v            BIGINT NOT NULL,
  tx           BIGINT NOT NULL,
  retracted_by BIGINT,
  CONSTRAINT datoms_ref_pk PRIMARY KEY (e, a, v, tx)
);

--------------------------------------------------------------------------------
-- INDEXES
--------------------------------------------------------------------------------

-- AVET indexes (lookup by attribute + value, current only)
CREATE INDEX datoms_text_avet ON datoms_text (a, v, e, tx) WHERE retracted_by IS NULL;
CREATE INDEX datoms_int4_avet ON datoms_int4 (a, v, e, tx) WHERE retracted_by IS NULL;
CREATE INDEX datoms_int8_avet ON datoms_int8 (a, v, e, tx) WHERE retracted_by IS NULL;
CREATE INDEX datoms_float4_avet ON datoms_float4 (a, v, e, tx) WHERE retracted_by IS NULL;
CREATE INDEX datoms_float8_avet ON datoms_float8 (a, v, e, tx) WHERE retracted_by IS NULL;
CREATE INDEX datoms_numeric_avet ON datoms_numeric (a, v, e, tx) WHERE retracted_by IS NULL;
CREATE INDEX datoms_bool_avet ON datoms_bool (a, v, e, tx) WHERE retracted_by IS NULL;
CREATE INDEX datoms_timestamptz_avet ON datoms_timestamptz (a, v, e, tx) WHERE retracted_by IS NULL;
CREATE INDEX datoms_date_avet ON datoms_date (a, v, e, tx) WHERE retracted_by IS NULL;
CREATE INDEX datoms_uuid_avet ON datoms_uuid (a, v, e, tx) WHERE retracted_by IS NULL;
CREATE INDEX datoms_jsonb_avet ON datoms_jsonb (a, v, e, tx) WHERE retracted_by IS NULL;
CREATE INDEX datoms_ref_avet ON datoms_ref (a, v, e, tx) WHERE retracted_by IS NULL;

-- VAET index (reverse reference lookup, refs only)
CREATE INDEX datoms_ref_vaet ON datoms_ref (v, a, e, tx) WHERE retracted_by IS NULL;

-- Transaction indexes (for temporal queries)
CREATE INDEX datoms_text_tx ON datoms_text (tx);
CREATE INDEX datoms_int4_tx ON datoms_int4 (tx);
CREATE INDEX datoms_int8_tx ON datoms_int8 (tx);
CREATE INDEX datoms_float4_tx ON datoms_float4 (tx);
CREATE INDEX datoms_float8_tx ON datoms_float8 (tx);
CREATE INDEX datoms_numeric_tx ON datoms_numeric (tx);
CREATE INDEX datoms_bool_tx ON datoms_bool (tx);
CREATE INDEX datoms_timestamptz_tx ON datoms_timestamptz (tx);
CREATE INDEX datoms_date_tx ON datoms_date (tx);
CREATE INDEX datoms_uuid_tx ON datoms_uuid (tx);
CREATE INDEX datoms_bytea_tx ON datoms_bytea (tx);
CREATE INDEX datoms_jsonb_tx ON datoms_jsonb (tx);
CREATE INDEX datoms_ref_tx ON datoms_ref (tx);

--------------------------------------------------------------------------------
-- CORE FUNCTIONS
--------------------------------------------------------------------------------

-- Allocate a new entity ID from a partition
CREATE FUNCTION mnemonic_allocate_entity(partition_ident TEXT)
RETURNS BIGINT
LANGUAGE plpgsql
AS $$
DECLARE
  p RECORD;
  new_id BIGINT;
BEGIN
  SELECT * INTO p FROM partitions WHERE ident = partition_ident FOR UPDATE;
  IF NOT FOUND THEN
    RAISE EXCEPTION 'Unknown partition: %', partition_ident;
  END IF;
  new_id := p.next_id;
  UPDATE partitions SET next_id = next_id + 1 WHERE id = p.id;
  -- Encode partition in high 16 bits
  RETURN (p.id::BIGINT << 48) | new_id;
END;
$$;

-- Create a new transaction
CREATE FUNCTION mnemonic_new_transaction()
RETURNS BIGINT
LANGUAGE plpgsql
AS $$
DECLARE
  tx_id BIGINT;
BEGIN
  tx_id := mnemonic_allocate_entity('tx');
  INSERT INTO transactions (id, tx_instant) VALUES (tx_id, NOW());
  RETURN tx_id;
END;
$$;

-- Lookup attribute ID by ident (uses bootstrap attr ID for db/ident)
CREATE FUNCTION mnemonic_attr_id(ident TEXT)
RETURNS BIGINT
LANGUAGE sql
STABLE
AS $$
  SELECT e FROM datoms_text
  WHERE a = 1  -- db/ident attribute has entity ID 1
    AND v = ident
    AND retracted_by IS NULL
  LIMIT 1;
$$;

-- Get the table name for a value type
CREATE FUNCTION mnemonic_value_type_table(type_ident TEXT)
RETURNS TEXT
LANGUAGE sql
IMMUTABLE
AS $$
  SELECT CASE type_ident
    WHEN 'db.type/text' THEN 'datoms_text'
    WHEN 'db.type/int4' THEN 'datoms_int4'
    WHEN 'db.type/int8' THEN 'datoms_int8'
    WHEN 'db.type/float4' THEN 'datoms_float4'
    WHEN 'db.type/float8' THEN 'datoms_float8'
    WHEN 'db.type/numeric' THEN 'datoms_numeric'
    WHEN 'db.type/bool' THEN 'datoms_bool'
    WHEN 'db.type/timestamptz' THEN 'datoms_timestamptz'
    WHEN 'db.type/date' THEN 'datoms_date'
    WHEN 'db.type/uuid' THEN 'datoms_uuid'
    WHEN 'db.type/bytea' THEN 'datoms_bytea'
    WHEN 'db.type/jsonb' THEN 'datoms_jsonb'
    WHEN 'db.type/ref' THEN 'datoms_ref'
    ELSE NULL
  END;
$$;

--------------------------------------------------------------------------------
-- BOOTSTRAP SYSTEM SCHEMA
--------------------------------------------------------------------------------

-- Bootstrap transaction (tx = 0, special case)
INSERT INTO transactions (id, tx_instant) VALUES (0, NOW());

-- We need to manually bootstrap the core attributes since mnemonic_attr_id
-- depends on db/ident existing. Entity IDs in partition 0 (db):
--   1 = db/ident
--   2 = db/valueType
--   3 = db/cardinality
--   4 = db/unique
--   5 = db/doc

-- db/ident (entity 1) - identifies attributes by keyword
INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES (1, 1, 'db/ident', 0, NULL);
INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES (1, 2, 100, 0, NULL);  -- valueType = text
INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES (1, 3, 200, 0, NULL);  -- cardinality = one

-- db/valueType (entity 2) - the value type of an attribute
INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES (2, 1, 'db/valueType', 0, NULL);
INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES (2, 2, 110, 0, NULL);  -- valueType = ref
INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES (2, 3, 200, 0, NULL);  -- cardinality = one

-- db/cardinality (entity 3) - one or many
INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES (3, 1, 'db/cardinality', 0, NULL);
INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES (3, 2, 110, 0, NULL);  -- valueType = ref
INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES (3, 3, 200, 0, NULL);  -- cardinality = one

-- db/unique (entity 4) - uniqueness constraint
INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES (4, 1, 'db/unique', 0, NULL);
INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES (4, 2, 110, 0, NULL);  -- valueType = ref
INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES (4, 3, 200, 0, NULL);  -- cardinality = one

-- db/doc (entity 5) - documentation string
INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES (5, 1, 'db/doc', 0, NULL);
INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES (5, 2, 100, 0, NULL);  -- valueType = text
INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES (5, 3, 200, 0, NULL);  -- cardinality = one

--------------------------------------------------------------------------------
-- VALUE TYPE ENTITIES (100-112)
--------------------------------------------------------------------------------

INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES
  (100, 1, 'db.type/text', 0, NULL),
  (101, 1, 'db.type/int4', 0, NULL),
  (102, 1, 'db.type/int8', 0, NULL),
  (103, 1, 'db.type/float4', 0, NULL),
  (104, 1, 'db.type/float8', 0, NULL),
  (105, 1, 'db.type/numeric', 0, NULL),
  (106, 1, 'db.type/bool', 0, NULL),
  (107, 1, 'db.type/timestamptz', 0, NULL),
  (108, 1, 'db.type/date', 0, NULL),
  (109, 1, 'db.type/uuid', 0, NULL),
  (110, 1, 'db.type/bytea', 0, NULL),
  (111, 1, 'db.type/jsonb', 0, NULL),
  (112, 1, 'db.type/ref', 0, NULL);

--------------------------------------------------------------------------------
-- CARDINALITY ENTITIES (200-201)
--------------------------------------------------------------------------------

INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES
  (200, 1, 'db.cardinality/one', 0, NULL),
  (201, 1, 'db.cardinality/many', 0, NULL);

--------------------------------------------------------------------------------
-- UNIQUENESS ENTITIES (210-211)
--------------------------------------------------------------------------------

INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES
  (210, 1, 'db.unique/identity', 0, NULL),
  (211, 1, 'db.unique/value', 0, NULL);

--------------------------------------------------------------------------------
-- VIEW DEFINITION ATTRIBUTES (10-12)
--------------------------------------------------------------------------------

-- db.view/ident (entity 10) - view name
INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES (10, 1, 'db.view/ident', 0, NULL);
INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES (10, 2, 100, 0, NULL);  -- text
INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES (10, 3, 200, 0, NULL);  -- one

-- db.view/attributes (entity 11) - refs to attributes in the view
INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES (11, 1, 'db.view/attributes', 0, NULL);
INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES (11, 2, 112, 0, NULL);  -- ref
INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES (11, 3, 201, 0, NULL);  -- many

-- db.view/doc (entity 12) - view documentation
INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES (12, 1, 'db.view/doc', 0, NULL);
INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES (12, 2, 100, 0, NULL);  -- text
INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES (12, 3, 200, 0, NULL);  -- one

-- db.view/optional-attributes (entity 13) - refs to optional attributes (LEFT JOIN)
INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES (13, 1, 'db.view/optional-attributes', 0, NULL);
INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES (13, 2, 112, 0, NULL);  -- ref
INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES (13, 3, 201, 0, NULL);  -- many

--------------------------------------------------------------------------------
-- UPDATE PARTITION COUNTERS
--------------------------------------------------------------------------------

-- Set next_id for db partition (we used 1-5, 10-13, 100-112, 200-201, 210-211)
UPDATE partitions SET next_id = 300 WHERE ident = 'db';

-- Set next_id for tx partition (we used 0 for bootstrap)
UPDATE partitions SET next_id = 1 WHERE ident = 'tx';

--------------------------------------------------------------------------------
-- HELPER VIEWS FOR SCHEMA INTROSPECTION
--------------------------------------------------------------------------------

-- View all defined attributes
CREATE VIEW mnemonic_attributes AS
SELECT
  ident.e AS id,
  ident.v AS ident,
  vtype_ident.v AS value_type,
  card_ident.v AS cardinality,
  uniq_ident.v AS unique_constraint,
  doc.v AS doc
FROM datoms_text ident
JOIN datoms_ref vtype ON ident.e = vtype.e AND vtype.a = 2 AND vtype.retracted_by IS NULL
JOIN datoms_text vtype_ident ON vtype.v = vtype_ident.e AND vtype_ident.a = 1 AND vtype_ident.retracted_by IS NULL
JOIN datoms_ref card ON ident.e = card.e AND card.a = 3 AND card.retracted_by IS NULL
JOIN datoms_text card_ident ON card.v = card_ident.e AND card_ident.a = 1 AND card_ident.retracted_by IS NULL
LEFT JOIN datoms_ref uniq ON ident.e = uniq.e AND uniq.a = 4 AND uniq.retracted_by IS NULL
LEFT JOIN datoms_text uniq_ident ON uniq.v = uniq_ident.e AND uniq_ident.a = 1 AND uniq_ident.retracted_by IS NULL
LEFT JOIN datoms_text doc ON ident.e = doc.e AND doc.a = 5 AND doc.retracted_by IS NULL
WHERE ident.a = 1
  AND ident.retracted_by IS NULL
  AND ident.v LIKE '%/%'  -- Only namespaced idents (attributes)
  AND ident.v NOT LIKE 'db.%';  -- Exclude system idents

-- View all defined views
CREATE VIEW mnemonic_views AS
SELECT
  ident.e AS id,
  ident.v AS name,
  doc.v AS doc
FROM datoms_text ident
LEFT JOIN datoms_text doc ON ident.e = doc.e AND doc.a = 12 AND doc.retracted_by IS NULL
WHERE ident.a = 10
  AND ident.retracted_by IS NULL;

-- View attributes in each view (with full attribute metadata)
-- Combines required attributes (entity 11) and optional attributes (entity 13)
CREATE VIEW mnemonic_view_attributes AS
-- Required attributes (db.view/attributes - entity 11)
SELECT
  view_ident.v AS view_name,
  attr_ident.v AS attribute_ident,
  attr_ident.e AS attribute_id,
  vtype_ident.v AS value_type,
  card_ident.v AS cardinality,
  true AS required
FROM datoms_text view_ident
JOIN datoms_ref view_attrs ON view_ident.e = view_attrs.e AND view_attrs.a = 11 AND view_attrs.retracted_by IS NULL
JOIN datoms_text attr_ident ON view_attrs.v = attr_ident.e AND attr_ident.a = 1 AND attr_ident.retracted_by IS NULL
JOIN datoms_ref vtype ON attr_ident.e = vtype.e AND vtype.a = 2 AND vtype.retracted_by IS NULL
JOIN datoms_text vtype_ident ON vtype.v = vtype_ident.e AND vtype_ident.a = 1 AND vtype_ident.retracted_by IS NULL
JOIN datoms_ref card ON attr_ident.e = card.e AND card.a = 3 AND card.retracted_by IS NULL
JOIN datoms_text card_ident ON card.v = card_ident.e AND card_ident.a = 1 AND card_ident.retracted_by IS NULL
WHERE view_ident.a = 10
  AND view_ident.retracted_by IS NULL
UNION ALL
-- Optional attributes (db.view/optional-attributes - entity 13)
SELECT
  view_ident.v AS view_name,
  attr_ident.v AS attribute_ident,
  attr_ident.e AS attribute_id,
  vtype_ident.v AS value_type,
  card_ident.v AS cardinality,
  false AS required
FROM datoms_text view_ident
JOIN datoms_ref view_attrs ON view_ident.e = view_attrs.e AND view_attrs.a = 13 AND view_attrs.retracted_by IS NULL
JOIN datoms_text attr_ident ON view_attrs.v = attr_ident.e AND attr_ident.a = 1 AND attr_ident.retracted_by IS NULL
JOIN datoms_ref vtype ON attr_ident.e = vtype.e AND vtype.a = 2 AND vtype.retracted_by IS NULL
JOIN datoms_text vtype_ident ON vtype.v = vtype_ident.e AND vtype_ident.a = 1 AND vtype_ident.retracted_by IS NULL
JOIN datoms_ref card ON attr_ident.e = card.e AND card.a = 3 AND card.retracted_by IS NULL
JOIN datoms_text card_ident ON card.v = card_ident.e AND card_ident.a = 1 AND card_ident.retracted_by IS NULL
WHERE view_ident.a = 10
  AND view_ident.retracted_by IS NULL;

--------------------------------------------------------------------------------
-- VIEW REGENERATION
--------------------------------------------------------------------------------

-- Convert attribute ident to a valid SQL column name
CREATE FUNCTION mnemonic_attr_to_column(attr_ident TEXT)
RETURNS TEXT
LANGUAGE sql
IMMUTABLE
AS $$
  -- Extract part after '/' and replace '-' with '_'
  SELECT REPLACE(SPLIT_PART(attr_ident, '/', 2), '-', '_');
$$;

-- Get PostgreSQL type for a value type ident
CREATE FUNCTION mnemonic_pg_type(value_type TEXT)
RETURNS TEXT
LANGUAGE sql
IMMUTABLE
AS $$
  SELECT CASE value_type
    WHEN 'db.type/text' THEN 'TEXT'
    WHEN 'db.type/int4' THEN 'INTEGER'
    WHEN 'db.type/int8' THEN 'BIGINT'
    WHEN 'db.type/float4' THEN 'REAL'
    WHEN 'db.type/float8' THEN 'DOUBLE PRECISION'
    WHEN 'db.type/numeric' THEN 'NUMERIC'
    WHEN 'db.type/bool' THEN 'BOOLEAN'
    WHEN 'db.type/timestamptz' THEN 'TIMESTAMPTZ'
    WHEN 'db.type/date' THEN 'DATE'
    WHEN 'db.type/uuid' THEN 'UUID'
    WHEN 'db.type/bytea' THEN 'BYTEA'
    WHEN 'db.type/jsonb' THEN 'JSONB'
    WHEN 'db.type/ref' THEN 'BIGINT'
    ELSE 'TEXT'
  END;
$$;

-- Regenerate a single view using join-based approach:
-- First required attribute = base table
-- Other required attributes = INNER JOIN
-- Optional attributes = LEFT JOIN
CREATE PROCEDURE mnemonic_regenerate_view(p_view_name TEXT)
LANGUAGE plpgsql
AS $$
DECLARE
  attr RECORD;
  select_cols TEXT;
  from_clause TEXT;
  where_clause TEXT;
  join_idx INTEGER := 0;
  col_name TEXT;
  table_name TEXT;
  alias TEXT;
  join_type TEXT;
  is_first BOOLEAN := true;
  base_alias TEXT;
BEGIN
  -- Drop existing view and triggers if they exist
  EXECUTE format('DROP VIEW IF EXISTS %I CASCADE', p_view_name);

  -- Process attributes: required first (ordered), then optional
  FOR attr IN
    SELECT * FROM mnemonic_view_attributes
    WHERE view_name = p_view_name
    ORDER BY required DESC, attribute_ident  -- required first, then by name
  LOOP
    join_idx := join_idx + 1;
    alias := 'j' || join_idx;
    col_name := mnemonic_attr_to_column(attr.attribute_ident);
    table_name := mnemonic_value_type_table(attr.value_type);

    IF is_first THEN
      -- First required attribute is the base table
      base_alias := alias;
      select_cols := format('%I.e AS id', alias);
      from_clause := format('%I %I', table_name, alias);
      where_clause := format('%I.a = %s AND %I.retracted_by IS NULL',
        alias, attr.attribute_id, alias);
      is_first := false;

      IF attr.cardinality = 'db.cardinality/one' THEN
        select_cols := select_cols || format(', %I.v AS %I', alias, col_name);
      ELSE
        -- Cardinality many for base - need lateral
        select_cols := select_cols || format(', %I_arr.v AS %I', alias, col_name);
        from_clause := from_clause || format(
          ' LEFT JOIN LATERAL (SELECT ARRAY_AGG(v) AS v FROM %I WHERE e = %I.e AND a = %s AND retracted_by IS NULL) %I_arr ON true',
          table_name, base_alias, attr.attribute_id, alias
        );
      END IF;
    ELSE
      -- Subsequent attributes: INNER JOIN for required, LEFT JOIN for optional
      join_type := CASE WHEN attr.required THEN 'INNER JOIN' ELSE 'LEFT JOIN' END;

      IF attr.cardinality = 'db.cardinality/one' THEN
        select_cols := select_cols || format(', %I.v AS %I', alias, col_name);
        from_clause := from_clause || format(
          ' %s %I %I ON %I.e = %I.e AND %I.a = %s AND %I.retracted_by IS NULL',
          join_type, table_name, alias, base_alias, alias, alias, attr.attribute_id, alias
        );
      ELSE
        -- Cardinality many: use lateral subquery for array aggregation
        select_cols := select_cols || format(', %I.v AS %I', alias, col_name);
        from_clause := from_clause || format(
          ' LEFT JOIN LATERAL (SELECT ARRAY_AGG(v) AS v FROM %I WHERE e = %I.e AND a = %s AND retracted_by IS NULL) %I ON true',
          table_name, base_alias, attr.attribute_id, alias
        );
      END IF;
    END IF;
  END LOOP;

  -- If no attributes, nothing to do
  IF join_idx = 0 THEN
    RAISE NOTICE 'View % has no attributes, skipping', p_view_name;
    RETURN;
  END IF;

  -- Create the view
  EXECUTE format(
    'CREATE VIEW %I AS SELECT %s FROM %s WHERE %s',
    p_view_name,
    select_cols,
    from_clause,
    where_clause
  );

  -- Create triggers
  EXECUTE format(
    'CREATE TRIGGER %I_insert INSTEAD OF INSERT ON %I FOR EACH ROW EXECUTE FUNCTION mnemonic_view_insert(%L)',
    p_view_name, p_view_name, p_view_name
  );

  EXECUTE format(
    'CREATE TRIGGER %I_update INSTEAD OF UPDATE ON %I FOR EACH ROW EXECUTE FUNCTION mnemonic_view_update(%L)',
    p_view_name, p_view_name, p_view_name
  );

  EXECUTE format(
    'CREATE TRIGGER %I_delete INSTEAD OF DELETE ON %I FOR EACH ROW EXECUTE FUNCTION mnemonic_view_delete(%L)',
    p_view_name, p_view_name, p_view_name
  );
END;
$$;

-- Regenerate all views
CREATE PROCEDURE mnemonic_regenerate_views()
LANGUAGE plpgsql
AS $$
DECLARE
  view_rec RECORD;
BEGIN
  FOR view_rec IN SELECT DISTINCT name FROM mnemonic_views
  LOOP
    CALL mnemonic_regenerate_view(view_rec.name);
  END LOOP;
END;
$$;

--------------------------------------------------------------------------------
-- TRIGGER FUNCTIONS FOR VIEW DML
--------------------------------------------------------------------------------

-- INSERT trigger: creates a new entity and asserts datoms for each column
CREATE FUNCTION mnemonic_view_insert()
RETURNS TRIGGER
LANGUAGE plpgsql
AS $$
DECLARE
  p_view_name TEXT := TG_ARGV[0];
  new_entity BIGINT;
  tx_id BIGINT;
  attr RECORD;
  col_name TEXT;
  col_value TEXT;
  table_name TEXT;
BEGIN
  -- Allocate new entity and transaction
  new_entity := mnemonic_allocate_entity('user');
  tx_id := mnemonic_new_transaction();

  -- Insert datom for each attribute
  FOR attr IN SELECT * FROM mnemonic_view_attributes WHERE view_name = p_view_name
  LOOP
    col_name := mnemonic_attr_to_column(attr.attribute_ident);
    table_name := mnemonic_value_type_table(attr.value_type);

    -- Get column value from NEW using dynamic SQL
    EXECUTE format('SELECT ($1).%I::TEXT', col_name) INTO col_value USING NEW;

    IF col_value IS NOT NULL THEN
      -- Insert the datom with appropriate type casting
      EXECUTE format(
        'INSERT INTO %I (e, a, v, tx, retracted_by) VALUES ($1, $2, $3::%s, $4, NULL)',
        table_name,
        mnemonic_pg_type(attr.value_type)
      ) USING new_entity, attr.attribute_id, col_value, tx_id;
    END IF;
  END LOOP;

  -- Return NEW with the allocated ID
  NEW.id := new_entity;
  RETURN NEW;
END;
$$;

-- UPDATE trigger: retracts changed values and asserts new ones
CREATE FUNCTION mnemonic_view_update()
RETURNS TRIGGER
LANGUAGE plpgsql
AS $$
DECLARE
  p_view_name TEXT := TG_ARGV[0];
  tx_id BIGINT;
  attr RECORD;
  col_name TEXT;
  old_value TEXT;
  new_value TEXT;
  table_name TEXT;
BEGIN
  tx_id := mnemonic_new_transaction();

  FOR attr IN SELECT * FROM mnemonic_view_attributes WHERE view_name = p_view_name
  LOOP
    col_name := mnemonic_attr_to_column(attr.attribute_ident);
    table_name := mnemonic_value_type_table(attr.value_type);

    -- Get old and new values
    EXECUTE format('SELECT ($1).%I::TEXT', col_name) INTO old_value USING OLD;
    EXECUTE format('SELECT ($1).%I::TEXT', col_name) INTO new_value USING NEW;

    -- Only process if value changed
    IF old_value IS DISTINCT FROM new_value THEN
      -- Retract old value if it existed
      IF old_value IS NOT NULL THEN
        EXECUTE format(
          'UPDATE %I SET retracted_by = $1 WHERE e = $2 AND a = $3 AND retracted_by IS NULL',
          table_name
        ) USING tx_id, OLD.id, attr.attribute_id;
      END IF;

      -- Assert new value if not null
      IF new_value IS NOT NULL THEN
        EXECUTE format(
          'INSERT INTO %I (e, a, v, tx, retracted_by) VALUES ($1, $2, $3::%s, $4, NULL)',
          table_name,
          mnemonic_pg_type(attr.value_type)
        ) USING OLD.id, attr.attribute_id, new_value, tx_id;
      END IF;
    END IF;
  END LOOP;

  RETURN NEW;
END;
$$;

-- DELETE trigger: retracts all datoms for attributes in the view
CREATE FUNCTION mnemonic_view_delete()
RETURNS TRIGGER
LANGUAGE plpgsql
AS $$
DECLARE
  p_view_name TEXT := TG_ARGV[0];
  tx_id BIGINT;
  attr RECORD;
  table_name TEXT;
BEGIN
  tx_id := mnemonic_new_transaction();

  FOR attr IN SELECT * FROM mnemonic_view_attributes WHERE view_name = p_view_name
  LOOP
    table_name := mnemonic_value_type_table(attr.value_type);

    -- Retract all current datoms for this entity/attribute
    EXECUTE format(
      'UPDATE %I SET retracted_by = $1 WHERE e = $2 AND a = $3 AND retracted_by IS NULL',
      table_name
    ) USING tx_id, OLD.id, attr.attribute_id;
  END LOOP;

  RETURN OLD;
END;
$$;

--------------------------------------------------------------------------------
-- SELF-MANAGING VIEW DEFINITIONS
--------------------------------------------------------------------------------

-- Admin view showing view definitions with required and optional attributes
CREATE VIEW mnemonic_defined_views AS
SELECT
  v.id,
  v.name,
  v.doc,
  COALESCE(req.attributes, '{}') AS attributes,
  COALESCE(opt.attributes, '{}') AS optional_attributes
FROM mnemonic_views v
LEFT JOIN LATERAL (
  SELECT ARRAY_AGG(va.attribute_ident ORDER BY va.attribute_ident) AS attributes
  FROM mnemonic_view_attributes va
  WHERE va.view_name = v.name AND va.required = true
) req ON true
LEFT JOIN LATERAL (
  SELECT ARRAY_AGG(va.attribute_ident ORDER BY va.attribute_ident) AS attributes
  FROM mnemonic_view_attributes va
  WHERE va.view_name = v.name AND va.required = false
) opt ON true;

-- INSERT trigger for mnemonic_defined_views
CREATE FUNCTION mnemonic_defined_views_insert()
RETURNS TRIGGER
LANGUAGE plpgsql
AS $$
DECLARE
  tx_id BIGINT;
  view_id BIGINT;
  attr_ident TEXT;
  attr_id BIGINT;
BEGIN
  -- Validate: at least one required attribute
  IF NEW.attributes IS NULL OR array_length(NEW.attributes, 1) IS NULL THEN
    RAISE EXCEPTION 'View must have at least one required attribute';
  END IF;

  -- Allocate view entity and transaction
  view_id := mnemonic_allocate_entity('db');
  tx_id := mnemonic_new_transaction();

  -- Assert view ident
  INSERT INTO datoms_text (e, a, v, tx, retracted_by)
  VALUES (view_id, 10, NEW.name, tx_id, NULL);

  -- Assert required attribute refs (entity 11)
  FOREACH attr_ident IN ARRAY NEW.attributes
  LOOP
    attr_id := mnemonic_attr_id(attr_ident);
    IF attr_id IS NULL THEN
      RAISE EXCEPTION 'Unknown attribute: %', attr_ident;
    END IF;
    INSERT INTO datoms_ref (e, a, v, tx, retracted_by)
    VALUES (view_id, 11, attr_id, tx_id, NULL);
  END LOOP;

  -- Assert optional attribute refs (entity 13)
  IF NEW.optional_attributes IS NOT NULL AND array_length(NEW.optional_attributes, 1) IS NOT NULL THEN
    FOREACH attr_ident IN ARRAY NEW.optional_attributes
    LOOP
      attr_id := mnemonic_attr_id(attr_ident);
      IF attr_id IS NULL THEN
        RAISE EXCEPTION 'Unknown attribute: %', attr_ident;
      END IF;
      INSERT INTO datoms_ref (e, a, v, tx, retracted_by)
      VALUES (view_id, 13, attr_id, tx_id, NULL);
    END LOOP;
  END IF;

  -- Assert doc if provided
  IF NEW.doc IS NOT NULL THEN
    INSERT INTO datoms_text (e, a, v, tx, retracted_by)
    VALUES (view_id, 12, NEW.doc, tx_id, NULL);
  END IF;

  -- Regenerate the SQL view
  CALL mnemonic_regenerate_view(NEW.name);

  NEW.id := view_id;
  RETURN NEW;
END;
$$;

-- UPDATE trigger for mnemonic_defined_views
CREATE FUNCTION mnemonic_defined_views_update()
RETURNS TRIGGER
LANGUAGE plpgsql
AS $$
DECLARE
  tx_id BIGINT;
  attr_ident TEXT;
  attr_id BIGINT;
BEGIN
  -- Validate: at least one required attribute
  IF NEW.attributes IS NULL OR array_length(NEW.attributes, 1) IS NULL THEN
    RAISE EXCEPTION 'View must have at least one required attribute';
  END IF;

  tx_id := mnemonic_new_transaction();

  -- If name changed, retract old and assert new
  IF NEW.name IS DISTINCT FROM OLD.name THEN
    UPDATE datoms_text SET retracted_by = tx_id
    WHERE e = OLD.id AND a = 10 AND retracted_by IS NULL;

    INSERT INTO datoms_text (e, a, v, tx, retracted_by)
    VALUES (OLD.id, 10, NEW.name, tx_id, NULL);

    -- Drop the old SQL view
    EXECUTE format('DROP VIEW IF EXISTS %I CASCADE', OLD.name);
  END IF;

  -- If required attributes changed
  IF NEW.attributes IS DISTINCT FROM OLD.attributes THEN
    UPDATE datoms_ref SET retracted_by = tx_id
    WHERE e = OLD.id AND a = 11 AND retracted_by IS NULL;

    FOREACH attr_ident IN ARRAY NEW.attributes
    LOOP
      attr_id := mnemonic_attr_id(attr_ident);
      IF attr_id IS NULL THEN
        RAISE EXCEPTION 'Unknown attribute: %', attr_ident;
      END IF;
      INSERT INTO datoms_ref (e, a, v, tx, retracted_by)
      VALUES (OLD.id, 11, attr_id, tx_id, NULL);
    END LOOP;
  END IF;

  -- If optional attributes changed
  IF NEW.optional_attributes IS DISTINCT FROM OLD.optional_attributes THEN
    UPDATE datoms_ref SET retracted_by = tx_id
    WHERE e = OLD.id AND a = 13 AND retracted_by IS NULL;

    IF NEW.optional_attributes IS NOT NULL AND array_length(NEW.optional_attributes, 1) IS NOT NULL THEN
      FOREACH attr_ident IN ARRAY NEW.optional_attributes
      LOOP
        attr_id := mnemonic_attr_id(attr_ident);
        IF attr_id IS NULL THEN
          RAISE EXCEPTION 'Unknown attribute: %', attr_ident;
        END IF;
        INSERT INTO datoms_ref (e, a, v, tx, retracted_by)
        VALUES (OLD.id, 13, attr_id, tx_id, NULL);
      END LOOP;
    END IF;
  END IF;

  -- If doc changed
  IF NEW.doc IS DISTINCT FROM OLD.doc THEN
    UPDATE datoms_text SET retracted_by = tx_id
    WHERE e = OLD.id AND a = 12 AND retracted_by IS NULL;

    IF NEW.doc IS NOT NULL THEN
      INSERT INTO datoms_text (e, a, v, tx, retracted_by)
      VALUES (OLD.id, 12, NEW.doc, tx_id, NULL);
    END IF;
  END IF;

  -- Regenerate the SQL view
  CALL mnemonic_regenerate_view(NEW.name);

  RETURN NEW;
END;
$$;

-- DELETE trigger for mnemonic_defined_views
CREATE FUNCTION mnemonic_defined_views_delete()
RETURNS TRIGGER
LANGUAGE plpgsql
AS $$
DECLARE
  tx_id BIGINT;
BEGIN
  tx_id := mnemonic_new_transaction();

  -- Retract view ident
  UPDATE datoms_text SET retracted_by = tx_id
  WHERE e = OLD.id AND a = 10 AND retracted_by IS NULL;

  -- Retract required attribute refs
  UPDATE datoms_ref SET retracted_by = tx_id
  WHERE e = OLD.id AND a = 11 AND retracted_by IS NULL;

  -- Retract optional attribute refs
  UPDATE datoms_ref SET retracted_by = tx_id
  WHERE e = OLD.id AND a = 13 AND retracted_by IS NULL;

  -- Retract doc
  UPDATE datoms_text SET retracted_by = tx_id
  WHERE e = OLD.id AND a = 12 AND retracted_by IS NULL;

  -- Drop the SQL view
  EXECUTE format('DROP VIEW IF EXISTS %I CASCADE', OLD.name);

  RETURN OLD;
END;
$$;

-- Create triggers on mnemonic_defined_views
CREATE TRIGGER mnemonic_defined_views_insert_trigger
  INSTEAD OF INSERT ON mnemonic_defined_views
  FOR EACH ROW EXECUTE FUNCTION mnemonic_defined_views_insert();

CREATE TRIGGER mnemonic_defined_views_update_trigger
  INSTEAD OF UPDATE ON mnemonic_defined_views
  FOR EACH ROW EXECUTE FUNCTION mnemonic_defined_views_update();

CREATE TRIGGER mnemonic_defined_views_delete_trigger
  INSTEAD OF DELETE ON mnemonic_defined_views
  FOR EACH ROW EXECUTE FUNCTION mnemonic_defined_views_delete();
