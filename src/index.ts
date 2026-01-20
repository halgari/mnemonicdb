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
   * Execute raw SQL (for mutations).
   */
  async exec(sql: string): Promise<void> {
    await this.db.exec(sql);
  }

  /**
   * Create a new transaction and return its ID.
   */
  async newTransaction(): Promise<bigint> {
    const result = await this.db.query<{ mnemonic_new_transaction: string }>(
      "SELECT mnemonic_new_transaction()"
    );
    return BigInt(result.rows[0].mnemonic_new_transaction);
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
   * Define a new attribute in the schema.
   */
  async defineAttribute(attr: AttributeDefinition): Promise<bigint> {
    const txId = await this.newTransaction();
    const attrId = await this.allocateEntity("db");

    // Get system attribute IDs
    const dbIdent = 1n;
    const dbValueType = 2n;
    const dbCardinality = 3n;
    const dbUnique = 4n;
    const dbDoc = 5n;

    // Get value type entity ID
    const valueTypeId = await this.attrId(attr.valueType);
    if (!valueTypeId) {
      throw new Error(`Unknown value type: ${attr.valueType}`);
    }

    // Get cardinality entity ID
    const cardinalityId = await this.attrId(attr.cardinality);
    if (!cardinalityId) {
      throw new Error(`Unknown cardinality: ${attr.cardinality}`);
    }

    // Assert ident
    await this.db.query(
      `INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES ($1, $2, $3, $4, NULL)`,
      [attrId.toString(), dbIdent.toString(), attr.ident, txId.toString()]
    );

    // Assert valueType
    await this.db.query(
      `INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES ($1, $2, $3, $4, NULL)`,
      [attrId.toString(), dbValueType.toString(), valueTypeId.toString(), txId.toString()]
    );

    // Assert cardinality
    await this.db.query(
      `INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES ($1, $2, $3, $4, NULL)`,
      [attrId.toString(), dbCardinality.toString(), cardinalityId.toString(), txId.toString()]
    );

    // Assert unique if provided
    if (attr.unique) {
      const uniqueId = await this.attrId(attr.unique);
      if (!uniqueId) {
        throw new Error(`Unknown uniqueness: ${attr.unique}`);
      }
      await this.db.query(
        `INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES ($1, $2, $3, $4, NULL)`,
        [attrId.toString(), dbUnique.toString(), uniqueId.toString(), txId.toString()]
      );
    }

    // Assert doc if provided
    if (attr.doc) {
      await this.db.query(
        `INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES ($1, $2, $3, $4, NULL)`,
        [attrId.toString(), dbDoc.toString(), attr.doc, txId.toString()]
      );
    }

    return attrId;
  }

  /**
   * Define a new view in the schema.
   * Automatically creates the SQL view via the mnemonic_defined_views admin table.
   */
  async defineView(view: ViewDefinition): Promise<bigint> {
    // Use the self-managing admin view - it handles datom creation and SQL view generation
    const result = await this.db.query<{ id: string }>(
      `INSERT INTO mnemonic_defined_views (name, attributes, doc)
       VALUES ($1, $2, $3)
       RETURNING id`,
      [view.name, view.attributes, view.doc ?? null]
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
  attributes: string[];
  doc?: string;
}

export { PGlite };
