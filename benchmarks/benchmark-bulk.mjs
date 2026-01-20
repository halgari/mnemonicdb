import { createMnemonicDB } from "../dist/index.js";

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

  for (const row of result) {
    console.log(row["QUERY PLAN"]);
  }
}

async function benchmark() {
  console.log("Setting up database...");
  const db = await createMnemonicDB();

  // Define schema
  await db.defineAttribute({
    ident: "user/name",
    valueType: "db.type/text",
    cardinality: "db.cardinality/one",
  });
  await db.defineAttribute({
    ident: "user/email",
    valueType: "db.type/text",
    cardinality: "db.cardinality/one",
  });
  await db.defineAttribute({
    ident: "user/birth-year",
    valueType: "db.type/int4",
    cardinality: "db.cardinality/one",
  });

  // Create regular view (for schema/inserts via INSTEAD OF triggers)
  await db.defineView({
    name: "users",
    attributes: ["user/name", "user/email", "user/birth-year"],
  });

  // Insert users via regular view
  const insertedNames = [];
  const userIds = [];
  console.log(`\nInserting ${NUM_USERS} users via regular view...`);
  const insertStart = performance.now();

  for (let i = 0; i < NUM_USERS; i++) {
    const name = randomName();
    const email = randomEmail(name, i);
    const birthYear = randomBirthYear();

    insertedNames.push(name);

    const result = await db.query(
      `INSERT INTO users (name, email, birth_year) VALUES ($1, $2, $3) RETURNING id`,
      [name, email, birthYear]
    );
    userIds.push(result[0].id);

    if ((i + 1) % 5000 === 0) {
      const elapsed = performance.now() - insertStart;
      const rate = (i + 1) / (elapsed / 1000);
      console.log(`  ${i + 1} inserted (${rate.toFixed(0)} rows/sec)`);
    }
  }

  const insertTime = performance.now() - insertStart;
  console.log(`Insert complete: ${(insertTime / 1000).toFixed(1)}s (${(NUM_USERS / (insertTime / 1000)).toFixed(0)} rows/sec)`);

  // Now create materialized view for fast reads
  console.log("\n" + "=".repeat(70));
  console.log("Creating MATERIALIZED VIEW for fast queries...");
  console.log("=".repeat(70));

  const matStart = performance.now();

  // Create materialized view based on the same query as the regular view
  await db.exec(`
    CREATE MATERIALIZED VIEW users_fast AS
    SELECT
      j1.e AS id,
      j3.v AS name,
      j2.v AS email,
      j1.v AS birth_year
    FROM datoms_int4 j1
    JOIN datoms_text j2 ON j1.e = j2.e AND j2.a = 301 AND j2.retracted_by IS NULL
    JOIN datoms_text j3 ON j1.e = j3.e AND j3.a = 300 AND j3.retracted_by IS NULL
    WHERE j1.a = 302 AND j1.retracted_by IS NULL
  `);

  const matCreateTime = performance.now() - matStart;
  console.log(`Materialized view created: ${matCreateTime.toFixed(2)}ms`);

  // Create indexes
  const idxStart = performance.now();
  await db.exec(`CREATE INDEX users_fast_id_idx ON users_fast (id)`);
  await db.exec(`CREATE INDEX users_fast_name_idx ON users_fast (name)`);
  await db.exec(`CREATE INDEX users_fast_name_pattern_idx ON users_fast (name text_pattern_ops)`);
  await db.exec(`CREATE INDEX users_fast_email_idx ON users_fast (email)`);
  await db.exec(`CREATE INDEX users_fast_birth_year_idx ON users_fast (birth_year)`);
  console.log(`Indexes created: ${(performance.now() - idxStart).toFixed(2)}ms`);

  const targetName = insertedNames[1000];
  console.log(`\nTarget name: "${targetName}"`);

  // Query benchmarks on materialized view
  console.log("\n" + "#".repeat(70));
  console.log("# MATERIALIZED VIEW QUERIES (users_fast)");
  console.log("#".repeat(70));

  await explainQuery(db, "Exact name match", `SELECT * FROM users_fast WHERE name = $1`, [targetName]);
  await explainQuery(db, "Name LIKE prefix", `SELECT * FROM users_fast WHERE name LIKE 'Alice%'`);
  await explainQuery(db, "Birth year exact", `SELECT * FROM users_fast WHERE birth_year = 1985`);

  // Now test the bulk update workflow
  console.log("\n" + "=".repeat(70));
  console.log("BULK UPDATE WORKFLOW");
  console.log("=".repeat(70));

  // Insert 1000 more users
  const bulkCount = 1000;
  console.log(`\nInserting ${bulkCount} more users...`);
  const bulkStart = performance.now();

  for (let i = 0; i < bulkCount; i++) {
    const name = randomName();
    const email = randomEmail(name, NUM_USERS + i);
    const birthYear = randomBirthYear();

    await db.query(
      `INSERT INTO users (name, email, birth_year) VALUES ($1, $2, $3)`,
      [name, email, birthYear]
    );
  }

  const bulkInsertTime = performance.now() - bulkStart;
  console.log(`Bulk insert: ${bulkInsertTime.toFixed(2)}ms (${(bulkCount / (bulkInsertTime / 1000)).toFixed(0)} rows/sec)`);

  // Check counts before refresh
  const regularCount = await db.query(`SELECT COUNT(*) as cnt FROM users`);
  const matCount = await db.query(`SELECT COUNT(*) as cnt FROM users_fast`);
  console.log(`\nRegular view count: ${regularCount[0].cnt}`);
  console.log(`Materialized view count (stale): ${matCount[0].cnt}`);

  // Refresh materialized view
  const refreshStart = performance.now();
  await db.exec(`REFRESH MATERIALIZED VIEW users_fast`);
  const refreshTime = performance.now() - refreshStart;
  console.log(`\nREFRESH MATERIALIZED VIEW: ${refreshTime.toFixed(2)}ms`);

  const matCountAfter = await db.query(`SELECT COUNT(*) as cnt FROM users_fast`);
  console.log(`Materialized view count (after refresh): ${matCountAfter[0].cnt}`);

  // Concurrent refresh option
  console.log("\n" + "=".repeat(70));
  console.log("CONCURRENT REFRESH (allows reads during refresh)");
  console.log("=".repeat(70));

  // Need unique index for CONCURRENTLY
  await db.exec(`CREATE UNIQUE INDEX users_fast_id_unique ON users_fast (id)`);

  const concurrentStart = performance.now();
  await db.exec(`REFRESH MATERIALIZED VIEW CONCURRENTLY users_fast`);
  const concurrentTime = performance.now() - concurrentStart;
  console.log(`REFRESH CONCURRENTLY: ${concurrentTime.toFixed(2)}ms`);

  // Summary
  console.log("\n" + "=".repeat(70));
  console.log("SUMMARY - Recommended Workflow");
  console.log("=".repeat(70));
  console.log(`
1. Use regular views for WRITES (INSTEAD OF triggers handle datom creation)
2. Use materialized views for READS (indexed, fast queries on any column)
3. Call REFRESH MATERIALIZED VIEW after bulk operations

Timing breakdown:
- Insert ${NUM_USERS} rows:     ${(insertTime / 1000).toFixed(1)}s
- Create mat view:       ${matCreateTime.toFixed(0)}ms
- Create indexes:        ${(performance.now() - idxStart).toFixed(0)}ms
- Refresh (full):        ${refreshTime.toFixed(0)}ms
- Refresh (concurrent):  ${concurrentTime.toFixed(0)}ms

For real-time updates: refresh after each transaction batch
For analytics: refresh on schedule or on-demand
`);

  await db.close();
}

benchmark().catch(console.error);
