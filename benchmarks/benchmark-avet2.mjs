import { createMnemonicDB } from "../dist/index.js";

const NUM_USERS = 20_000;

const firstNames = [
  "Alice", "Bob", "Charlie", "David", "Eve", "Frank", "Grace", "Henry",
  "Ivy", "Jack", "Kate", "Leo", "Mia", "Noah", "Olivia", "Paul",
];

const lastNames = [
  "Smith", "Johnson", "Williams", "Brown", "Jones", "Garcia", "Miller",
  "Davis", "Rodriguez", "Martinez",
];

function randomName() {
  const first = firstNames[Math.floor(Math.random() * firstNames.length)];
  const last = lastNames[Math.floor(Math.random() * lastNames.length)];
  return `${first} ${last}`;
}

function randomEmail(name, idx) {
  return `${name.toLowerCase().replace(" ", ".")}.${idx}@example.com`;
}

function randomBirthYear() {
  return 1950 + Math.floor(Math.random() * 60);
}

async function explainQuery(db, name, sql, params = []) {
  console.log(`\n${"=".repeat(70)}`);
  console.log(`${name}`);
  console.log(`${"=".repeat(70)}`);

  const explainSql = `EXPLAIN ANALYZE ${sql}`;
  const result = await db.query(explainSql, params);

  for (const row of result) {
    console.log(row["QUERY PLAN"]);
  }
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
      [name, randomEmail(name, i), randomBirthYear()]);
    if ((i + 1) % 5000 === 0) console.log(`  ${i + 1}...`);
  }

  const targetName = insertedNames[1000];
  console.log(`\nTarget: "${targetName}"`);

  const nameAttrId = 300;
  const emailAttrId = 301;
  const birthYearAttrId = 302;

  // Check what the AVET index actually returns for our target
  console.log("\n" + "#".repeat(70));
  console.log("# DIRECT AVET INDEX LOOKUP");
  console.log("#".repeat(70));

  await explainQuery(db, "Direct AVET lookup: just find entities with name='X'",
    `SELECT e FROM datoms_text WHERE a = $1 AND v = $2 AND retracted_by IS NULL`,
    [nameAttrId, targetName]);

  // This should be fast - just using the AVET index
  const directResult = await timeQuery(db,
    `SELECT e FROM datoms_text WHERE a = $1 AND v = $2 AND retracted_by IS NULL`,
    [nameAttrId, targetName]);
  console.log(`Direct AVET lookup: ${directResult.time.toFixed(2)}ms (${directResult.rows} entities)`);

  // Now try using that as a subquery with OFFSET 0 to force materialization
  console.log("\n" + "#".repeat(70));
  console.log("# FORCE FILTER-FIRST WITH OFFSET 0 TRICK");
  console.log("#".repeat(70));

  await explainQuery(db, "Subquery with OFFSET 0 (forces materialization)",
    `SELECT
       e.id,
       name_t.v AS name,
       email_t.v AS email,
       birth_t.v AS birth_year
     FROM (
       SELECT e AS id FROM datoms_text
       WHERE a = $1 AND v = $2 AND retracted_by IS NULL
       OFFSET 0
     ) e
     JOIN datoms_text name_t ON e.id = name_t.e AND name_t.a = $1 AND name_t.retracted_by IS NULL
     JOIN datoms_text email_t ON e.id = email_t.e AND email_t.a = $3 AND email_t.retracted_by IS NULL
     JOIN datoms_int4 birth_t ON e.id = birth_t.e AND birth_t.a = $4 AND birth_t.retracted_by IS NULL`,
    [nameAttrId, targetName, emailAttrId, birthYearAttrId]);

  // Try with join_collapse_limit = 1
  console.log("\n" + "#".repeat(70));
  console.log("# DISABLE JOIN REORDERING");
  console.log("#".repeat(70));

  await db.exec("SET join_collapse_limit = 1");

  await explainQuery(db, "With join_collapse_limit = 1 (preserve written order)",
    `SELECT
       name_t.e AS id,
       name_t.v AS name,
       email_t.v AS email,
       birth_t.v AS birth_year
     FROM datoms_text name_t
     JOIN datoms_text email_t ON name_t.e = email_t.e AND email_t.a = $2 AND email_t.retracted_by IS NULL
     JOIN datoms_int4 birth_t ON name_t.e = birth_t.e AND birth_t.a = $3 AND birth_t.retracted_by IS NULL
     WHERE name_t.a = $1 AND name_t.v = $4 AND name_t.retracted_by IS NULL`,
    [nameAttrId, emailAttrId, birthYearAttrId, targetName]);

  await db.exec("RESET join_collapse_limit");

  // Timing comparison
  console.log("\n" + "#".repeat(70));
  console.log("# TIMING COMPARISON");
  console.log("#".repeat(70));

  const queries = [
    ["Current view", `SELECT * FROM users WHERE name = $1`, [targetName]],
    ["OFFSET 0 trick", `
      SELECT e.id, name_t.v AS name, email_t.v AS email, birth_t.v AS birth_year
      FROM (SELECT e AS id FROM datoms_text WHERE a = $1 AND v = $2 AND retracted_by IS NULL OFFSET 0) e
      JOIN datoms_text name_t ON e.id = name_t.e AND name_t.a = $1 AND name_t.retracted_by IS NULL
      JOIN datoms_text email_t ON e.id = email_t.e AND email_t.a = $3 AND email_t.retracted_by IS NULL
      JOIN datoms_int4 birth_t ON e.id = birth_t.e AND birth_t.a = $4 AND birth_t.retracted_by IS NULL`,
      [nameAttrId, targetName, emailAttrId, birthYearAttrId]],
  ];

  for (let run = 0; run < 3; run++) {
    console.log(`\n--- Run ${run + 1} ---`);
    for (const [name, sql, params] of queries) {
      const r = await timeQuery(db, sql, params);
      console.log(`${name.padEnd(20)}: ${r.time.toFixed(2)}ms (${r.rows} rows)`);
    }
  }

  // Now test with join_collapse_limit = 1 for timing
  console.log("\n--- With join_collapse_limit = 1 ---");
  await db.exec("SET join_collapse_limit = 1");

  for (let run = 0; run < 3; run++) {
    const r = await timeQuery(db,
      `SELECT name_t.e AS id, name_t.v AS name, email_t.v AS email, birth_t.v AS birth_year
       FROM datoms_text name_t
       JOIN datoms_text email_t ON name_t.e = email_t.e AND email_t.a = $2 AND email_t.retracted_by IS NULL
       JOIN datoms_int4 birth_t ON name_t.e = birth_t.e AND birth_t.a = $3 AND birth_t.retracted_by IS NULL
       WHERE name_t.a = $1 AND name_t.v = $4 AND name_t.retracted_by IS NULL`,
      [nameAttrId, emailAttrId, birthYearAttrId, targetName]);
    console.log(`Forced order (run ${run + 1}): ${r.time.toFixed(2)}ms (${r.rows} rows)`);
  }

  await db.close();
}

benchmark().catch(console.error);
