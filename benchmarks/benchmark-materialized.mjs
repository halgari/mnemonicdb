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
  console.log("Setting up database and inserting data...");
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

  // Create regular view first (for inserting data)
  await db.defineView({
    name: "users",
    attributes: ["user/name", "user/email", "user/birth-year"],
  });

  // Insert users
  const userIds = [];
  const insertedNames = [];
  console.log(`Inserting ${NUM_USERS} users...`);
  const start = performance.now();

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
      console.log(`  ${i + 1} inserted...`);
    }
  }
  console.log(`Insert complete: ${((performance.now() - start) / 1000).toFixed(1)}s`);

  // Pick a specific name that exists
  const targetName = insertedNames[1000];
  console.log(`\nTarget name for queries: "${targetName}"`);

  // Count how many have this exact name
  const countResult = await db.query(
    `SELECT COUNT(*) as cnt FROM users WHERE name = $1`,
    [targetName]
  );
  console.log(`Users with this exact name: ${countResult[0].cnt}`);

  // Now create the materialized view
  console.log("\n" + "=".repeat(70));
  console.log("Creating MATERIALIZED VIEW with indexes...");
  console.log("=".repeat(70));

  let matStart = performance.now();

  // Drop the regular view and create materialized version
  await db.exec(`DROP VIEW IF EXISTS users CASCADE`);

  // Get the view definition from mnemonic_views
  const viewDef = await db.query(
    `SELECT * FROM mnemonic_view_attributes WHERE view_name = 'users' ORDER BY required DESC, attribute_ident`
  );

  // Build the materialized view manually based on the same structure
  await db.exec(`
    CREATE MATERIALIZED VIEW users_mat AS
    SELECT
      j1.e AS id,
      j1.v AS birth_year,
      j2.v AS email,
      j3.v AS name
    FROM datoms_int4 j1
    JOIN datoms_text j2 ON j1.e = j2.e AND j2.a = 301 AND j2.retracted_by IS NULL
    JOIN datoms_text j3 ON j1.e = j3.e AND j3.a = 300 AND j3.retracted_by IS NULL
    WHERE j1.a = 302 AND j1.retracted_by IS NULL
  `);

  console.log(`Materialized view created: ${(performance.now() - matStart).toFixed(2)}ms`);

  // Create indexes on all columns
  matStart = performance.now();
  await db.exec(`CREATE INDEX users_mat_id_idx ON users_mat (id)`);
  await db.exec(`CREATE INDEX users_mat_name_idx ON users_mat (name)`);
  await db.exec(`CREATE INDEX users_mat_email_idx ON users_mat (email)`);
  await db.exec(`CREATE INDEX users_mat_birth_year_idx ON users_mat (birth_year)`);
  console.log(`Indexes created: ${(performance.now() - matStart).toFixed(2)}ms`);

  // Also create a btree index for prefix searches
  await db.exec(`CREATE INDEX users_mat_name_pattern_idx ON users_mat (name text_pattern_ops)`);
  console.log(`Pattern index created for LIKE queries`);

  // Run EXPLAIN ANALYZE on materialized view
  console.log("\n" + "#".repeat(70));
  console.log("# MATERIALIZED VIEW QUERIES");
  console.log("#".repeat(70));

  await explainQuery(
    db,
    "Exact name match (materialized)",
    `SELECT * FROM users_mat WHERE name = $1`,
    [targetName]
  );

  await explainQuery(
    db,
    "Name prefix LIKE (materialized)",
    `SELECT * FROM users_mat WHERE name LIKE 'Alice%'`
  );

  await explainQuery(
    db,
    "Exact email match (materialized)",
    `SELECT * FROM users_mat WHERE email = $1`,
    [`alice.smith.500@example.com`]
  );

  await explainQuery(
    db,
    "Exact birth_year match (materialized)",
    `SELECT * FROM users_mat WHERE birth_year = 1985`
  );

  await explainQuery(
    db,
    "Point lookup by ID (materialized)",
    `SELECT * FROM users_mat WHERE id = $1`,
    [userIds[500]]
  );

  await explainQuery(
    db,
    "Combined: name AND birth_year range (materialized)",
    `SELECT * FROM users_mat WHERE name = $1 AND birth_year > 1980`,
    [targetName]
  );

  // Show materialized view size
  console.log("\n" + "=".repeat(70));
  console.log("STORAGE COMPARISON");
  console.log("=".repeat(70));

  const sizes = await db.query(`
    SELECT
      relname as name,
      pg_size_pretty(pg_relation_size(oid)) as size
    FROM pg_class
    WHERE relname IN ('users_mat', 'datoms_text', 'datoms_int4')
    ORDER BY pg_relation_size(oid) DESC
  `);

  for (const row of sizes) {
    console.log(`${row.name}: ${row.size}`);
  }

  // Compare refresh time
  console.log("\n" + "=".repeat(70));
  console.log("REFRESH MATERIALIZED VIEW");
  console.log("=".repeat(70));

  const refreshStart = performance.now();
  await db.exec(`REFRESH MATERIALIZED VIEW users_mat`);
  console.log(`Refresh time: ${(performance.now() - refreshStart).toFixed(2)}ms`);

  await db.close();
}

benchmark().catch(console.error);
