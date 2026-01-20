import { createMnemonicDB } from "../dist/index.js";

const NUM_USERS = 20_000;

const firstNames = ["Alice", "Bob", "Charlie", "David", "Eve", "Frank", "Grace", "Henry",
  "Ivy", "Jack", "Kate", "Leo", "Mia", "Noah", "Olivia", "Paul"];
const lastNames = ["Smith", "Johnson", "Williams", "Brown", "Jones", "Garcia", "Miller",
  "Davis", "Rodriguez", "Martinez"];

function randomName() {
  return `${firstNames[Math.floor(Math.random() * firstNames.length)]} ${lastNames[Math.floor(Math.random() * lastNames.length)]}`;
}

async function explainQuery(db, name, sql, params = []) {
  console.log(`\n${"=".repeat(70)}`);
  console.log(name);
  console.log("=".repeat(70));
  const result = await db.query(`EXPLAIN ANALYZE ${sql}`, params);
  for (const row of result) console.log(row["QUERY PLAN"]);
}

async function timeQuery(db, sql, params = []) {
  const start = performance.now();
  const result = await db.query(sql, params);
  return { time: performance.now() - start, rows: result.length };
}

async function benchmark() {
  console.log("Setting up database...");
  const db = await createMnemonicDB();

  await db.defineAttribute({ ident: "user/name", valueType: "db.type/text", cardinality: "db.cardinality/one" });
  await db.defineAttribute({ ident: "user/email", valueType: "db.type/text", cardinality: "db.cardinality/one" });
  await db.defineAttribute({ ident: "user/birth-year", valueType: "db.type/int4", cardinality: "db.cardinality/one" });
  await db.defineView({ name: "users", attributes: ["user/name", "user/email", "user/birth-year"] });

  const insertedNames = [];
  console.log(`Inserting ${NUM_USERS} users...`);
  for (let i = 0; i < NUM_USERS; i++) {
    const name = randomName();
    insertedNames.push(name);
    await db.query(`INSERT INTO users (name, email, birth_year) VALUES ($1, $2, $3)`,
      [name, `${name.toLowerCase().replace(" ", ".")}.${i}@example.com`, 1950 + Math.floor(Math.random() * 60)]);
    if ((i + 1) % 5000 === 0) console.log(`  ${i + 1}...`);
  }

  const targetName = insertedNames[1000];
  console.log(`\nTarget: "${targetName}"`);

  // Approach: Use scalar subqueries to look up each attribute
  // This forces PostgreSQL to use the PK index for each lookup
  console.log("\n" + "#".repeat(70));
  console.log("# SCALAR SUBQUERY APPROACH");
  console.log("#".repeat(70));

  await explainQuery(db, "Scalar subqueries for attribute lookups",
    `SELECT
       name_t.e AS id,
       name_t.v AS name,
       (SELECT v FROM datoms_text WHERE e = name_t.e AND a = 301 AND retracted_by IS NULL LIMIT 1) AS email,
       (SELECT v FROM datoms_int4 WHERE e = name_t.e AND a = 302 AND retracted_by IS NULL LIMIT 1) AS birth_year
     FROM datoms_text name_t
     WHERE name_t.a = 300 AND name_t.v = $1 AND name_t.retracted_by IS NULL`,
    [targetName]);

  console.log("\n--- Timing: Scalar subqueries ---");
  for (let i = 0; i < 5; i++) {
    const r = await timeQuery(db,
      `SELECT
         name_t.e AS id,
         name_t.v AS name,
         (SELECT v FROM datoms_text WHERE e = name_t.e AND a = 301 AND retracted_by IS NULL LIMIT 1) AS email,
         (SELECT v FROM datoms_int4 WHERE e = name_t.e AND a = 302 AND retracted_by IS NULL LIMIT 1) AS birth_year
       FROM datoms_text name_t
       WHERE name_t.a = 300 AND name_t.v = $1 AND name_t.retracted_by IS NULL`,
      [targetName]);
    console.log(`Run ${i + 1}: ${r.time.toFixed(2)}ms (${r.rows} rows)`);
  }

  // Compare with current view
  console.log("\n--- Timing: Current view ---");
  for (let i = 0; i < 5; i++) {
    const r = await timeQuery(db, `SELECT * FROM users WHERE name = $1`, [targetName]);
    console.log(`Run ${i + 1}: ${r.time.toFixed(2)}ms (${r.rows} rows)`);
  }

  // Now test with birth_year filter (base column)
  console.log("\n" + "#".repeat(70));
  console.log("# COMPARISON: Filter on different columns");
  console.log("#".repeat(70));

  const approaches = [
    ["View: WHERE name = X (non-base)", `SELECT * FROM users WHERE name = $1`, [targetName]],
    ["View: WHERE birth_year = 1985 (base)", `SELECT * FROM users WHERE birth_year = 1985`, []],
    ["Scalar: WHERE name = X", `
      SELECT name_t.e AS id, name_t.v AS name,
        (SELECT v FROM datoms_text WHERE e = name_t.e AND a = 301 AND retracted_by IS NULL LIMIT 1) AS email,
        (SELECT v FROM datoms_int4 WHERE e = name_t.e AND a = 302 AND retracted_by IS NULL LIMIT 1) AS birth_year
      FROM datoms_text name_t
      WHERE name_t.a = 300 AND name_t.v = $1 AND name_t.retracted_by IS NULL`, [targetName]],
    ["Scalar: WHERE birth_year = 1985", `
      SELECT birth_t.e AS id,
        (SELECT v FROM datoms_text WHERE e = birth_t.e AND a = 300 AND retracted_by IS NULL LIMIT 1) AS name,
        (SELECT v FROM datoms_text WHERE e = birth_t.e AND a = 301 AND retracted_by IS NULL LIMIT 1) AS email,
        birth_t.v AS birth_year
      FROM datoms_int4 birth_t
      WHERE birth_t.a = 302 AND birth_t.v = 1985 AND birth_t.retracted_by IS NULL`, []],
  ];

  console.log("\n--- Results ---");
  for (const [name, sql, params] of approaches) {
    const times = [];
    let rows = 0;
    for (let i = 0; i < 5; i++) {
      const r = await timeQuery(db, sql, params);
      times.push(r.time);
      rows = r.rows;
    }
    const avg = times.reduce((a, b) => a + b) / times.length;
    console.log(`${name.padEnd(40)}: ${avg.toFixed(2)}ms (${rows} rows)`);
  }

  // Show EXPLAIN for scalar approach
  await explainQuery(db, "Scalar: WHERE birth_year = 1985",
    `SELECT birth_t.e AS id,
      (SELECT v FROM datoms_text WHERE e = birth_t.e AND a = 300 AND retracted_by IS NULL LIMIT 1) AS name,
      (SELECT v FROM datoms_text WHERE e = birth_t.e AND a = 301 AND retracted_by IS NULL LIMIT 1) AS email,
      birth_t.v AS birth_year
    FROM datoms_int4 birth_t
    WHERE birth_t.a = 302 AND birth_t.v = 1985 AND birth_t.retracted_by IS NULL`, []);

  await db.close();
}

benchmark().catch(console.error);
