import { PGlite } from "@electric-sql/pglite";
import { readFileSync } from "fs";
import { dirname, join } from "path";
import { fileURLToPath } from "url";

const __dirname = dirname(fileURLToPath(import.meta.url));

/**
 * Initialize a new MnemonicDB database with the bootstrap schema.
 */
export async function initializeDatabase(db: PGlite): Promise<void> {
  const bootstrapSql = readFileSync(
    join(__dirname, "..", "sql", "bootstrap.sql"),
    "utf-8"
  );
  await db.exec(bootstrapSql);
}

/**
 * Create and initialize a new MnemonicDB instance.
 */
export async function createMnemonicDB(
  dataDir?: string
): Promise<MnemonicDB> {
  const db = new PGlite(dataDir);
  await initializeDatabase(db);
  return new MnemonicDB(db);
}

/**
 * Open an existing MnemonicDB instance (does not run bootstrap).
 */
export async function openMnemonicDB(dataDir: string): Promise<MnemonicDB> {
  const db = new PGlite(dataDir);
  return new MnemonicDB(db);
}

/**
 * MnemonicDB - Immutable temporal tuplestore built on pglite.
 */
export class MnemonicDB {
  constructor(public readonly db: PGlite) {}

  /**
   * Execute raw SQL (for queries).
   */
  async query<T = unknown>(sql: string, params?: unknown[]): Promise<T[]> {
    const result = await this.db.query<T>(sql, params);
    return result.rows;
  }

  /**
   * Execute a query at a specific point in time (as-of query).
   * This is safe for concurrent use - each query sets its own transaction-local context.
   *
   * @param sql - The SQL query to execute
   * @param params - Query parameters (as-of tx will be prepended as $1)
   * @param asOf - Transaction ID to query as of, or null for current state
   */
  async queryAsOf<T = unknown>(
    sql: string,
    params: unknown[] = [],
    asOf: bigint | null
  ): Promise<T[]> {
    if (asOf === null) {
      // Current state - just run the query normally
      return this.query<T>(sql, params);
    }

    // First shift params in the original query: $1 becomes $2, $2 becomes $3, etc.
    const shiftedOriginal = sql.replace(
      /\$(\d+)/g,
      (_, n) => `$${parseInt(n) + 1}`
    );

    // Then wrap with CTE using $1 for asOf
    // LATERAL is required to ensure the subquery sees the config set by the CTE
    const wrappedSql = `
      WITH _mnemonic_asof AS (
        SELECT set_config('mnemonic.as_of_tx', $1::text, true)
      )
      SELECT __inner__.* FROM _mnemonic_asof, LATERAL (${shiftedOriginal}) __inner__
    `;

    const result = await this.db.query<T>(wrappedSql, [asOf.toString(), ...params]);
    return result.rows;
  }

  /**
   * Execute raw SQL (for mutations).
   */
  async exec(sql: string): Promise<void> {
    await this.db.exec(sql);
  }

  /**
   * Create a new transaction and return its ID.
   * @deprecated Use beginTransaction() instead for managed transaction context.
   */
  async newTransaction(): Promise<bigint> {
    const result = await this.db.query<{ mnemonic_new_transaction: string }>(
      "SELECT mnemonic_new_transaction()"
    );
    return BigInt(result.rows[0].mnemonic_new_transaction);
  }

  /**
   * Begin a new MnemonicDB transaction and set it as the active transaction context.
   * All subsequent inserts/updates/deletes will use this transaction until commit.
   * Throws if a transaction is already active.
   */
  async beginTransaction(): Promise<void> {
    await this.db.exec("CALL mnemonic_begin_tx()");
  }

  /**
   * Commit the current MnemonicDB transaction and clear the transaction context.
   * Throws if no transaction is active.
   */
  async commitTransaction(): Promise<void> {
    await this.db.exec("CALL mnemonic_commit_tx()");
  }

  /**
   * Check if there is an active MnemonicDB transaction.
   */
  async inTransaction(): Promise<boolean> {
    const result = await this.db.query<{ mnemonic_in_tx: boolean }>(
      "SELECT mnemonic_in_tx()"
    );
    return result.rows[0].mnemonic_in_tx;
  }

  /**
   * Execute a function within a transaction context.
   * Automatically begins a transaction, executes the function, and commits.
   * If the function throws, the transaction context is cleared (but note that
   * MnemonicDB transactions are append-only, so partial writes may persist).
   */
  async withTransaction<T>(fn: () => Promise<T>): Promise<T> {
    await this.beginTransaction();
    try {
      const result = await fn();
      await this.commitTransaction();
      return result;
    } catch (error) {
      // Clear the transaction context on error
      try {
        await this.db.exec("SELECT set_config('mnemonic.current_tx', '', false)");
      } catch {
        // Ignore errors clearing context
      }
      throw error;
    }
  }

  /**
   * Allocate a new entity ID from a partition.
   */
  async allocateEntity(partition: string = "user"): Promise<bigint> {
    const result = await this.db.query<{ mnemonic_allocate_entity: string }>(
      "SELECT mnemonic_allocate_entity($1)",
      [partition]
    );
    return BigInt(result.rows[0].mnemonic_allocate_entity);
  }

  /**
   * Get the entity ID for an attribute by its ident.
   */
  async attrId(ident: string): Promise<bigint | null> {
    const result = await this.db.query<{ mnemonic_attr_id: string | null }>(
      "SELECT mnemonic_attr_id($1)",
      [ident]
    );
    const id = result.rows[0].mnemonic_attr_id;
    return id ? BigInt(id) : null;
  }

  /**
   * List all user-defined attributes.
   */
  async listAttributes(): Promise<AttributeInfo[]> {
    return this.query<AttributeInfo>("SELECT * FROM mnemonic_attributes");
  }

  /**
   * List all defined views.
   */
  async listViews(): Promise<ViewInfo[]> {
    return this.query<ViewInfo>("SELECT * FROM mnemonic_views");
  }

  /**
   * Close the database connection.
   */
  async close(): Promise<void> {
    await this.db.close();
  }

  /**
   * Set the as-of transaction for temporal queries.
   * All subsequent queries will see the database as of this transaction.
   * Pass null to return to current (latest) view.
   */
  async setAsOf(txId: bigint | null): Promise<void> {
    await this.db.query("SELECT mnemonic_set_as_of($1)", [
      txId?.toString() ?? null,
    ]);
  }

  /**
   * Get the current as-of transaction.
   * Returns null if viewing current (latest) state.
   */
  async getAsOf(): Promise<bigint | null> {
    const result = await this.db.query<{ mnemonic_get_as_of: string | null }>(
      "SELECT mnemonic_get_as_of()"
    );
    const value = result.rows[0].mnemonic_get_as_of;
    return value ? BigInt(value) : null;
  }

  /**
   * Execute a function with a specific as-of context, then restore the previous context.
   * Useful for temporarily viewing historical data.
   */
  async withAsOf<T>(txId: bigint | null, fn: () => Promise<T>): Promise<T> {
    const previousAsOf = await this.getAsOf();
    try {
      await this.setAsOf(txId);
      return await fn();
    } finally {
      await this.setAsOf(previousAsOf);
    }
  }

  /**
   * Define a new attribute in the schema.
   * Creates the attribute metadata and an inherited table for the attribute's data.
   */
  async defineAttribute(attr: AttributeDefinition): Promise<bigint> {
    // Use the self-managing admin view - it handles datom creation and table generation
    const result = await this.db.query<{ id: string }>(
      `INSERT INTO mnemonic_defined_attributes (ident, value_type, cardinality, unique_constraint, doc)
       VALUES ($1, $2, $3, $4, $5)
       RETURNING id`,
      [attr.ident, attr.valueType, attr.cardinality, attr.unique ?? null, attr.doc ?? null]
    );
    return BigInt(result.rows[0].id);
  }

  /**
   * Define a new view in the schema.
   * Automatically creates the SQL view via the mnemonic_defined_views admin table.
   */
  async defineView(view: ViewDefinition): Promise<bigint> {
    // Use the self-managing admin view - it handles datom creation and SQL view generation
    const result = await this.db.query<{ id: string }>(
      `INSERT INTO mnemonic_defined_views (name, attributes, optional_attributes, doc)
       VALUES ($1, $2, $3, $4)
       RETURNING id`,
      [view.name, view.attributes, view.optionalAttributes ?? [], view.doc ?? null]
    );
    return BigInt(result.rows[0].id);
  }

  /**
   * Update an existing view definition.
   * Automatically regenerates the SQL view.
   */
  async updateView(name: string, view: Partial<ViewDefinition>): Promise<void> {
    const setClauses: string[] = [];
    const params: unknown[] = [];
    let paramIdx = 1;

    if (view.name !== undefined) {
      setClauses.push(`name = $${paramIdx++}`);
      params.push(view.name);
    }
    if (view.attributes !== undefined) {
      setClauses.push(`attributes = $${paramIdx++}`);
      params.push(view.attributes);
    }
    if (view.optionalAttributes !== undefined) {
      setClauses.push(`optional_attributes = $${paramIdx++}`);
      params.push(view.optionalAttributes);
    }
    if (view.doc !== undefined) {
      setClauses.push(`doc = $${paramIdx++}`);
      params.push(view.doc);
    }

    if (setClauses.length === 0) return;

    params.push(name);
    await this.db.query(
      `UPDATE mnemonic_defined_views SET ${setClauses.join(", ")} WHERE name = $${paramIdx}`,
      params
    );
  }

  /**
   * Delete a view definition.
   * Automatically drops the SQL view.
   */
  async deleteView(name: string): Promise<void> {
    await this.db.query(
      `DELETE FROM mnemonic_defined_views WHERE name = $1`,
      [name]
    );
  }

  /**
   * Regenerate all projection views from schema definitions.
   * Usually not needed since views auto-regenerate, but useful for manual sync.
   */
  async regenerateViews(): Promise<void> {
    await this.db.exec("CALL mnemonic_regenerate_views()");
  }
}

export interface AttributeInfo {
  id: string;
  ident: string;
  value_type: string;
  cardinality: string;
  unique_constraint: string | null;
  doc: string | null;
}

export interface ViewInfo {
  id: string;
  name: string;
  doc: string | null;
}

export type ValueType =
  | "db.type/text"
  | "db.type/int4"
  | "db.type/int8"
  | "db.type/float4"
  | "db.type/float8"
  | "db.type/numeric"
  | "db.type/bool"
  | "db.type/timestamptz"
  | "db.type/date"
  | "db.type/uuid"
  | "db.type/bytea"
  | "db.type/jsonb"
  | "db.type/ref";

export type Cardinality = "db.cardinality/one" | "db.cardinality/many";

export type Uniqueness = "db.unique/identity" | "db.unique/value";

export interface AttributeDefinition {
  ident: string;
  valueType: ValueType;
  cardinality: Cardinality;
  unique?: Uniqueness;
  doc?: string;
}

export interface ViewDefinition {
  name: string;
  /** Required attributes (INNER JOIN) - at least one required */
  attributes: string[];
  /** Optional attributes (LEFT JOIN) */
  optionalAttributes?: string[];
  doc?: string;
}

export { PGlite };
