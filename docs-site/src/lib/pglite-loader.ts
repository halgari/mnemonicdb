// PGLite type for the dynamically loaded module
type PGliteType = import('@electric-sql/pglite').PGlite;
type PGliteClass = typeof import('@electric-sql/pglite').PGlite;

let dbInstance: PGliteType | null = null;
let dbInitPromise: Promise<PGliteType> | null = null;
let PGliteConstructor: PGliteClass | null = null;

/**
 * Load PGLite from CDN to avoid WASM asset path issues with base URLs.
 */
async function loadPGlite(): Promise<PGliteClass> {
  if (PGliteConstructor) {
    return PGliteConstructor;
  }

  try {
    // Import from unpkg which serves the browser-compatible ESM build
    const module = await import(
      /* @vite-ignore */
      'https://unpkg.com/@electric-sql/pglite@0.2.12/dist/index.js'
    );

    if (!module.PGlite) {
      throw new Error('PGlite not found in module. Keys: ' + Object.keys(module).join(', '));
    }

    PGliteConstructor = module.PGlite;
    return PGliteConstructor;
  } catch (err) {
    console.error('Failed to load PGLite from CDN:', err);
    throw err;
  }
}

/**
 * Get the shared PGLite instance, initializing it if necessary.
 * The database is bootstrapped with MnemonicDB schema on first call.
 */
export async function getDatabase(): Promise<PGliteType> {
  if (dbInstance) {
    return dbInstance;
  }

  if (dbInitPromise) {
    return dbInitPromise;
  }

  dbInitPromise = initializeDatabase();
  return dbInitPromise;
}

async function initializeDatabase(): Promise<PGliteType> {
  console.log('Loading PGLite from CDN...');
  const PGlite = await loadPGlite();
  console.log('PGLite loaded, creating database...');

  // Create in-memory database
  const db = new PGlite();
  console.log('Database created, waiting for ready...');
  await db.waitReady;
  console.log('Database ready');

  // Fetch and execute bootstrap SQL
  // Try multiple base URL strategies for different environments
  const baseUrl = import.meta.env.BASE_URL || '';
  const paths = [
    `${baseUrl}/bootstrap.sql`,
    `${baseUrl}bootstrap.sql`,
    '/mnemonicdb/bootstrap.sql',
    './bootstrap.sql',
  ];

  let bootstrapSql: string | null = null;
  let lastError: Error | null = null;

  for (const path of paths) {
    try {
      const response = await fetch(path);
      if (response.ok) {
        bootstrapSql = await response.text();
        break;
      }
    } catch (e) {
      lastError = e as Error;
    }
  }

  if (!bootstrapSql) {
    throw new Error(`Failed to fetch bootstrap.sql: ${lastError?.message || 'Not found'}`);
  }

  await db.exec(bootstrapSql);

  dbInstance = db;
  return db;
}

/**
 * Reset the database to a fresh state.
 * This closes the current connection and clears the singleton.
 */
export async function resetDatabase(): Promise<PGliteType> {
  if (dbInstance) {
    await dbInstance.close();
    dbInstance = null;
    dbInitPromise = null;
  }
  return getDatabase();
}

/**
 * Execute a SQL query and return the results.
 */
export async function executeQuery<T = Record<string, unknown>>(
  sql: string
): Promise<{ rows: T[]; fields: { name: string }[] }> {
  const db = await getDatabase();
  const result = await db.query<T>(sql);

  console.log('Raw PGLite result:', result);
  console.log('Result keys:', Object.keys(result));
  console.log('Rows:', result.rows);
  console.log('Fields:', result.fields);

  // Handle different PGLite API versions
  let fields: { name: string }[] = [];
  if (result.fields && Array.isArray(result.fields)) {
    fields = result.fields.map((f: any) => ({
      name: f.name || f.fieldName || String(f)
    }));
  } else if (result.rows && result.rows.length > 0) {
    // Fallback: extract field names from the first row's keys
    fields = Object.keys(result.rows[0]).map(name => ({ name }));
  }

  return {
    rows: result.rows || [],
    fields,
  };
}

/**
 * Execute multiple SQL statements.
 */
export async function executeStatements(sql: string): Promise<void> {
  const db = await getDatabase();
  await db.exec(sql);
}
