import { createMnemonicDB } from "../dist/index.js";

const NUM_USERS = 20_000;

// Generate random data
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
  return 1950 + Math.floor(Math.random() * 60); // 1950-2009
}

async function benchmark() {
  console.log("=".repeat(60));
  console.log("MnemonicDB Benchmark");
  console.log("=".repeat(60));
  console.log();

  // Setup
  console.log("Setting up database...");
  let start = performance.now();
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

  console.log(`Schema setup: ${(performance.now() - start).toFixed(2)}ms`);
  console.log();

  // Insert users
  console.log(`Inserting ${NUM_USERS.toLocaleString()} users...`);
  start = performance.now();

  const userIds = [];
  const userNames = [];

  for (let i = 0; i < NUM_USERS; i++) {
    const name = randomName();
    const email = randomEmail(name, i);
    const birthYear = randomBirthYear();

    userNames.push(name);

    const result = await db.query(
      `INSERT INTO users (name, email, birth_year) VALUES ($1, $2, $3) RETURNING id`,
      [name, email, birthYear]
    );
    userIds.push(result[0].id);

    if ((i + 1) % 5000 === 0) {
      const elapsed = performance.now() - start;
      const rate = (i + 1) / (elapsed / 1000);
      console.log(`  ${i + 1} inserted (${rate.toFixed(0)} rows/sec)`);
    }
  }

  const insertTime = performance.now() - start;
  console.log(`Insert complete: ${insertTime.toFixed(2)}ms (${(NUM_USERS / (insertTime / 1000)).toFixed(0)} rows/sec)`);
  console.log();

  // Benchmark: Point lookups by ID
  console.log("Benchmark: Point lookups (100 random IDs)...");
  const sampleIds = [];
  for (let i = 0; i < 100; i++) {
    sampleIds.push(userIds[Math.floor(Math.random() * userIds.length)]);
  }

  start = performance.now();
  for (const id of sampleIds) {
    await db.query(`SELECT * FROM users WHERE id = $1`, [id]);
  }
  const pointLookupTime = performance.now() - start;
  console.log(`  Total: ${pointLookupTime.toFixed(2)}ms`);
  console.log(`  Avg per lookup: ${(pointLookupTime / 100).toFixed(2)}ms`);
  console.log();

  // Benchmark: Name prefix search
  console.log("Benchmark: Name prefix search (LIKE 'Alice%')...");
  start = performance.now();
  const aliceResult = await db.query(`SELECT * FROM users WHERE name LIKE 'Alice%'`);
  const namePrefixTime = performance.now() - start;
  console.log(`  Found ${aliceResult.length} users`);
  console.log(`  Time: ${namePrefixTime.toFixed(2)}ms`);
  console.log();

  // Benchmark: Multiple name prefixes
  console.log("Benchmark: Multiple name prefix searches...");
  const prefixes = ["Bob", "Charlie", "David", "Eve", "Frank"];
  start = performance.now();
  for (const prefix of prefixes) {
    await db.query(`SELECT * FROM users WHERE name LIKE $1`, [`${prefix}%`]);
  }
  const multiPrefixTime = performance.now() - start;
  console.log(`  5 prefix searches: ${multiPrefixTime.toFixed(2)}ms`);
  console.log(`  Avg per search: ${(multiPrefixTime / 5).toFixed(2)}ms`);
  console.log();

  // Benchmark: Birth year range (users born after 1990 = under ~35)
  console.log("Benchmark: Range query (birth_year > 1990)...");
  start = performance.now();
  const youngUsers = await db.query(`SELECT * FROM users WHERE birth_year > 1990`);
  const rangeTime = performance.now() - start;
  console.log(`  Found ${youngUsers.length} users`);
  console.log(`  Time: ${rangeTime.toFixed(2)}ms`);
  console.log();

  // Benchmark: Range with ordering
  console.log("Benchmark: Range query with ORDER BY (birth_year > 1990 ORDER BY birth_year)...");
  start = performance.now();
  const youngUsersSorted = await db.query(
    `SELECT * FROM users WHERE birth_year > 1990 ORDER BY birth_year DESC`
  );
  const rangeOrderTime = performance.now() - start;
  console.log(`  Found ${youngUsersSorted.length} users`);
  console.log(`  Time: ${rangeOrderTime.toFixed(2)}ms`);
  console.log();

  // Benchmark: Count query
  console.log("Benchmark: COUNT(*) query...");
  start = performance.now();
  const countResult = await db.query(`SELECT COUNT(*) as count FROM users`);
  const countTime = performance.now() - start;
  console.log(`  Count: ${countResult[0].count}`);
  console.log(`  Time: ${countTime.toFixed(2)}ms`);
  console.log();

  // Benchmark: Aggregate query
  console.log("Benchmark: Aggregate query (AVG birth_year by name prefix)...");
  start = performance.now();
  const aggResult = await db.query(`
    SELECT
      SUBSTRING(name, 1, 1) as first_letter,
      COUNT(*) as count,
      AVG(birth_year) as avg_birth_year
    FROM users
    GROUP BY SUBSTRING(name, 1, 1)
    ORDER BY first_letter
  `);
  const aggTime = performance.now() - start;
  console.log(`  Groups: ${aggResult.length}`);
  console.log(`  Time: ${aggTime.toFixed(2)}ms`);
  console.log();

  // Benchmark: Full table scan
  console.log("Benchmark: Full table scan (SELECT *)...");
  start = performance.now();
  const allUsers = await db.query(`SELECT * FROM users`);
  const fullScanTime = performance.now() - start;
  console.log(`  Rows: ${allUsers.length}`);
  console.log(`  Time: ${fullScanTime.toFixed(2)}ms`);
  console.log();

  // Summary
  console.log("=".repeat(60));
  console.log("Summary");
  console.log("=".repeat(60));
  console.log(`Total users:          ${NUM_USERS.toLocaleString()}`);
  console.log(`Insert rate:          ${(NUM_USERS / (insertTime / 1000)).toFixed(0)} rows/sec`);
  console.log(`Point lookup (avg):   ${(pointLookupTime / 100).toFixed(2)}ms`);
  console.log(`Name prefix search:   ${namePrefixTime.toFixed(2)}ms`);
  console.log(`Range query:          ${rangeTime.toFixed(2)}ms`);
  console.log(`Range + ORDER BY:     ${rangeOrderTime.toFixed(2)}ms`);
  console.log(`COUNT(*):             ${countTime.toFixed(2)}ms`);
  console.log(`Aggregate (GROUP BY): ${aggTime.toFixed(2)}ms`);
  console.log(`Full scan:            ${fullScanTime.toFixed(2)}ms`);

  await db.close();
}

benchmark().catch(console.error);
