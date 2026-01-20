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

async function benchmark() {
  console.log("=".repeat(70));
  console.log("TRANSACTION BATCHING BENCHMARK");
  console.log("=".repeat(70));

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

  // Test 1: Individual inserts (auto-commit each)
  console.log(`\n1. Individual inserts (auto-commit per row) - ${NUM_USERS} users`);
  let start = performance.now();

  for (let i = 0; i < NUM_USERS; i++) {
    const name = randomName();
    const email = randomEmail(name, i);
    const birthYear = randomBirthYear();

    await db.query(
      `INSERT INTO users (name, email, birth_year) VALUES ($1, $2, $3)`,
      [name, email, birthYear]
    );

    if ((i + 1) % 5000 === 0) {
      const elapsed = performance.now() - start;
      console.log(`  ${i + 1}: ${(elapsed / 1000).toFixed(1)}s (${((i + 1) / (elapsed / 1000)).toFixed(0)} rows/sec)`);
    }
  }

  const individualTime = performance.now() - start;
  console.log(`  Total: ${(individualTime / 1000).toFixed(1)}s (${(NUM_USERS / (individualTime / 1000)).toFixed(0)} rows/sec)`);

  // Clear for next test
  await db.exec(`DELETE FROM datoms_text WHERE a IN (300, 301)`);
  await db.exec(`DELETE FROM datoms_int4 WHERE a = 302`);

  // Test 2: Batched in single transaction
  console.log(`\n2. Single transaction (BEGIN...COMMIT) - ${NUM_USERS} users`);
  start = performance.now();

  await db.exec("BEGIN");

  for (let i = 0; i < NUM_USERS; i++) {
    const name = randomName();
    const email = randomEmail(name, i);
    const birthYear = randomBirthYear();

    await db.query(
      `INSERT INTO users (name, email, birth_year) VALUES ($1, $2, $3)`,
      [name, email, birthYear]
    );

    if ((i + 1) % 5000 === 0) {
      const elapsed = performance.now() - start;
      console.log(`  ${i + 1}: ${(elapsed / 1000).toFixed(1)}s (${((i + 1) / (elapsed / 1000)).toFixed(0)} rows/sec)`);
    }
  }

  await db.exec("COMMIT");

  const batchedTime = performance.now() - start;
  console.log(`  Total: ${(batchedTime / 1000).toFixed(1)}s (${(NUM_USERS / (batchedTime / 1000)).toFixed(0)} rows/sec)`);

  // Clear for next test
  await db.exec(`DELETE FROM datoms_text WHERE a IN (300, 301)`);
  await db.exec(`DELETE FROM datoms_int4 WHERE a = 302`);

  // Test 3: Chunked transactions (commit every 1000 rows)
  console.log(`\n3. Chunked transactions (commit every 1000 rows) - ${NUM_USERS} users`);
  start = performance.now();

  const CHUNK_SIZE = 1000;
  await db.exec("BEGIN");

  for (let i = 0; i < NUM_USERS; i++) {
    const name = randomName();
    const email = randomEmail(name, i);
    const birthYear = randomBirthYear();

    await db.query(
      `INSERT INTO users (name, email, birth_year) VALUES ($1, $2, $3)`,
      [name, email, birthYear]
    );

    if ((i + 1) % CHUNK_SIZE === 0) {
      await db.exec("COMMIT");
      if (i + 1 < NUM_USERS) {
        await db.exec("BEGIN");
      }
    }

    if ((i + 1) % 5000 === 0) {
      const elapsed = performance.now() - start;
      console.log(`  ${i + 1}: ${(elapsed / 1000).toFixed(1)}s (${((i + 1) / (elapsed / 1000)).toFixed(0)} rows/sec)`);
    }
  }

  // Commit any remaining
  if (NUM_USERS % CHUNK_SIZE !== 0) {
    await db.exec("COMMIT");
  }

  const chunkedTime = performance.now() - start;
  console.log(`  Total: ${(chunkedTime / 1000).toFixed(1)}s (${(NUM_USERS / (chunkedTime / 1000)).toFixed(0)} rows/sec)`);

  // Test 4: Using pglite's transaction API (if available)
  console.log(`\n4. Using db.transaction() API - ${NUM_USERS} users`);

  // Clear for next test
  await db.exec(`DELETE FROM datoms_text WHERE a IN (300, 301)`);
  await db.exec(`DELETE FROM datoms_int4 WHERE a = 302`);

  start = performance.now();

  // pglite has a transaction method
  await db.db.transaction(async (tx) => {
    for (let i = 0; i < NUM_USERS; i++) {
      const name = randomName();
      const email = randomEmail(name, i);
      const birthYear = randomBirthYear();

      await tx.query(
        `INSERT INTO users (name, email, birth_year) VALUES ($1, $2, $3)`,
        [name, email, birthYear]
      );

      if ((i + 1) % 5000 === 0) {
        const elapsed = performance.now() - start;
        console.log(`  ${i + 1}: ${(elapsed / 1000).toFixed(1)}s (${((i + 1) / (elapsed / 1000)).toFixed(0)} rows/sec)`);
      }
    }
  });

  const txApiTime = performance.now() - start;
  console.log(`  Total: ${(txApiTime / 1000).toFixed(1)}s (${(NUM_USERS / (txApiTime / 1000)).toFixed(0)} rows/sec)`);

  // Summary
  console.log("\n" + "=".repeat(70));
  console.log("SUMMARY");
  console.log("=".repeat(70));
  console.log(`Individual (auto-commit):  ${(individualTime / 1000).toFixed(1)}s  (${(NUM_USERS / (individualTime / 1000)).toFixed(0)} rows/sec)`);
  console.log(`Single transaction:        ${(batchedTime / 1000).toFixed(1)}s  (${(NUM_USERS / (batchedTime / 1000)).toFixed(0)} rows/sec)`);
  console.log(`Chunked (1000/commit):     ${(chunkedTime / 1000).toFixed(1)}s  (${(NUM_USERS / (chunkedTime / 1000)).toFixed(0)} rows/sec)`);
  console.log(`db.transaction() API:      ${(txApiTime / 1000).toFixed(1)}s  (${(NUM_USERS / (txApiTime / 1000)).toFixed(0)} rows/sec)`);
  console.log(`\nSpeedup (single tx vs individual): ${(individualTime / batchedTime).toFixed(1)}x`);

  await db.close();
}

benchmark().catch(console.error);
