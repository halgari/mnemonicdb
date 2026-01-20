import { createMnemonicDB } from "./dist/index.js";

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
  const targetName = insertedNames[1000]; // Pick one we know exists
  console.log(`\nTarget name for exact match: "${targetName}"`);

  // Count how many have this exact name
  const countResult = await db.query(
    `SELECT COUNT(*) as cnt FROM users WHERE name = $1`,
    [targetName]
  );
  console.log(`Users with this exact name: ${countResult[0].cnt}`);

  // Test exact match
  await explainQuery(
    db,
    "Exact name match (name = 'Alice Smith')",
    `SELECT * FROM users WHERE name = $1`,
    [targetName]
  );

  // Test exact match on email (another text column)
  const targetEmail = `alice.smith.500@example.com`;
  await explainQuery(
    db,
    "Exact email match",
    `SELECT * FROM users WHERE email = $1`,
    [targetEmail]
  );

  // Compare with birth_year exact match (base column)
  await explainQuery(
    db,
    "Exact birth_year match (base column)",
    `SELECT * FROM users WHERE birth_year = 1985`
  );

  // Test combined: exact name AND birth_year range
  await explainQuery(
    db,
    "Combined: exact name AND birth_year > 1980",
    `SELECT * FROM users WHERE name = $1 AND birth_year > 1980`,
    [targetName]
  );

  // Show view definition
  console.log(`\n${"=".repeat(70)}`);
  console.log("VIEW DEFINITION");
  console.log(`${"=".repeat(70)}`);
  const viewDef = await db.query(
    "SELECT definition FROM pg_views WHERE viewname = 'users'"
  );
  console.log(viewDef[0].definition);

  await db.close();
}

benchmark().catch(console.error);
