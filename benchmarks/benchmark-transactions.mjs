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

async function setupSchema(db) {
  await db.withTransaction(async () => {
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
  });
}

async function clearData(db) {
  // Delete from attribute tables directly (bypasses transaction requirement)
  await db.exec(`DELETE FROM attr_user_name`);
  await db.exec(`DELETE FROM attr_user_email`);
  await db.exec(`DELETE FROM attr_user_birth_year`);
}

async function benchmark() {
  console.log("=".repeat(70));
  console.log("TRANSACTION BATCHING BENCHMARK");
  console.log("=".repeat(70));

  // Test different batch sizes
  const batchSizes = [1, 10, 100, 500, 1000, 5000, NUM_USERS];
  const results = [];

  for (const batchSize of batchSizes) {
    // Create fresh database for each test
    const db = await createMnemonicDB();
    await setupSchema(db);

    const label = batchSize === 1 ? "1 (per-row tx)" :
                  batchSize === NUM_USERS ? `${NUM_USERS} (single tx)` :
                  `${batchSize}`;

    console.log(`\nBatch size: ${label}`);
    const start = performance.now();

    for (let i = 0; i < NUM_USERS; i += batchSize) {
      const batchEnd = Math.min(i + batchSize, NUM_USERS);

      await db.beginTransaction();

      for (let j = i; j < batchEnd; j++) {
        const name = randomName();
        const email = randomEmail(name, j);
        const birthYear = randomBirthYear();

        await db.query(
          `INSERT INTO users (name, email, birth_year) VALUES ($1, $2, $3)`,
          [name, email, birthYear]
        );
      }

      await db.commitTransaction();

      if (batchEnd % 5000 === 0 || batchEnd === NUM_USERS) {
        const elapsed = performance.now() - start;
        console.log(`  ${batchEnd}: ${(elapsed / 1000).toFixed(1)}s (${(batchEnd / (elapsed / 1000)).toFixed(0)} rows/sec)`);
      }
    }

    const totalTime = performance.now() - start;
    const rowsPerSec = NUM_USERS / (totalTime / 1000);
    results.push({ batchSize, totalTime, rowsPerSec });

    await db.close();
  }

  // Summary
  console.log("\n" + "=".repeat(70));
  console.log("SUMMARY");
  console.log("=".repeat(70));
  console.log(`\n${"Batch Size".padEnd(20)} ${"Time".padEnd(12)} ${"Rows/sec".padEnd(12)} Speedup`);
  console.log("-".repeat(60));

  const baselineRate = results[0].rowsPerSec;
  for (const { batchSize, totalTime, rowsPerSec } of results) {
    const label = batchSize === 1 ? "1 (per-row)" :
                  batchSize === NUM_USERS ? "all (single tx)" :
                  `${batchSize}`;
    const speedup = rowsPerSec / baselineRate;
    console.log(
      `${label.padEnd(20)} ${(totalTime / 1000).toFixed(1).padEnd(12)}s ${rowsPerSec.toFixed(0).padEnd(12)} ${speedup.toFixed(1)}x`
    );
  }
}

benchmark().catch(console.error);
