import { PGlite } from "@electric-sql/pglite";
import { pg_ivm } from "@electric-sql/pglite/pg_ivm";
import { readFileSync } from "fs";
import { dirname, join } from "path";
import { fileURLToPath } from "url";

const __dirname = dirname(fileURLToPath(import.meta.url));

const NUM_USERS = 20_000;

const firstNames = [
  "Alice", "Bob", "Charlie", "David", "Eve", "Frank", "Grace", "Henry",
  "Ivy", "Jack", "Kate", "Leo", "Mia", "Noah", "Olivia", "Paul",
  "Quinn", "Rose", "Sam", "Tina", "Uma", "Victor", "Wendy", "Xander",
  "Yara", "Zach", "Anna", "Ben", "Clara", "Dan", "Emma", "Finn"
];

const lastNames = [
  "Smith", "Johnson", "Williams", "Brown", "Jones", "Garcia", "Miller",
  "Davis", "Rodriguez", "Martinez", "Hernandez", "Lopez", "Gonzalez",
  "Wilson", "Anderson", "Thomas", "Taylor", "Moore", "Jackson", "Martin"
];

function randomName() {
  const first = firstNames[Math.floor(Math.random() * firstNames.length)];
  const last = lastNames[Math.floor(Math.random() * lastNames.length)];
  return `${first} ${last}`;
}

function randomEmail(name, idx) {
  const clean = name.toLowerCase().replace(" ", ".");
  return `${clean}.${idx}@example.com`;
}

function randomBirthYear() {
  return 1950 + Math.floor(Math.random() * 60);
}

async function explainQuery(db, name, sql, params = []) {
  console.log(`\n${"=".repeat(70)}`);
  console.log(`EXPLAIN ANALYZE: ${name}`);
  console.log(`${"=".repeat(70)}`);
  console.log(`SQL: ${sql}`);
  if (params.length > 0) {
    console.log(`Params: ${JSON.stringify(params)}`);
  }
  console.log("-".repeat(70));

  const explainSql = `EXPLAIN ANALYZE ${sql}`;
  const result = await db.query(explainSql, params);

  for (const row of result.rows) {
    console.log(row["QUERY PLAN"]);
  }
}

async function benchmark() {
  console.log("Setting up database with pg_ivm extension...");

  // Create PGlite with pg_ivm extension
  const db = new PGlite({
    extensions: { pg_ivm }
  });

  // Load bootstrap SQL
  const bootstrapSql = readFileSync(
    join(__dirname, "sql", "bootstrap.sql"),
    "utf-8"
  );
  await db.exec(bootstrapSql);

  // Create the extension
  await db.exec("CREATE EXTENSION IF NOT EXISTS pg_ivm");

  // Define attributes (simplified - directly insert into datoms)
  const txId = 1000000n;

  // user/name = attr 300
  await db.query(
    `INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES ($1, $2, $3, $4, NULL)`,
    ["300", "1", "user/name", txId.toString()]
  );
  await db.query(
    `INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES ($1, $2, $3, $4, NULL)`,
    ["300", "2", "100", txId.toString()] // db.type/text
  );
  await db.query(
    `INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES ($1, $2, $3, $4, NULL)`,
    ["300", "3", "200", txId.toString()] // db.cardinality/one
  );

  // user/email = attr 301
  await db.query(
    `INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES ($1, $2, $3, $4, NULL)`,
    ["301", "1", "user/email", txId.toString()]
  );
  await db.query(
    `INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES ($1, $2, $3, $4, NULL)`,
    ["301", "2", "100", txId.toString()]
  );
  await db.query(
    `INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES ($1, $2, $3, $4, NULL)`,
    ["301", "3", "200", txId.toString()]
  );

  // user/birth-year = attr 302
  await db.query(
    `INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES ($1, $2, $3, $4, NULL)`,
    ["302", "1", "user/birth-year", txId.toString()]
  );
  await db.query(
    `INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES ($1, $2, $3, $4, NULL)`,
    ["302", "2", "102", txId.toString()] // db.type/int4
  );
  await db.query(
    `INSERT INTO datoms_ref (e, a, v, tx, retracted_by) VALUES ($1, $2, $3, $4, NULL)`,
    ["302", "3", "200", txId.toString()]
  );

  console.log("Attributes defined.");

  // Create the IMMV (Incremental Materialized View) using pg_ivm
  console.log("\nCreating Incremental Materialized View with pg_ivm...");

  const immvStart = performance.now();

  // Note: pg_ivm requires simple base tables, so we create the IMMV on the datom tables
  await db.exec(`
    SELECT pgivm.create_immv('users_immv', $$
      SELECT
        j1.e AS id,
        j1.v AS birth_year,
        j2.v AS email,
        j3.v AS name
      FROM datoms_int4 j1
      INNER JOIN datoms_text j2 ON j1.e = j2.e AND j2.a = 301 AND j2.retracted_by IS NULL
      INNER JOIN datoms_text j3 ON j1.e = j3.e AND j3.a = 300 AND j3.retracted_by IS NULL
      WHERE j1.a = 302 AND j1.retracted_by IS NULL
    $$)
  `);

  console.log(`IMMV created: ${(performance.now() - immvStart).toFixed(2)}ms`);

  // Create indexes on the IMMV
  await db.exec(`CREATE INDEX users_immv_name_idx ON users_immv (name)`);
  await db.exec(`CREATE INDEX users_immv_email_idx ON users_immv (email)`);
  await db.exec(`CREATE INDEX users_immv_birth_year_idx ON users_immv (birth_year)`);
  await db.exec(`CREATE INDEX users_immv_name_pattern_idx ON users_immv (name text_pattern_ops)`);
  console.log("Indexes created on IMMV.");

  // Insert users directly into datom tables
  const insertedNames = [];
  const userIds = [];
  console.log(`\nInserting ${NUM_USERS} users (IMMV updates automatically)...`);
  const start = performance.now();

  const USER_PARTITION_BASE = 562949953421312n; // 0x0002_0000_0000_0000

  for (let i = 0; i < NUM_USERS; i++) {
    const name = randomName();
    const email = randomEmail(name, i);
    const birthYear = randomBirthYear();
    const entityId = USER_PARTITION_BASE + BigInt(i + 1);

    insertedNames.push(name);
    userIds.push(entityId);

    // Insert all three datoms for this user
    await db.query(
      `INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES ($1, 300, $2, $3, NULL)`,
      [entityId.toString(), name, txId.toString()]
    );
    await db.query(
      `INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES ($1, 301, $2, $3, NULL)`,
      [entityId.toString(), email, txId.toString()]
    );
    await db.query(
      `INSERT INTO datoms_int4 (e, a, v, tx, retracted_by) VALUES ($1, 302, $2, $3, NULL)`,
      [entityId.toString(), birthYear, txId.toString()]
    );

    if ((i + 1) % 5000 === 0) {
      const elapsed = performance.now() - start;
      const rate = (i + 1) / (elapsed / 1000);
      console.log(`  ${i + 1} inserted (${rate.toFixed(0)} rows/sec)`);
    }
  }

  const insertTime = performance.now() - start;
  console.log(`Insert complete: ${(insertTime / 1000).toFixed(1)}s (${(NUM_USERS / (insertTime / 1000)).toFixed(0)} rows/sec)`);

  // Verify IMMV has the data
  const countResult = await db.query(`SELECT COUNT(*) as cnt FROM users_immv`);
  console.log(`\nIMMV row count: ${countResult.rows[0].cnt}`);

  // Pick a target name
  const targetName = insertedNames[1000];
  console.log(`Target name for queries: "${targetName}"`);

  // Run query benchmarks
  console.log("\n" + "#".repeat(70));
  console.log("# INCREMENTAL MATERIALIZED VIEW QUERIES");
  console.log("#".repeat(70));

  await explainQuery(
    db,
    "Exact name match (IMMV)",
    `SELECT * FROM users_immv WHERE name = $1`,
    [targetName]
  );

  await explainQuery(
    db,
    "Name prefix LIKE (IMMV)",
    `SELECT * FROM users_immv WHERE name LIKE 'Alice%'`
  );

  await explainQuery(
    db,
    "Exact email match (IMMV)",
    `SELECT * FROM users_immv WHERE email = $1`,
    [`alice.smith.500@example.com`]
  );

  await explainQuery(
    db,
    "Exact birth_year match (IMMV)",
    `SELECT * FROM users_immv WHERE birth_year = 1985`
  );

  await explainQuery(
    db,
    "Point lookup by ID (IMMV)",
    `SELECT * FROM users_immv WHERE id = $1`,
    [userIds[500].toString()]
  );

  // Test that IMMV updates automatically by inserting a new user
  console.log("\n" + "=".repeat(70));
  console.log("Testing automatic IMMV update...");
  console.log("=".repeat(70));

  const beforeCount = await db.query(`SELECT COUNT(*) as cnt FROM users_immv`);
  console.log(`Before insert: ${beforeCount.rows[0].cnt} rows`);

  const newEntityId = USER_PARTITION_BASE + BigInt(NUM_USERS + 1);
  const insertStart = performance.now();

  await db.query(
    `INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES ($1, 300, 'Test User', $2, NULL)`,
    [newEntityId.toString(), txId.toString()]
  );
  await db.query(
    `INSERT INTO datoms_text (e, a, v, tx, retracted_by) VALUES ($1, 301, 'test@example.com', $2, NULL)`,
    [newEntityId.toString(), txId.toString()]
  );
  await db.query(
    `INSERT INTO datoms_int4 (e, a, v, tx, retracted_by) VALUES ($1, 302, 2000, $2, NULL)`,
    [newEntityId.toString(), txId.toString()]
  );

  const insertSingleTime = performance.now() - insertStart;

  const afterCount = await db.query(`SELECT COUNT(*) as cnt FROM users_immv`);
  console.log(`After insert: ${afterCount.rows[0].cnt} rows`);
  console.log(`Single user insert time (with IMMV update): ${insertSingleTime.toFixed(2)}ms`);

  // Verify the new user is queryable
  const newUser = await db.query(`SELECT * FROM users_immv WHERE name = 'Test User'`);
  console.log(`New user found: ${newUser.rows.length > 0 ? 'YES' : 'NO'}`);

  await db.close();
}

benchmark().catch(console.error);
