# MnemonicDB

An immutable temporal tuplestore built on [PGlite](https://github.com/electric-sql/pglite). Combines Datomic-style semantics with SQL compatibility for embedded use in Node.js, Bun, or the browser.

## Features

- **Immutable Data Model** - Data is never overwritten. Every change is recorded as a new fact, with retractions marking old values as historical.
- **Temporal Queries** - Query data "as-of" any point in time. Full audit trail of all changes.
- **SQL-Native** - Standard SQL with projection views. Works with ORMs like Drizzle, Prisma, and Kysely.
- **Embedded** - Runs in-process with persistence to filesystem or IndexedDB.
- **Schema as Data** - Attributes and views are stored as queryable data within the database.

## Installation

```bash
npm install mnemonicdb
```

## Quick Start

```typescript
import { createMnemonicDB } from 'mnemonicdb';

// Create an in-memory database
const db = await createMnemonicDB();

// Define attributes
await db.defineAttribute({
  ident: 'person/name',
  valueType: 'db.type/text',
  cardinality: 'db.cardinality/one',
});

await db.defineAttribute({
  ident: 'person/email',
  valueType: 'db.type/text',
  cardinality: 'db.cardinality/one',
  unique: 'db.unique/identity',
});

// Create a projection view
await db.defineView({
  name: 'persons',
  attributes: ['person/name', 'person/email'],
});

// Use standard SQL
await db.query(`INSERT INTO persons (name, email) VALUES ('Alice', 'alice@example.com')`);

const persons = await db.query<{ id: bigint; name: string; email: string }>(
  'SELECT * FROM persons'
);
```

## Time Travel

Every transaction is recorded, allowing you to query historical states:

```typescript
// Get current data
const current = await db.query('SELECT * FROM persons WHERE id = $1', [personId]);

// Query data as it existed at a specific transaction
const historical = await db.queryAsOf(
  'SELECT * FROM persons WHERE id = $1',
  [personId],
  txId  // Transaction ID to query as-of
);

// Set a global as-of point for all queries
await db.setAsOf(txId);
const allHistorical = await db.query('SELECT * FROM persons');
await db.setAsOf(null);  // Return to current
```

## The Datom Model

Data is stored as datoms (data atoms) - immutable facts:

```
[Entity, Attribute, Value, Transaction, RetractedBy]
```

When you update a value, the old datom is marked as retracted and a new datom is asserted:

```
[101, person/name, "Bob",    tx/1,  tx/50]   # Retracted at tx/50
[101, person/name, "Robert", tx/50, null]    # Current value
```

## Persistence

```typescript
// Filesystem persistence (Node.js/Bun)
const db = await createMnemonicDB('./data/mydb');

// Open an existing database
const db = await openMnemonicDB('./data/mydb');

// In-memory (default)
const db = await createMnemonicDB();
```

## Value Types

| Type | PostgreSQL |
|------|------------|
| `db.type/text` | text |
| `db.type/int4` | integer |
| `db.type/int8` | bigint |
| `db.type/float4` | real |
| `db.type/float8` | double precision |
| `db.type/numeric` | numeric |
| `db.type/bool` | boolean |
| `db.type/timestamptz` | timestamptz |
| `db.type/date` | date |
| `db.type/uuid` | uuid |
| `db.type/bytea` | bytea |
| `db.type/jsonb` | jsonb |
| `db.type/ref` | bigint (entity reference) |

## Documentation

Full documentation is available at the [MnemonicDB docs site](https://tbaldridge.github.io/mnemonicdb/).

- [Overview](https://tbaldridge.github.io/mnemonicdb/docs/overview/) - Introduction and design goals
- [Data Model](https://tbaldridge.github.io/mnemonicdb/docs/data-model/) - Datom structure and EAVT model
- [Schema](https://tbaldridge.github.io/mnemonicdb/docs/schema/) - Attribute definitions and migrations
- [Views](https://tbaldridge.github.io/mnemonicdb/docs/views/) - Projection views and ORM compatibility
- [Temporal Queries](https://tbaldridge.github.io/mnemonicdb/docs/temporal-queries/) - Time travel and history
- [Architecture](https://tbaldridge.github.io/mnemonicdb/docs/architecture/) - Internal structure

## Development

```bash
# Install dependencies
npm install

# Build
npm run build

# Run tests
npm test

# Development mode
npm run dev
```

