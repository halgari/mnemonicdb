# Temporal Queries

MnemonicDB's immutable data model enables powerful temporal queries. Every fact is preserved with its transaction history, allowing you to query data as it existed at any point in time.

## The RetractedBy Column

The key to temporal queries is the `retracted_by` column on every datom:

| retracted_by | Meaning |
|--------------|---------|
| NULL | Datom is current (not retracted) |
| tx_id | Datom was retracted in transaction tx_id |

This enables efficient filtering:

```sql
-- Current data: fact was asserted and not yet retracted
WHERE retracted_by IS NULL

-- Data as-of transaction 500: fact existed at that point
WHERE tx <= 500 AND (retracted_by IS NULL OR retracted_by > 500)
```

## As-Of Queries

An **as-of query** shows data as it existed at a specific transaction.

### As-Of Function

MnemonicDB provides a function to generate as-of views:

```sql
-- Get the persons view as of transaction 500
SELECT * FROM mnemonic_as_of('persons', 500);

-- Get persons as of a specific timestamp
SELECT * FROM mnemonic_as_of('persons', mnemonic_tx_at('2024-01-15 10:30:00'));
```

### How It Works

The `mnemonic_as_of` function generates a query that:
1. Only includes datoms where `tx <= as_of_tx`
2. Excludes datoms where `retracted_by <= as_of_tx`

Conceptually:

```sql
-- Persons as of transaction 500
SELECT
  e.entity_id AS id,
  name.v AS name,
  email.v AS email
FROM entities e
LEFT JOIN datoms_text name ON e.entity_id = name.e
  AND name.a = attr_id('person/name')
  AND name.tx <= 500
  AND (name.retracted_by IS NULL OR name.retracted_by > 500)
LEFT JOIN datoms_text email ON e.entity_id = email.e
  AND email.a = attr_id('person/email')
  AND email.tx <= 500
  AND (email.retracted_by IS NULL OR email.retracted_by > 500)
WHERE ...
```

### Use Cases

**Auditing**: See what data looked like when a decision was made
```sql
-- What did we know about this customer when we approved their loan?
SELECT * FROM mnemonic_as_of('customers', loan.approval_tx)
WHERE id = loan.customer_id;
```

**Debugging**: Reproduce bugs with historical data
```sql
-- What was the state when the error occurred?
SELECT * FROM mnemonic_as_of('orders', error_log.tx);
```

**Compliance**: Demonstrate historical state for regulators
```sql
-- Show account balances as of end of fiscal year
SELECT * FROM mnemonic_as_of('accounts', mnemonic_tx_at('2023-12-31 23:59:59'));
```

## Transaction Lookup

Find the transaction ID for a given timestamp:

```sql
-- Get transaction active at a specific time
SELECT mnemonic_tx_at('2024-01-15 10:30:00');

-- Get all transactions in a time range
SELECT * FROM transactions
WHERE tx_instant BETWEEN '2024-01-01' AND '2024-01-31';
```

## History Queries

A **history query** shows all values an attribute has had over time.

### Entity History

```sql
-- All historical values for person 101's name
SELECT
  tx.tx_instant AS changed_at,
  d.v AS name,
  d.retracted_by IS NOT NULL AS was_retracted,
  rtx.tx_instant AS retracted_at
FROM datoms_text d
JOIN transactions tx ON d.tx = tx.id
LEFT JOIN transactions rtx ON d.retracted_by = rtx.id
WHERE d.e = 101
  AND d.a = attr_id('person/name')
ORDER BY d.tx;
```

Result:
```
changed_at          | name    | was_retracted | retracted_at
2024-01-01 10:00:00 | Robert  | true          | 2024-03-15 14:30:00
2024-03-15 14:30:00 | Bob     | true          | 2024-06-01 09:00:00
2024-06-01 09:00:00 | Roberto | false         | null
```

### Full Entity History

```sql
-- All changes to entity 101
SELECT
  attr.v AS attribute,
  COALESCE(t.v, i.v::text, r.v::text) AS value,
  tx.tx_instant AS asserted_at,
  rtx.tx_instant AS retracted_at
FROM (
  SELECT e, a, tx, retracted_by, v, 'text' AS vtype FROM datoms_text WHERE e = 101
  UNION ALL
  SELECT e, a, tx, retracted_by, v::text, 'int' FROM datoms_int8 WHERE e = 101
  UNION ALL
  SELECT e, a, tx, retracted_by, v::text, 'ref' FROM datoms_ref WHERE e = 101
  -- ... other value types
) d
JOIN datoms_text attr ON d.a = attr.e AND attr.a = attr_id('db/ident')
JOIN transactions tx ON d.tx = tx.id
LEFT JOIN transactions rtx ON d.retracted_by = rtx.id
ORDER BY d.tx, attr.v;
```

### History View Helper

MnemonicDB provides a helper for entity history:

```sql
-- Get full history for an entity
SELECT * FROM mnemonic_entity_history(101);

-- Get history for specific attributes
SELECT * FROM mnemonic_entity_history(101, ARRAY['person/name', 'person/email']);
```

## Transaction Log

Query the transaction log to see all changes:

```sql
-- Recent transactions with their datom counts
SELECT
  t.id AS tx,
  t.tx_instant,
  COUNT(*) AS datom_count
FROM transactions t
JOIN (
  SELECT tx FROM datoms_text
  UNION ALL SELECT tx FROM datoms_int8
  UNION ALL SELECT tx FROM datoms_ref
  -- ... other types
) d ON t.id = d.tx
GROUP BY t.id, t.tx_instant
ORDER BY t.id DESC
LIMIT 100;
```

### Transaction Details

```sql
-- All datoms in a specific transaction
SELECT * FROM mnemonic_tx_datoms(500);
```

## Comparing Points in Time

Compare entity state between two transactions:

```sql
-- What changed between tx 400 and tx 500?
WITH
  old AS (SELECT * FROM mnemonic_as_of('persons', 400) WHERE id = 101),
  new AS (SELECT * FROM mnemonic_as_of('persons', 500) WHERE id = 101)
SELECT
  'name' AS field,
  old.name AS old_value,
  new.name AS new_value
FROM old, new
WHERE old.name IS DISTINCT FROM new.name

UNION ALL

SELECT
  'email',
  old.email,
  new.email
FROM old, new
WHERE old.email IS DISTINCT FROM new.email;
```

## Performance Considerations

### Indexes for Temporal Queries

The standard indexes support efficient temporal queries:

- `(e, a, tx)` - Entity history lookups
- `(a, v, tx)` on AVET tables - Value history lookups
- `(tx)` - Transaction log scans

### As-Of Query Performance

As-of queries add filtering overhead. For frequently-accessed historical snapshots, consider:

1. **Caching**: Cache as-of query results for common timestamps
2. **Materialized snapshots**: For critical audit dates, materialize the as-of state

### Pruning (Future)

For long-running systems, old history may be prunable:
- Archive datoms older than retention period
- Keep only latest state for non-audited attributes
- Compact retracted datoms into summary records

Pruning is destructive and should be used carefully—it removes the ability to query as-of before the pruning point.
