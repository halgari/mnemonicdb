-- MnemonicDB Bootstrap Schema
-- Initializes the database with system tables, functions, and schema
--
-- NEW ARCHITECTURE: Unified JSONB storage with per-attribute inherited tables
-- - Single parent table `datoms` stores all data with v_data JSONB
-- - Each attribute gets an inherited table with typed `v` generated column
-- - Value indexes live on attribute tables, not parent

--------------------------------------------------------------------------------
-- QUERY PLANNER CONFIGURATION
--------------------------------------------------------------------------------

SET random_page_cost = 1.1;
SET enable_bitmapscan = off;

--------------------------------------------------------------------------------
-- PARTITIONS
--------------------------------------------------------------------------------

CREATE TABLE partitions (
  id        INTEGER PRIMARY KEY,
  ident     TEXT UNIQUE NOT NULL,
  next_id   BIGINT NOT NULL DEFAULT 1
);

INSERT INTO partitions (id, ident, next_id) VALUES
  (0, 'db', 1),
  (1, 'tx', 1),
  (2, 'user', 1);

--------------------------------------------------------------------------------
-- TRANSACTIONS TABLE
--------------------------------------------------------------------------------

CREATE TABLE transactions (
  id          BIGINT PRIMARY KEY,
  tx_instant  TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

--------------------------------------------------------------------------------
-- UNIFIED DATOMS TABLE (parent for all attribute tables)
--------------------------------------------------------------------------------

CREATE TABLE datoms (
  e            BIGINT NOT NULL,
  a            BIGINT NOT NULL,
  v_data       JSONB NOT NULL,
  tx           BIGINT NOT NULL,
  retracted_by BIGINT,
  CONSTRAINT datoms_pk PRIMARY KEY (e, a, v_data, tx)
);

CREATE INDEX datoms_e ON datoms (e) WHERE retracted_by IS NULL;
CREATE INDEX datoms_tx ON datoms (tx);

--------------------------------------------------------------------------------
-- SYSTEM ATTRIBUTE TABLES (created before functions that reference them)
--------------------------------------------------------------------------------

-- attr_db_ident: stores db/ident values (text) - attribute ID 1
CREATE TABLE attr_db_ident (
  v TEXT GENERATED ALWAYS AS ((v_data #>> '{}')) STORED,
  CHECK (a = 1)
) INHERITS (datoms);
CREATE INDEX attr_db_ident_v ON attr_db_ident (v) WHERE retracted_by IS NULL;
CREATE INDEX attr_db_ident_e ON attr_db_ident (e) WHERE retracted_by IS NULL;

-- attr_db_valuetype: stores db/valueType values (ref) - attribute ID 2
CREATE TABLE attr_db_valuetype (
  v BIGINT GENERATED ALWAYS AS ((v_data #>> '{}')::bigint) STORED,
  CHECK (a = 2)
) INHERITS (datoms);
CREATE INDEX attr_db_valuetype_v ON attr_db_valuetype (v) WHERE retracted_by IS NULL;
CREATE INDEX attr_db_valuetype_e ON attr_db_valuetype (e) WHERE retracted_by IS NULL;
CREATE INDEX attr_db_valuetype_vaet ON attr_db_valuetype (v, e) WHERE retracted_by IS NULL;

-- attr_db_cardinality: stores db/cardinality values (ref) - attribute ID 3
CREATE TABLE attr_db_cardinality (
  v BIGINT GENERATED ALWAYS AS ((v_data #>> '{}')::bigint) STORED,
  CHECK (a = 3)
) INHERITS (datoms);
CREATE INDEX attr_db_cardinality_v ON attr_db_cardinality (v) WHERE retracted_by IS NULL;
CREATE INDEX attr_db_cardinality_e ON attr_db_cardinality (e) WHERE retracted_by IS NULL;
CREATE INDEX attr_db_cardinality_vaet ON attr_db_cardinality (v, e) WHERE retracted_by IS NULL;

-- attr_db_unique: stores db/unique values (ref) - attribute ID 4
CREATE TABLE attr_db_unique (
  v BIGINT GENERATED ALWAYS AS ((v_data #>> '{}')::bigint) STORED,
  CHECK (a = 4)
) INHERITS (datoms);
CREATE INDEX attr_db_unique_v ON attr_db_unique (v) WHERE retracted_by IS NULL;
CREATE INDEX attr_db_unique_e ON attr_db_unique (e) WHERE retracted_by IS NULL;
CREATE INDEX attr_db_unique_vaet ON attr_db_unique (v, e) WHERE retracted_by IS NULL;

-- attr_db_doc: stores db/doc values (text) - attribute ID 5
CREATE TABLE attr_db_doc (
  v TEXT GENERATED ALWAYS AS ((v_data #>> '{}')) STORED,
  CHECK (a = 5)
) INHERITS (datoms);
CREATE INDEX attr_db_doc_v ON attr_db_doc (v) WHERE retracted_by IS NULL;
CREATE INDEX attr_db_doc_e ON attr_db_doc (e) WHERE retracted_by IS NULL;

-- attr_db_view_ident: stores db.view/ident values (text) - attribute ID 10
CREATE TABLE attr_db_view_ident (
  v TEXT GENERATED ALWAYS AS ((v_data #>> '{}')) STORED,
  CHECK (a = 10)
) INHERITS (datoms);
CREATE INDEX attr_db_view_ident_v ON attr_db_view_ident (v) WHERE retracted_by IS NULL;
CREATE INDEX attr_db_view_ident_e ON attr_db_view_ident (e) WHERE retracted_by IS NULL;

-- attr_db_view_attributes: stores db.view/attributes values (ref, many) - attribute ID 11
CREATE TABLE attr_db_view_attributes (
  v BIGINT GENERATED ALWAYS AS ((v_data #>> '{}')::bigint) STORED,
  CHECK (a = 11)
) INHERITS (datoms);
CREATE INDEX attr_db_view_attributes_v ON attr_db_view_attributes (v) WHERE retracted_by IS NULL;
CREATE INDEX attr_db_view_attributes_e ON attr_db_view_attributes (e) WHERE retracted_by IS NULL;
CREATE INDEX attr_db_view_attributes_vaet ON attr_db_view_attributes (v, e) WHERE retracted_by IS NULL;

-- attr_db_view_doc: stores db.view/doc values (text) - attribute ID 12
CREATE TABLE attr_db_view_doc (
  v TEXT GENERATED ALWAYS AS ((v_data #>> '{}')) STORED,
  CHECK (a = 12)
) INHERITS (datoms);
CREATE INDEX attr_db_view_doc_v ON attr_db_view_doc (v) WHERE retracted_by IS NULL;
CREATE INDEX attr_db_view_doc_e ON attr_db_view_doc (e) WHERE retracted_by IS NULL;

-- attr_db_view_optional_attributes: stores db.view/optional-attributes (ref, many) - attribute ID 13
CREATE TABLE attr_db_view_optional_attributes (
  v BIGINT GENERATED ALWAYS AS ((v_data #>> '{}')::bigint) STORED,
  CHECK (a = 13)
) INHERITS (datoms);
CREATE INDEX attr_db_view_optional_attributes_v ON attr_db_view_optional_attributes (v) WHERE retracted_by IS NULL;
CREATE INDEX attr_db_view_optional_attributes_e ON attr_db_view_optional_attributes (e) WHERE retracted_by IS NULL;
CREATE INDEX attr_db_view_optional_attributes_vaet ON attr_db_view_optional_attributes (v, e) WHERE retracted_by IS NULL;

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

-- Convert value type ident to extraction expression
CREATE FUNCTION mnemonic_jsonb_extract_expr(value_type TEXT)
RETURNS TEXT
LANGUAGE sql
IMMUTABLE
AS $$
  SELECT CASE value_type
    WHEN 'db.type/text' THEN '(v_data #>> ''{}'')'
    WHEN 'db.type/int4' THEN '(v_data #>> ''{}'')::integer'
    WHEN 'db.type/int8' THEN '(v_data #>> ''{}'')::bigint'
    WHEN 'db.type/float4' THEN '(v_data #>> ''{}'')::real'
    WHEN 'db.type/float8' THEN '(v_data #>> ''{}'')::double precision'
    WHEN 'db.type/numeric' THEN '(v_data)::numeric'
    WHEN 'db.type/bool' THEN '(v_data)::boolean'
    WHEN 'db.type/timestamptz' THEN '(v_data #>> ''{}'')::timestamptz'
    WHEN 'db.type/date' THEN '(v_data #>> ''{}'')::date'
    WHEN 'db.type/uuid' THEN '(v_data #>> ''{}'')::uuid'
    WHEN 'db.type/bytea' THEN 'decode(v_data #>> ''{}'', ''base64'')'
    WHEN 'db.type/jsonb' THEN 'v_data'
    WHEN 'db.type/ref' THEN '(v_data #>> ''{}'')::bigint'
    ELSE '(v_data #>> ''{}'')'
  END;
$$;

-- Convert attribute ident to table name: person/name -> attr_person_name
CREATE FUNCTION mnemonic_attr_table_name(attr_ident TEXT)
RETURNS TEXT
LANGUAGE sql
IMMUTABLE
AS $$
  SELECT 'attr_' || REPLACE(REPLACE(attr_ident, '/', '_'), '-', '_');
$$;

-- Convert attribute ident to a valid SQL column name
CREATE FUNCTION mnemonic_attr_to_column(attr_ident TEXT)
RETURNS TEXT
LANGUAGE sql
IMMUTABLE
AS $$
  SELECT REPLACE(SPLIT_PART(attr_ident, '/', 2), '-', '_');
$$;

-- Create an inherited table for an attribute
CREATE PROCEDURE mnemonic_create_attr_table(
  p_attr_id BIGINT,
  p_attr_ident TEXT,
  p_value_type TEXT
)
LANGUAGE plpgsql
AS $$
DECLARE
  table_name TEXT;
  extract_expr TEXT;
  pg_type TEXT;
BEGIN
  table_name := mnemonic_attr_table_name(p_attr_ident);
  extract_expr := mnemonic_jsonb_extract_expr(p_value_type);
  pg_type := mnemonic_pg_type(p_value_type);

  EXECUTE format(
    'CREATE TABLE %I (
      v %s GENERATED ALWAYS AS (%s) STORED,
      CHECK (a = %s)
    ) INHERITS (datoms)',
    table_name, pg_type, extract_expr, p_attr_id
  );

  EXECUTE format(
    'CREATE INDEX %I_v ON %I (v) WHERE retracted_by IS NULL',
    table_name, table_name
  );

  EXECUTE format(
    'CREATE INDEX %I_e ON %I (e) WHERE retracted_by IS NULL',
    table_name, table_name
  );

  IF p_value_type = 'db.type/ref' THEN
    EXECUTE format(
      'CREATE INDEX %I_vaet ON %I (v, e) WHERE retracted_by IS NULL',
      table_name, table_name
    );
  END IF;
END;
$$;

-- Lookup attribute ID by ident (uses attr_db_ident table)
CREATE FUNCTION mnemonic_attr_id(ident TEXT)
RETURNS BIGINT
LANGUAGE sql
STABLE
AS $$
  SELECT e FROM attr_db_ident
  WHERE v = ident
    AND retracted_by IS NULL
  LIMIT 1;
$$;

-- Get the current as-of transaction (cached per query since STABLE with no args)
CREATE FUNCTION mnemonic_as_of_tx_cached()
RETURNS BIGINT
LANGUAGE sql
STABLE
AS $$
  SELECT NULLIF(current_setting('mnemonic.as_of_tx', true), '')::bigint;
$$;

-- Check if a datom is visible given the current temporal context
-- Uses mnemonic_as_of_tx_cached() which PostgreSQL evaluates once per query
CREATE FUNCTION mnemonic_datom_visible(datom_tx BIGINT, datom_retracted_by BIGINT)
RETURNS BOOLEAN
LANGUAGE sql
STABLE
AS $$
  SELECT CASE
    WHEN mnemonic_as_of_tx_cached() IS NULL THEN
      datom_retracted_by IS NULL
    ELSE
      datom_tx <= mnemonic_as_of_tx_cached()
      AND (datom_retracted_by IS NULL OR datom_retracted_by > mnemonic_as_of_tx_cached())
  END;
$$;

-- Helper to set the as-of transaction for temporal queries
CREATE FUNCTION mnemonic_set_as_of(tx_id BIGINT)
RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
  IF tx_id IS NULL THEN
    PERFORM set_config('mnemonic.as_of_tx', '', false);
  ELSE
    PERFORM set_config('mnemonic.as_of_tx', tx_id::TEXT, false);
  END IF;
END;
$$;

-- Helper to get the current as-of transaction
CREATE FUNCTION mnemonic_get_as_of()
RETURNS BIGINT
LANGUAGE plpgsql
STABLE
AS $$
DECLARE
  as_of_tx TEXT;
BEGIN
  as_of_tx := current_setting('mnemonic.as_of_tx', true);
  IF as_of_tx IS NULL OR as_of_tx = '' THEN
    RETURN NULL;
  ELSE
    RETURN as_of_tx::BIGINT;
  END IF;
END;
$$;

-- Convert a value to JSONB based on type
CREATE FUNCTION mnemonic_to_jsonb(val TEXT, value_type TEXT)
RETURNS JSONB
LANGUAGE sql
IMMUTABLE
AS $$
  SELECT CASE
    WHEN val IS NULL THEN NULL
    WHEN value_type = 'db.type/text' THEN to_jsonb(val)
    WHEN value_type = 'db.type/int4' THEN to_jsonb(val::integer)
    WHEN value_type = 'db.type/int8' THEN to_jsonb(val::bigint)
    WHEN value_type = 'db.type/float4' THEN to_jsonb(val::real)
    WHEN value_type = 'db.type/float8' THEN to_jsonb(val::double precision)
    WHEN value_type = 'db.type/numeric' THEN to_jsonb(val::numeric)
    WHEN value_type = 'db.type/bool' THEN to_jsonb(val::boolean)
    WHEN value_type = 'db.type/timestamptz' THEN to_jsonb(val)
    WHEN value_type = 'db.type/date' THEN to_jsonb(val)
    WHEN value_type = 'db.type/uuid' THEN to_jsonb(val)
    WHEN value_type = 'db.type/bytea' THEN to_jsonb(encode(val::bytea, 'base64'))
    WHEN value_type = 'db.type/jsonb' THEN val::jsonb
    WHEN value_type = 'db.type/ref' THEN to_jsonb(val::bigint)
    ELSE to_jsonb(val)
  END;
$$;

--------------------------------------------------------------------------------
-- BOOTSTRAP SYSTEM SCHEMA
--------------------------------------------------------------------------------

INSERT INTO transactions (id, tx_instant) VALUES (0, NOW());

-- Bootstrap core attributes (db/ident, db/valueType, db/cardinality, db/unique, db/doc)
-- Entity 1: db/ident
INSERT INTO attr_db_ident (e, a, v_data, tx, retracted_by) VALUES (1, 1, '"db/ident"', 0, NULL);
INSERT INTO attr_db_valuetype (e, a, v_data, tx, retracted_by) VALUES (1, 2, '100', 0, NULL);
INSERT INTO attr_db_cardinality (e, a, v_data, tx, retracted_by) VALUES (1, 3, '200', 0, NULL);

-- Entity 2: db/valueType
INSERT INTO attr_db_ident (e, a, v_data, tx, retracted_by) VALUES (2, 1, '"db/valueType"', 0, NULL);
INSERT INTO attr_db_valuetype (e, a, v_data, tx, retracted_by) VALUES (2, 2, '112', 0, NULL);
INSERT INTO attr_db_cardinality (e, a, v_data, tx, retracted_by) VALUES (2, 3, '200', 0, NULL);

-- Entity 3: db/cardinality
INSERT INTO attr_db_ident (e, a, v_data, tx, retracted_by) VALUES (3, 1, '"db/cardinality"', 0, NULL);
INSERT INTO attr_db_valuetype (e, a, v_data, tx, retracted_by) VALUES (3, 2, '112', 0, NULL);
INSERT INTO attr_db_cardinality (e, a, v_data, tx, retracted_by) VALUES (3, 3, '200', 0, NULL);

-- Entity 4: db/unique
INSERT INTO attr_db_ident (e, a, v_data, tx, retracted_by) VALUES (4, 1, '"db/unique"', 0, NULL);
INSERT INTO attr_db_valuetype (e, a, v_data, tx, retracted_by) VALUES (4, 2, '112', 0, NULL);
INSERT INTO attr_db_cardinality (e, a, v_data, tx, retracted_by) VALUES (4, 3, '200', 0, NULL);

-- Entity 5: db/doc
INSERT INTO attr_db_ident (e, a, v_data, tx, retracted_by) VALUES (5, 1, '"db/doc"', 0, NULL);
INSERT INTO attr_db_valuetype (e, a, v_data, tx, retracted_by) VALUES (5, 2, '100', 0, NULL);
INSERT INTO attr_db_cardinality (e, a, v_data, tx, retracted_by) VALUES (5, 3, '200', 0, NULL);

-- View definition attributes
-- Entity 10: db.view/ident
INSERT INTO attr_db_ident (e, a, v_data, tx, retracted_by) VALUES (10, 1, '"db.view/ident"', 0, NULL);
INSERT INTO attr_db_valuetype (e, a, v_data, tx, retracted_by) VALUES (10, 2, '100', 0, NULL);
INSERT INTO attr_db_cardinality (e, a, v_data, tx, retracted_by) VALUES (10, 3, '200', 0, NULL);

-- Entity 11: db.view/attributes
INSERT INTO attr_db_ident (e, a, v_data, tx, retracted_by) VALUES (11, 1, '"db.view/attributes"', 0, NULL);
INSERT INTO attr_db_valuetype (e, a, v_data, tx, retracted_by) VALUES (11, 2, '112', 0, NULL);
INSERT INTO attr_db_cardinality (e, a, v_data, tx, retracted_by) VALUES (11, 3, '201', 0, NULL);

-- Entity 12: db.view/doc
INSERT INTO attr_db_ident (e, a, v_data, tx, retracted_by) VALUES (12, 1, '"db.view/doc"', 0, NULL);
INSERT INTO attr_db_valuetype (e, a, v_data, tx, retracted_by) VALUES (12, 2, '100', 0, NULL);
INSERT INTO attr_db_cardinality (e, a, v_data, tx, retracted_by) VALUES (12, 3, '200', 0, NULL);

-- Entity 13: db.view/optional-attributes
INSERT INTO attr_db_ident (e, a, v_data, tx, retracted_by) VALUES (13, 1, '"db.view/optional-attributes"', 0, NULL);
INSERT INTO attr_db_valuetype (e, a, v_data, tx, retracted_by) VALUES (13, 2, '112', 0, NULL);
INSERT INTO attr_db_cardinality (e, a, v_data, tx, retracted_by) VALUES (13, 3, '201', 0, NULL);

-- Value type entities (100-112)
INSERT INTO attr_db_ident (e, a, v_data, tx, retracted_by) VALUES
  (100, 1, '"db.type/text"', 0, NULL),
  (101, 1, '"db.type/int4"', 0, NULL),
  (102, 1, '"db.type/int8"', 0, NULL),
  (103, 1, '"db.type/float4"', 0, NULL),
  (104, 1, '"db.type/float8"', 0, NULL),
  (105, 1, '"db.type/numeric"', 0, NULL),
  (106, 1, '"db.type/bool"', 0, NULL),
  (107, 1, '"db.type/timestamptz"', 0, NULL),
  (108, 1, '"db.type/date"', 0, NULL),
  (109, 1, '"db.type/uuid"', 0, NULL),
  (110, 1, '"db.type/bytea"', 0, NULL),
  (111, 1, '"db.type/jsonb"', 0, NULL),
  (112, 1, '"db.type/ref"', 0, NULL);

-- Cardinality entities (200-201)
INSERT INTO attr_db_ident (e, a, v_data, tx, retracted_by) VALUES
  (200, 1, '"db.cardinality/one"', 0, NULL),
  (201, 1, '"db.cardinality/many"', 0, NULL);

-- Uniqueness entities (210-211)
INSERT INTO attr_db_ident (e, a, v_data, tx, retracted_by) VALUES
  (210, 1, '"db.unique/identity"', 0, NULL),
  (211, 1, '"db.unique/value"', 0, NULL);

-- Update partition counters
UPDATE partitions SET next_id = 300 WHERE ident = 'db';
UPDATE partitions SET next_id = 1 WHERE ident = 'tx';

--------------------------------------------------------------------------------
-- HELPER VIEWS FOR SCHEMA INTROSPECTION
--------------------------------------------------------------------------------

CREATE VIEW mnemonic_attributes AS
SELECT
  ident.e AS id,
  ident.v AS ident,
  vtype_ident.v AS value_type,
  card_ident.v AS cardinality,
  uniq_ident.v AS unique_constraint,
  doc.v AS doc
FROM attr_db_ident ident
JOIN attr_db_valuetype vtype ON ident.e = vtype.e AND vtype.retracted_by IS NULL
JOIN attr_db_ident vtype_ident ON vtype.v = vtype_ident.e AND vtype_ident.retracted_by IS NULL
JOIN attr_db_cardinality card ON ident.e = card.e AND card.retracted_by IS NULL
JOIN attr_db_ident card_ident ON card.v = card_ident.e AND card_ident.retracted_by IS NULL
LEFT JOIN attr_db_unique uniq ON ident.e = uniq.e AND uniq.retracted_by IS NULL
LEFT JOIN attr_db_ident uniq_ident ON uniq.v = uniq_ident.e AND uniq_ident.retracted_by IS NULL
LEFT JOIN attr_db_doc doc ON ident.e = doc.e AND doc.retracted_by IS NULL
WHERE ident.retracted_by IS NULL
  AND ident.v LIKE '%/%'
  AND ident.v NOT LIKE 'db.%';

CREATE VIEW mnemonic_views AS
SELECT
  ident.e AS id,
  ident.v AS name,
  doc.v AS doc
FROM attr_db_view_ident ident
LEFT JOIN attr_db_view_doc doc ON ident.e = doc.e AND doc.retracted_by IS NULL
WHERE ident.retracted_by IS NULL;

CREATE VIEW mnemonic_view_attributes AS
SELECT
  view_ident.v AS view_name,
  attr_ident.v AS attribute_ident,
  attr_ident.e AS attribute_id,
  vtype_ident.v AS value_type,
  card_ident.v AS cardinality,
  true AS required
FROM attr_db_view_ident view_ident
JOIN attr_db_view_attributes view_attrs ON view_ident.e = view_attrs.e AND view_attrs.retracted_by IS NULL
JOIN attr_db_ident attr_ident ON view_attrs.v = attr_ident.e AND attr_ident.retracted_by IS NULL
JOIN attr_db_valuetype vtype ON attr_ident.e = vtype.e AND vtype.retracted_by IS NULL
JOIN attr_db_ident vtype_ident ON vtype.v = vtype_ident.e AND vtype_ident.retracted_by IS NULL
JOIN attr_db_cardinality card ON attr_ident.e = card.e AND card.retracted_by IS NULL
JOIN attr_db_ident card_ident ON card.v = card_ident.e AND card_ident.retracted_by IS NULL
WHERE view_ident.retracted_by IS NULL
UNION ALL
SELECT
  view_ident.v AS view_name,
  attr_ident.v AS attribute_ident,
  attr_ident.e AS attribute_id,
  vtype_ident.v AS value_type,
  card_ident.v AS cardinality,
  false AS required
FROM attr_db_view_ident view_ident
JOIN attr_db_view_optional_attributes view_attrs ON view_ident.e = view_attrs.e AND view_attrs.retracted_by IS NULL
JOIN attr_db_ident attr_ident ON view_attrs.v = attr_ident.e AND attr_ident.retracted_by IS NULL
JOIN attr_db_valuetype vtype ON attr_ident.e = vtype.e AND vtype.retracted_by IS NULL
JOIN attr_db_ident vtype_ident ON vtype.v = vtype_ident.e AND vtype_ident.retracted_by IS NULL
JOIN attr_db_cardinality card ON attr_ident.e = card.e AND card.retracted_by IS NULL
JOIN attr_db_ident card_ident ON card.v = card_ident.e AND card_ident.retracted_by IS NULL
WHERE view_ident.retracted_by IS NULL;

--------------------------------------------------------------------------------
-- VIEW REGENERATION
--------------------------------------------------------------------------------

-- Generate a view with specified visibility check
-- p_use_temporal: false = simple "retracted_by IS NULL", true = full temporal check
CREATE PROCEDURE mnemonic_generate_view_sql(
  p_view_name TEXT,
  p_actual_view_name TEXT,
  p_use_temporal BOOLEAN,
  OUT p_select_cols TEXT,
  OUT p_from_clause TEXT,
  OUT p_where_clause TEXT
)
LANGUAGE plpgsql
AS $$
DECLARE
  attr RECORD;
  join_idx INTEGER := 0;
  col_name TEXT;
  table_name TEXT;
  alias TEXT;
  join_type TEXT;
  is_first BOOLEAN := true;
  base_alias TEXT;
  visibility_check TEXT;
BEGIN
  p_select_cols := '';
  p_from_clause := '';
  p_where_clause := '';

  FOR attr IN
    SELECT * FROM mnemonic_view_attributes
    WHERE view_name = p_view_name
    ORDER BY required DESC, attribute_ident
  LOOP
    join_idx := join_idx + 1;
    alias := 'j' || join_idx;
    col_name := mnemonic_attr_to_column(attr.attribute_ident);
    table_name := mnemonic_attr_table_name(attr.attribute_ident);

    -- Choose visibility check based on temporal flag
    IF p_use_temporal THEN
      visibility_check := format('mnemonic_datom_visible(%I.tx, %I.retracted_by)', alias, alias);
    ELSE
      visibility_check := format('%I.retracted_by IS NULL', alias);
    END IF;

    IF is_first THEN
      base_alias := alias;
      p_select_cols := format('%I.e AS id', alias);
      p_from_clause := format('%I %I', table_name, alias);
      p_where_clause := visibility_check;
      is_first := false;

      IF attr.cardinality = 'db.cardinality/one' THEN
        p_select_cols := p_select_cols || format(', %I.v AS %I', alias, col_name);
      ELSE
        p_select_cols := p_select_cols || format(', %I_arr.v AS %I', alias, col_name);
        IF p_use_temporal THEN
          p_from_clause := p_from_clause || format(
            ' LEFT JOIN LATERAL (SELECT ARRAY_AGG(v) AS v FROM %I WHERE e = %I.e AND mnemonic_datom_visible(tx, retracted_by)) %I_arr ON true',
            table_name, base_alias, alias
          );
        ELSE
          p_from_clause := p_from_clause || format(
            ' LEFT JOIN LATERAL (SELECT ARRAY_AGG(v) AS v FROM %I WHERE e = %I.e AND retracted_by IS NULL) %I_arr ON true',
            table_name, base_alias, alias
          );
        END IF;
      END IF;
    ELSE
      join_type := CASE WHEN attr.required THEN 'INNER JOIN' ELSE 'LEFT JOIN' END;

      IF attr.cardinality = 'db.cardinality/one' THEN
        p_select_cols := p_select_cols || format(', %I.v AS %I', alias, col_name);
        IF p_use_temporal THEN
          p_from_clause := p_from_clause || format(
            ' %s %I %I ON %I.e = %I.e AND mnemonic_datom_visible(%I.tx, %I.retracted_by)',
            join_type, table_name, alias, base_alias, alias, alias, alias
          );
        ELSE
          p_from_clause := p_from_clause || format(
            ' %s %I %I ON %I.e = %I.e AND %I.retracted_by IS NULL',
            join_type, table_name, alias, base_alias, alias, alias
          );
        END IF;
      ELSE
        p_select_cols := p_select_cols || format(', %I.v AS %I', alias, col_name);
        IF p_use_temporal THEN
          p_from_clause := p_from_clause || format(
            ' LEFT JOIN LATERAL (SELECT ARRAY_AGG(v) AS v FROM %I WHERE e = %I.e AND mnemonic_datom_visible(tx, retracted_by)) %I ON true',
            table_name, base_alias, alias
          );
        ELSE
          p_from_clause := p_from_clause || format(
            ' LEFT JOIN LATERAL (SELECT ARRAY_AGG(v) AS v FROM %I WHERE e = %I.e AND retracted_by IS NULL) %I ON true',
            table_name, base_alias, alias
          );
        END IF;
      END IF;
    END IF;
  END LOOP;
END;
$$;

-- Regenerate both _current and _history views for a view definition
CREATE PROCEDURE mnemonic_regenerate_view(p_view_name TEXT)
LANGUAGE plpgsql
AS $$
DECLARE
  select_cols TEXT;
  from_clause TEXT;
  where_clause TEXT;
  current_view_name TEXT;
  history_view_name TEXT;
BEGIN
  current_view_name := p_view_name || '_current';
  history_view_name := p_view_name || '_history';

  -- Drop existing views
  EXECUTE format('DROP VIEW IF EXISTS %I CASCADE', current_view_name);
  EXECUTE format('DROP VIEW IF EXISTS %I CASCADE', history_view_name);

  -- Generate _current view (fast, simple retracted_by IS NULL)
  CALL mnemonic_generate_view_sql(p_view_name, current_view_name, false, select_cols, from_clause, where_clause);

  IF select_cols = '' THEN
    RAISE NOTICE 'View % has no attributes, skipping', p_view_name;
    RETURN;
  END IF;

  EXECUTE format(
    'CREATE VIEW %I AS SELECT %s FROM %s WHERE %s',
    current_view_name, select_cols, from_clause, where_clause
  );

  -- Add DML triggers to _current view
  EXECUTE format(
    'CREATE TRIGGER %I_insert INSTEAD OF INSERT ON %I FOR EACH ROW EXECUTE FUNCTION mnemonic_view_insert(%L)',
    current_view_name, current_view_name, p_view_name
  );

  EXECUTE format(
    'CREATE TRIGGER %I_update INSTEAD OF UPDATE ON %I FOR EACH ROW EXECUTE FUNCTION mnemonic_view_update(%L)',
    current_view_name, current_view_name, p_view_name
  );

  EXECUTE format(
    'CREATE TRIGGER %I_delete INSTEAD OF DELETE ON %I FOR EACH ROW EXECUTE FUNCTION mnemonic_view_delete(%L)',
    current_view_name, current_view_name, p_view_name
  );

  -- Generate _history view (temporal, uses mnemonic_datom_visible)
  CALL mnemonic_generate_view_sql(p_view_name, history_view_name, true, select_cols, from_clause, where_clause);

  EXECUTE format(
    'CREATE VIEW %I AS SELECT %s FROM %s WHERE %s',
    history_view_name, select_cols, from_clause, where_clause
  );

  -- Generate dispatching view (base name) that routes to _current or _history
  -- based on whether mnemonic.as_of_tx is set
  -- The UNION ALL with mutually exclusive conditions allows the optimizer to prune
  EXECUTE format('DROP VIEW IF EXISTS %I CASCADE', p_view_name);
  EXECUTE format(
    'CREATE VIEW %I AS
     SELECT * FROM %I
     WHERE COALESCE(NULLIF(current_setting(''mnemonic.as_of_tx'', true), ''''), '''') = ''''
     UNION ALL
     SELECT * FROM %I
     WHERE COALESCE(NULLIF(current_setting(''mnemonic.as_of_tx'', true), ''''), '''') != ''''',
    p_view_name, current_view_name, history_view_name
  );

  -- Add DML triggers to base view (routes to _current for writes)
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
  jsonb_val JSONB;
BEGIN
  new_entity := mnemonic_allocate_entity('user');
  tx_id := mnemonic_new_transaction();

  FOR attr IN SELECT * FROM mnemonic_view_attributes WHERE view_name = p_view_name
  LOOP
    col_name := mnemonic_attr_to_column(attr.attribute_ident);
    table_name := mnemonic_attr_table_name(attr.attribute_ident);

    EXECUTE format('SELECT ($1).%I::TEXT', col_name) INTO col_value USING NEW;

    IF col_value IS NOT NULL THEN
      jsonb_val := mnemonic_to_jsonb(col_value, attr.value_type);
      EXECUTE format(
        'INSERT INTO %I (e, a, v_data, tx, retracted_by) VALUES ($1, $2, $3, $4, NULL)',
        table_name
      ) USING new_entity, attr.attribute_id, jsonb_val, tx_id;
    END IF;
  END LOOP;

  NEW.id := new_entity;
  RETURN NEW;
END;
$$;

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
  jsonb_val JSONB;
BEGIN
  tx_id := mnemonic_new_transaction();

  FOR attr IN SELECT * FROM mnemonic_view_attributes WHERE view_name = p_view_name
  LOOP
    col_name := mnemonic_attr_to_column(attr.attribute_ident);
    table_name := mnemonic_attr_table_name(attr.attribute_ident);

    EXECUTE format('SELECT ($1).%I::TEXT', col_name) INTO old_value USING OLD;
    EXECUTE format('SELECT ($1).%I::TEXT', col_name) INTO new_value USING NEW;

    IF old_value IS DISTINCT FROM new_value THEN
      IF old_value IS NOT NULL THEN
        EXECUTE format(
          'UPDATE %I SET retracted_by = $1 WHERE e = $2 AND retracted_by IS NULL',
          table_name
        ) USING tx_id, OLD.id;
      END IF;

      IF new_value IS NOT NULL THEN
        jsonb_val := mnemonic_to_jsonb(new_value, attr.value_type);
        EXECUTE format(
          'INSERT INTO %I (e, a, v_data, tx, retracted_by) VALUES ($1, $2, $3, $4, NULL)',
          table_name
        ) USING OLD.id, attr.attribute_id, jsonb_val, tx_id;
      END IF;
    END IF;
  END LOOP;

  RETURN NEW;
END;
$$;

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
    table_name := mnemonic_attr_table_name(attr.attribute_ident);
    EXECUTE format(
      'UPDATE %I SET retracted_by = $1 WHERE e = $2 AND retracted_by IS NULL',
      table_name
    ) USING tx_id, OLD.id;
  END LOOP;

  RETURN OLD;
END;
$$;

--------------------------------------------------------------------------------
-- SELF-MANAGING VIEW DEFINITIONS
--------------------------------------------------------------------------------

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
  IF NEW.attributes IS NULL OR array_length(NEW.attributes, 1) IS NULL THEN
    RAISE EXCEPTION 'View must have at least one required attribute';
  END IF;

  view_id := mnemonic_allocate_entity('db');
  tx_id := mnemonic_new_transaction();

  INSERT INTO attr_db_view_ident (e, a, v_data, tx, retracted_by)
  VALUES (view_id, 10, to_jsonb(NEW.name), tx_id, NULL);

  FOREACH attr_ident IN ARRAY NEW.attributes
  LOOP
    attr_id := mnemonic_attr_id(attr_ident);
    IF attr_id IS NULL THEN
      RAISE EXCEPTION 'Unknown attribute: %', attr_ident;
    END IF;
    INSERT INTO attr_db_view_attributes (e, a, v_data, tx, retracted_by)
    VALUES (view_id, 11, to_jsonb(attr_id), tx_id, NULL);
  END LOOP;

  IF NEW.optional_attributes IS NOT NULL AND array_length(NEW.optional_attributes, 1) IS NOT NULL THEN
    FOREACH attr_ident IN ARRAY NEW.optional_attributes
    LOOP
      attr_id := mnemonic_attr_id(attr_ident);
      IF attr_id IS NULL THEN
        RAISE EXCEPTION 'Unknown attribute: %', attr_ident;
      END IF;
      INSERT INTO attr_db_view_optional_attributes (e, a, v_data, tx, retracted_by)
      VALUES (view_id, 13, to_jsonb(attr_id), tx_id, NULL);
    END LOOP;
  END IF;

  IF NEW.doc IS NOT NULL THEN
    INSERT INTO attr_db_view_doc (e, a, v_data, tx, retracted_by)
    VALUES (view_id, 12, to_jsonb(NEW.doc), tx_id, NULL);
  END IF;

  CALL mnemonic_regenerate_view(NEW.name);

  NEW.id := view_id;
  RETURN NEW;
END;
$$;

CREATE FUNCTION mnemonic_defined_views_update()
RETURNS TRIGGER
LANGUAGE plpgsql
AS $$
DECLARE
  tx_id BIGINT;
  attr_ident TEXT;
  attr_id BIGINT;
BEGIN
  IF NEW.attributes IS NULL OR array_length(NEW.attributes, 1) IS NULL THEN
    RAISE EXCEPTION 'View must have at least one required attribute';
  END IF;

  tx_id := mnemonic_new_transaction();

  IF NEW.name IS DISTINCT FROM OLD.name THEN
    UPDATE attr_db_view_ident SET retracted_by = tx_id
    WHERE e = OLD.id AND retracted_by IS NULL;

    INSERT INTO attr_db_view_ident (e, a, v_data, tx, retracted_by)
    VALUES (OLD.id, 10, to_jsonb(NEW.name), tx_id, NULL);

    -- Drop all three views (base, _current, _history)
    EXECUTE format('DROP VIEW IF EXISTS %I CASCADE', OLD.name);
    EXECUTE format('DROP VIEW IF EXISTS %I CASCADE', OLD.name || '_current');
    EXECUTE format('DROP VIEW IF EXISTS %I CASCADE', OLD.name || '_history');
  END IF;

  IF NEW.attributes IS DISTINCT FROM OLD.attributes THEN
    UPDATE attr_db_view_attributes SET retracted_by = tx_id
    WHERE e = OLD.id AND retracted_by IS NULL;

    FOREACH attr_ident IN ARRAY NEW.attributes
    LOOP
      attr_id := mnemonic_attr_id(attr_ident);
      IF attr_id IS NULL THEN
        RAISE EXCEPTION 'Unknown attribute: %', attr_ident;
      END IF;
      INSERT INTO attr_db_view_attributes (e, a, v_data, tx, retracted_by)
      VALUES (OLD.id, 11, to_jsonb(attr_id), tx_id, NULL);
    END LOOP;
  END IF;

  IF NEW.optional_attributes IS DISTINCT FROM OLD.optional_attributes THEN
    UPDATE attr_db_view_optional_attributes SET retracted_by = tx_id
    WHERE e = OLD.id AND retracted_by IS NULL;

    IF NEW.optional_attributes IS NOT NULL AND array_length(NEW.optional_attributes, 1) IS NOT NULL THEN
      FOREACH attr_ident IN ARRAY NEW.optional_attributes
      LOOP
        attr_id := mnemonic_attr_id(attr_ident);
        IF attr_id IS NULL THEN
          RAISE EXCEPTION 'Unknown attribute: %', attr_ident;
        END IF;
        INSERT INTO attr_db_view_optional_attributes (e, a, v_data, tx, retracted_by)
        VALUES (OLD.id, 13, to_jsonb(attr_id), tx_id, NULL);
      END LOOP;
    END IF;
  END IF;

  IF NEW.doc IS DISTINCT FROM OLD.doc THEN
    UPDATE attr_db_view_doc SET retracted_by = tx_id
    WHERE e = OLD.id AND retracted_by IS NULL;

    IF NEW.doc IS NOT NULL THEN
      INSERT INTO attr_db_view_doc (e, a, v_data, tx, retracted_by)
      VALUES (OLD.id, 12, to_jsonb(NEW.doc), tx_id, NULL);
    END IF;
  END IF;

  CALL mnemonic_regenerate_view(NEW.name);

  RETURN NEW;
END;
$$;

CREATE FUNCTION mnemonic_defined_views_delete()
RETURNS TRIGGER
LANGUAGE plpgsql
AS $$
DECLARE
  tx_id BIGINT;
BEGIN
  tx_id := mnemonic_new_transaction();

  UPDATE attr_db_view_ident SET retracted_by = tx_id
  WHERE e = OLD.id AND retracted_by IS NULL;

  UPDATE attr_db_view_attributes SET retracted_by = tx_id
  WHERE e = OLD.id AND retracted_by IS NULL;

  UPDATE attr_db_view_optional_attributes SET retracted_by = tx_id
  WHERE e = OLD.id AND retracted_by IS NULL;

  UPDATE attr_db_view_doc SET retracted_by = tx_id
  WHERE e = OLD.id AND retracted_by IS NULL;

  -- Drop all three views (base, _current, _history)
  EXECUTE format('DROP VIEW IF EXISTS %I CASCADE', OLD.name);
  EXECUTE format('DROP VIEW IF EXISTS %I CASCADE', OLD.name || '_current');
  EXECUTE format('DROP VIEW IF EXISTS %I CASCADE', OLD.name || '_history');

  RETURN OLD;
END;
$$;

CREATE TRIGGER mnemonic_defined_views_insert_trigger
  INSTEAD OF INSERT ON mnemonic_defined_views
  FOR EACH ROW EXECUTE FUNCTION mnemonic_defined_views_insert();

CREATE TRIGGER mnemonic_defined_views_update_trigger
  INSTEAD OF UPDATE ON mnemonic_defined_views
  FOR EACH ROW EXECUTE FUNCTION mnemonic_defined_views_update();

CREATE TRIGGER mnemonic_defined_views_delete_trigger
  INSTEAD OF DELETE ON mnemonic_defined_views
  FOR EACH ROW EXECUTE FUNCTION mnemonic_defined_views_delete();
