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

  await db.defineView({
    name: "users",
    attributes: ["user/name", "user/email", "user/birth-year"],
  });

  // Insert users (show progress)
  const userIds = [];
  console.log(`Inserting ${NUM_USERS} users...`);
  const start = performance.now();

  for (let i = 0; i < NUM_USERS; i++) {
    const name = randomName();
    const email = randomEmail(name, i);
    const birthYear = randomBirthYear();

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

  // Pick a sample ID for point lookup
  const sampleId = userIds[Math.floor(Math.random() * userIds.length)];

  // Run EXPLAIN ANALYZE for each query type
  await explainQuery(
    db,
    "Point lookup by ID",
    `SELECT * FROM users WHERE id = $1`,
    [sampleId]
  );

  await explainQuery(
    db,
    "Name prefix search (LIKE 'Alice%')",
    `SELECT * FROM users WHERE name LIKE 'Alice%'`
  );

  await explainQuery(
    db,
    "Range query (birth_year > 1990)",
    `SELECT * FROM users WHERE birth_year > 1990`
  );

  await explainQuery(
    db,
    "Range query with ORDER BY",
    `SELECT * FROM users WHERE birth_year > 1990 ORDER BY birth_year DESC`
  );

  await explainQuery(
    db,
    "COUNT(*)",
    `SELECT COUNT(*) FROM users`
  );

  await explainQuery(
    db,
    "Aggregate (GROUP BY first letter)",
    `SELECT SUBSTRING(name, 1, 1) as first_letter, COUNT(*), AVG(birth_year)
     FROM users GROUP BY SUBSTRING(name, 1, 1) ORDER BY first_letter`
  );

  await explainQuery(
    db,
    "Full table scan",
    `SELECT * FROM users`
  );

  // Also show the view definition for reference
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
