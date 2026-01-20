# Projection Views

Projection views are the primary interface for applications. They present datom data in familiar tabular form, making MnemonicDB compatible with standard SQL tooling and ORMs.

## View Structure

A projection view joins datoms from multiple value-type tables to present an entity as a row:

```sql
-- Generated view for 'persons'
CREATE VIEW persons AS
SELECT
  e.entity_id AS id,
  name.v AS name,
  email.v AS email,
  dept.v AS department_id,
  created.v AS created_at
FROM entities e
LEFT JOIN datoms_text name ON e.entity_id = name.e
  AND name.a = attr_id('person/name') AND name.retracted_by IS NULL
LEFT JOIN datoms_text email ON e.entity_id = email.e
  AND email.a = attr_id('person/email') AND email.retracted_by IS NULL
LEFT JOIN datoms_ref dept ON e.entity_id = dept.e
  AND dept.a = attr_id('person/department') AND dept.retracted_by IS NULL
LEFT JOIN datoms_timestamptz created ON e.entity_id = created.e
  AND created.a = attr_id('person/created-at') AND created.retracted_by IS NULL
WHERE e.partition = partition_id('user')
  AND EXISTS (
    SELECT 1 FROM datoms_text d
    WHERE d.e = e.entity_id
    AND d.a IN (attr_id('person/name'), attr_id('person/email'))
    AND d.retracted_by IS NULL
  );
```

### Implicit Columns

Every view includes an implicit `id` column containing the entity ID. This is the primary key for the view.

### NULL Handling

LEFT JOINs ensure that entities appear even if they don't have all attributes. Missing attributes appear as NULL.

## INSTEAD OF Triggers

Views support INSERT, UPDATE, and DELETE through `INSTEAD OF` triggers that translate operations into datom assertions and retractions.

### INSERT Trigger

```sql
CREATE TRIGGER persons_insert
INSTEAD OF INSERT ON persons
FOR EACH ROW
EXECUTE FUNCTION mnemonic_view_insert('persons');
```

The insert function:
1. Allocates a new entity ID from the appropriate partition
2. Creates a new transaction
3. Asserts a datom for each non-NULL column value
4. Returns the new entity ID

```sql
INSERT INTO persons (name, email, department_id)
VALUES ('Alice', 'alice@example.com', 50);

-- Internally becomes:
-- 1. Allocate entity ID (e.g., 101)
-- 2. Create transaction (e.g., tx/500)
-- 3. Assert datoms:
--    [101, :person/name, 'Alice', tx/500, null]
--    [101, :person/email, 'alice@example.com', tx/500, null]
--    [101, :person/department, 50, tx/500, null]
```

### UPDATE Trigger

```sql
CREATE TRIGGER persons_update
INSTEAD OF UPDATE ON persons
FOR EACH ROW
EXECUTE FUNCTION mnemonic_view_update('persons');
```

The update function:
1. Creates a new transaction
2. For each changed column:
   - If old value is not NULL: retract the old datom (set `retracted_by` to current tx)
   - If new value is not NULL: assert a new datom
3. Unchanged columns are not touched

```sql
UPDATE persons SET name = 'Alicia' WHERE id = 101;

-- Internally becomes:
-- 1. Create transaction (e.g., tx/501)
-- 2. Retract old name:
--    UPDATE datoms_text SET retracted_by = tx/501
--    WHERE e = 101 AND a = :person/name AND retracted_by IS NULL
-- 3. Assert new name:
--    [101, :person/name, 'Alicia', tx/501, null]
```

### DELETE Trigger

```sql
CREATE TRIGGER persons_delete
INSTEAD OF DELETE ON persons
FOR EACH ROW
EXECUTE FUNCTION mnemonic_view_delete('persons');
```

The delete function:
1. Creates a new transaction
2. Retracts all datoms for attributes defined in this view
3. Does NOT retract datoms for attributes not in the view

```sql
DELETE FROM persons WHERE id = 101;

-- Internally retracts only the attributes in the 'persons' view:
-- [101, :person/name, ...]        retracted
-- [101, :person/email, ...]       retracted
-- [101, :person/department, ...]  retracted
-- [101, :person/created-at, ...]  retracted
-- [101, :person/tags, ...]        NOT retracted (not in view)
```

This behavior allows different views to manage different aspects of the same entity.

## ORM Compatibility

Projection views look like regular PostgreSQL tables to ORMs:

### Drizzle

```typescript
import { pgTable, text, bigint, timestamp } from 'drizzle-orm/pg-core';

export const persons = pgTable('persons', {
  id: bigint('id', { mode: 'number' }).primaryKey(),
  name: text('name'),
  email: text('email'),
  departmentId: bigint('department_id', { mode: 'number' }),
  createdAt: timestamp('created_at', { withTimezone: true }),
});

// Works normally
const alice = await db.insert(persons).values({
  name: 'Alice',
  email: 'alice@example.com',
}).returning();

await db.update(persons)
  .set({ name: 'Alicia' })
  .where(eq(persons.id, alice.id));
```

### Prisma

```prisma
model Person {
  id           BigInt    @id
  name         String?
  email        String?
  departmentId BigInt?   @map("department_id")
  createdAt    DateTime? @map("created_at") @db.Timestamptz

  @@map("persons")
}
```

### Kysely

```typescript
interface Database {
  persons: {
    id: number;
    name: string | null;
    email: string | null;
    department_id: number | null;
    created_at: Date | null;
  };
}

const db = new Kysely<Database>({ ... });

await db.insertInto('persons')
  .values({ name: 'Alice', email: 'alice@example.com' })
  .execute();
```

## TanStack Query Integration

Views work with pglite's live query features and TanStack Query:

```typescript
import { useLiveQuery } from '@electric-sql/pglite-react';

function PersonList() {
  const persons = useLiveQuery('SELECT * FROM persons ORDER BY name');

  return (
    <ul>
      {persons?.rows.map(p => (
        <li key={p.id}>{p.name}</li>
      ))}
    </ul>
  );
}
```

Changes made through the view (INSERT/UPDATE/DELETE) trigger reactive updates automatically.

## View Regeneration

Views are regenerated from schema data by calling:

```sql
CALL mnemonic_regenerate_views();
```

This reads view definitions from the database and recreates:
1. The SELECT view with appropriate joins
2. INSTEAD OF INSERT trigger
3. INSTEAD OF UPDATE trigger
4. INSTEAD OF DELETE trigger

### When to Regenerate

- After adding/removing attributes from a view definition
- After changing attribute properties
- During migrations

Regeneration is idempotent—safe to run multiple times.

## Limitations

### No Direct Entity Creation

You cannot insert a row without any column values:

```sql
-- This won't work meaningfully
INSERT INTO persons DEFAULT VALUES;
```

At least one attribute value should be provided.

### Cardinality-Many

Cardinality-many attributes are expressed as PostgreSQL arrays:

```sql
-- A person with multiple tags
SELECT * FROM persons;
-- id | name  | email           | tags
-- 1  | Bob   | bob@example.com | {developer,manager}
```

The view aggregates multiple datoms into an array:

```sql
-- In view generation
ARRAY_AGG(tags.v) FILTER (WHERE tags.v IS NOT NULL) AS tags
```

INSERT/UPDATE triggers handle array unpacking to create individual datoms.

(Implementation deferred to later phase)

### Cross-View Updates

An entity might appear in multiple views. Updates through one view only affect the attributes defined in that view.
