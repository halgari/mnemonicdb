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
  console.log(`${name}`);
  console.log(`${"=".repeat(70)}`);
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

  const nameAttrId = 300;
  const emailAttrId = 301;
  const birthYearAttrId = 302;

  // Baseline before EAVT indexes
  console.log("\n" + "#".repeat(70));
  console.log("# BEFORE EAVT INDEXES");
  console.log("#".repeat(70));

  await explainQuery(db, "Query using AVET filter, then join",
    `SELECT name_t.e AS id, name_t.v AS name, email_t.v AS email, birth_t.v AS birth_year
     FROM datoms_text name_t
     JOIN datoms_text email_t ON name_t.e = email_t.e AND email_t.a = $2 AND email_t.retracted_by IS NULL
     JOIN datoms_int4 birth_t ON name_t.e = birth_t.e AND birth_t.a = $3 AND birth_t.retracted_by IS NULL
     WHERE name_t.a = $1 AND name_t.v = $4 AND name_t.retracted_by IS NULL`,
    [nameAttrId, emailAttrId, birthYearAttrId, targetName]);

  console.log("\n--- Timing (before EAVT) ---");
  for (let i = 0; i < 3; i++) {
    const r = await timeQuery(db, `SELECT * FROM users WHERE name = $1`, [targetName]);
    console.log(`Run ${i + 1}: ${r.time.toFixed(2)}ms (${r.rows} rows)`);
  }

  // Add EAVT indexes
  console.log("\n" + "#".repeat(70));
  console.log("# ADDING EAVT INDEXES");
  console.log("#".repeat(70));

  const indexStart = performance.now();

  // EAVT index: (e, a) is enough for joining on entity + attribute
  // Including v and tx makes it a covering index for most queries
  await db.exec(`CREATE INDEX datoms_text_eavt ON datoms_text (e, a, v, tx) WHERE retracted_by IS NULL`);
  await db.exec(`CREATE INDEX datoms_int4_eavt ON datoms_int4 (e, a, v, tx) WHERE retracted_by IS NULL`);
  await db.exec(`CREATE INDEX datoms_ref_eavt ON datoms_ref (e, a, v, tx) WHERE retracted_by IS NULL`);

  console.log(`EAVT indexes created: ${(performance.now() - indexStart).toFixed(2)}ms`);

  // After EAVT indexes
  console.log("\n" + "#".repeat(70));
  console.log("# AFTER EAVT INDEXES");
  console.log("#".repeat(70));

  await explainQuery(db, "Same query with EAVT indexes available",
    `SELECT name_t.e AS id, name_t.v AS name, email_t.v AS email, birth_t.v AS birth_year
     FROM datoms_text name_t
     JOIN datoms_text email_t ON name_t.e = email_t.e AND email_t.a = $2 AND email_t.retracted_by IS NULL
     JOIN datoms_int4 birth_t ON name_t.e = birth_t.e AND birth_t.a = $3 AND birth_t.retracted_by IS NULL
     WHERE name_t.a = $1 AND name_t.v = $4 AND name_t.retracted_by IS NULL`,
    [nameAttrId, emailAttrId, birthYearAttrId, targetName]);

  console.log("\n--- Timing (after EAVT) ---");
  for (let i = 0; i < 3; i++) {
    const r = await timeQuery(db, `SELECT * FROM users WHERE name = $1`, [targetName]);
    console.log(`Run ${i + 1}: ${r.time.toFixed(2)}ms (${r.rows} rows)`);
  }

  // Now test with forced join order to use AVET for filter, EAVT for lookups
  console.log("\n" + "#".repeat(70));
  console.log("# OPTIMIZED QUERY: AVET filter -> EAVT lookups");
  console.log("#".repeat(70));

  await db.exec("SET join_collapse_limit = 1");

  await explainQuery(db, "Forced order: filter by name first (AVET), then lookup others (EAVT)",
    `SELECT name_t.e AS id, name_t.v AS name, email_t.v AS email, birth_t.v AS birth_year
     FROM datoms_text name_t
     JOIN datoms_text email_t ON name_t.e = email_t.e AND email_t.a = $2 AND email_t.retracted_by IS NULL
     JOIN datoms_int4 birth_t ON name_t.e = birth_t.e AND birth_t.a = $3 AND birth_t.retracted_by IS NULL
     WHERE name_t.a = $1 AND name_t.v = $4 AND name_t.retracted_by IS NULL`,
    [nameAttrId, emailAttrId, birthYearAttrId, targetName]);

  console.log("\n--- Timing (forced order + EAVT) ---");
  for (let i = 0; i < 3; i++) {
    const r = await timeQuery(db,
      `SELECT name_t.e AS id, name_t.v AS name, email_t.v AS email, birth_t.v AS birth_year
       FROM datoms_text name_t
       JOIN datoms_text email_t ON name_t.e = email_t.e AND email_t.a = $2 AND email_t.retracted_by IS NULL
       JOIN datoms_int4 birth_t ON name_t.e = birth_t.e AND birth_t.a = $3 AND birth_t.retracted_by IS NULL
       WHERE name_t.a = $1 AND name_t.v = $4 AND name_t.retracted_by IS NULL`,
      [nameAttrId, emailAttrId, birthYearAttrId, targetName]);
    console.log(`Run ${i + 1}: ${r.time.toFixed(2)}ms (${r.rows} rows)`);
  }

  await db.exec("RESET join_collapse_limit");

  // Compare all approaches
  console.log("\n" + "#".repeat(70));
  console.log("# FINAL COMPARISON");
  console.log("#".repeat(70));

  const approaches = [
    ["Current view (unchanged)", `SELECT * FROM users WHERE name = $1`, [targetName]],
    ["Direct query (with EAVT)",
      `SELECT name_t.e AS id, name_t.v AS name, email_t.v AS email, birth_t.v AS birth_year
       FROM datoms_text name_t
       JOIN datoms_text email_t ON name_t.e = email_t.e AND email_t.a = ${emailAttrId} AND email_t.retracted_by IS NULL
       JOIN datoms_int4 birth_t ON name_t.e = birth_t.e AND birth_t.a = ${birthYearAttrId} AND birth_t.retracted_by IS NULL
       WHERE name_t.a = ${nameAttrId} AND name_t.v = $1 AND name_t.retracted_by IS NULL`,
      [targetName]],
  ];

  for (const [name, sql, params] of approaches) {
    const times = [];
    for (let i = 0; i < 5; i++) {
      const r = await timeQuery(db, sql, params);
      times.push(r.time);
    }
    const avg = times.reduce((a, b) => a + b) / times.length;
    console.log(`${name.padEnd(30)}: ${avg.toFixed(2)}ms avg`);
  }

  await db.close();
}

benchmark().catch(console.error);
