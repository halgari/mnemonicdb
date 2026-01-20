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

async function timeQuery(db, sql, params = [], runs = 5) {
  const times = [];
  let rows = 0;
  for (let i = 0; i < runs; i++) {
    const start = performance.now();
    const result = await db.query(sql, params);
    times.push(performance.now() - start);
    rows = result.length;
  }
  const avg = times.reduce((a, b) => a + b) / times.length;
  const min = Math.min(...times);
  return { avg, min, rows };
}

async function explainQuery(db, name, sql, params = []) {
  console.log(`\n${"=".repeat(70)}`);
  console.log(`EXPLAIN: ${name}`);
  console.log("=".repeat(70));
  const result = await db.query(`EXPLAIN ANALYZE ${sql}`, params);
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

  // Create view using current join-based approach
  await db.defineView({
    name: "users",
    attributes: ["user/name", "user/email", "user/birth-year"],
  });

  // Get attribute IDs for building pivot queries
  const nameAttrId = await db.attrId("user/name");
  const emailAttrId = await db.attrId("user/email");
  const birthYearAttrId = await db.attrId("user/birth-year");

  console.log(`Attribute IDs: name=${nameAttrId}, email=${emailAttrId}, birth_year=${birthYearAttrId}`);

  // Insert users
  const insertedNames = [];
  console.log(`\nInserting ${NUM_USERS} users...`);
  const start = performance.now();

  for (let i = 0; i < NUM_USERS; i++) {
    const name = randomName();
    const email = randomEmail(name, i);
    const birthYear = randomBirthYear();

    insertedNames.push(name);

    await db.query(
      `INSERT INTO users_current (name, email, birth_year) VALUES ($1, $2, $3)`,
      [name, email, birthYear]
    );

    if ((i + 1) % 5000 === 0) {
      const elapsed = performance.now() - start;
      console.log(`  ${i + 1} inserted (${((i + 1) / (elapsed / 1000)).toFixed(0)} rows/sec)`);
    }
  }
  console.log(`Insert complete: ${((performance.now() - start) / 1000).toFixed(1)}s`);

  const targetName = insertedNames[1000];
  console.log(`\nTarget name: "${targetName}"`);

  console.log("\n" + "#".repeat(70));
  console.log("# VIEW STRUCTURE");
  console.log("#".repeat(70));
  console.log("Generated views:");
  console.log("  - users          (dispatches based on mnemonic.as_of_tx)");
  console.log("  - users_current  (fast, simple retracted_by IS NULL)");
  console.log("  - users_history  (temporal, full mnemonic_datom_visible check)");

  // Test queries
  console.log("\n" + "#".repeat(70));
  console.log("# BENCHMARK: Full table scan (SELECT * FROM view)");
  console.log("#".repeat(70));

  const approaches = [
    ["users (dispatch->current)", "SELECT * FROM users"],
    ["users_current (direct)", "SELECT * FROM users_current"],
    ["users_history (direct)", "SELECT * FROM users_history"],
  ];

  for (const [name, sql] of approaches) {
    const r = await timeQuery(db, sql);
    console.log(`${name.padEnd(25)}: avg ${r.avg.toFixed(1)}ms, min ${r.min.toFixed(1)}ms (${r.rows} rows)`);
  }

  // Filtered queries
  console.log("\n" + "#".repeat(70));
  console.log("# BENCHMARK: Filtered query (WHERE name = X)");
  console.log("#".repeat(70));

  const filterApproaches = [
    ["users (dispatch)", `SELECT * FROM users WHERE name = $1`],
    ["users_current (direct)", `SELECT * FROM users_current WHERE name = $1`],
    ["users_history (direct)", `SELECT * FROM users_history WHERE name = $1`],
  ];

  for (const [name, sql] of filterApproaches) {
    const r = await timeQuery(db, sql, [targetName]);
    console.log(`${name.padEnd(25)}: avg ${r.avg.toFixed(1)}ms, min ${r.min.toFixed(1)}ms (${r.rows} rows)`);
  }

  // Range query on birth_year
  console.log("\n" + "#".repeat(70));
  console.log("# BENCHMARK: Range query (WHERE birth_year > 1990)");
  console.log("#".repeat(70));

  const rangeApproaches = [
    ["users (dispatch)", `SELECT * FROM users WHERE birth_year > 1990`],
    ["users_current (direct)", `SELECT * FROM users_current WHERE birth_year > 1990`],
    ["users_history (direct)", `SELECT * FROM users_history WHERE birth_year > 1990`],
  ];

  for (const [name, sql] of rangeApproaches) {
    const r = await timeQuery(db, sql);
    console.log(`${name.padEnd(25)}: avg ${r.avg.toFixed(1)}ms, min ${r.min.toFixed(1)}ms (${r.rows} rows)`);
  }

  // EXPLAIN ANALYZE for insight
  console.log("\n" + "#".repeat(70));
  console.log("# EXPLAIN ANALYZE");
  console.log("#".repeat(70));

  await explainQuery(db, "users (dispatch, no T): name filter", `SELECT * FROM users WHERE name = $1`, [targetName]);
  await explainQuery(db, "users_current: name filter", `SELECT * FROM users_current WHERE name = $1`, [targetName]);

  // Test temporal dispatch - set as_of_tx and verify it routes to _history
  console.log("\n" + "#".repeat(70));
  console.log("# TEMPORAL DISPATCH TEST");
  console.log("#".repeat(70));

  // Get a transaction ID to use for as-of query
  const txResult = await db.query("SELECT MAX(tx) as max_tx FROM datoms");
  const maxTx = txResult[0].max_tx;
  console.log(`Setting as_of_tx to ${maxTx}...`);

  await db.query("SELECT set_config('mnemonic.as_of_tx', $1, false)", [maxTx.toString()]);

  // Now the dispatch view should route to _history
  console.log("\n--- With as_of_tx set (should use _history) ---");
  const temporalApproaches = [
    ["users (dispatch->history)", `SELECT * FROM users WHERE name = $1`],
    ["users_history (direct)", `SELECT * FROM users_history WHERE name = $1`],
  ];
  for (const [name, sql] of temporalApproaches) {
    const r = await timeQuery(db, sql, [targetName]);
    console.log(`${name.padEnd(30)}: avg ${r.avg.toFixed(1)}ms, min ${r.min.toFixed(1)}ms (${r.rows} rows)`);
  }

  await explainQuery(db, "users (dispatch, with T): name filter", `SELECT * FROM users WHERE name = $1`, [targetName]);

  // Reset
  await db.query("SELECT set_config('mnemonic.as_of_tx', '', false)");

  await db.close();
}

benchmark().catch(console.error);
