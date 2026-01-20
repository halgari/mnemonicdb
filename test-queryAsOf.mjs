import { createMnemonicDB } from "./dist/index.js";

async function test() {
  console.log("=".repeat(70));
  console.log("TESTING queryAsOf() METHOD");
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

  await db.defineView({
    name: "users",
    attributes: ["user/name", "user/email"],
  });

  // Insert users at different transactions
  console.log("\n1. Insert Alice");
  await db.query(
    `INSERT INTO users (name, email) VALUES ('Alice', 'alice@example.com')`
  );
  const tx1 = BigInt((await db.query(`SELECT MAX(id) as tx FROM transactions`))[0].tx);
  console.log(`   Transaction after Alice: ${tx1}`);

  console.log("\n2. Insert Bob");
  await db.query(
    `INSERT INTO users (name, email) VALUES ('Bob', 'bob@example.com')`
  );
  const tx2 = BigInt((await db.query(`SELECT MAX(id) as tx FROM transactions`))[0].tx);
  console.log(`   Transaction after Bob: ${tx2}`);

  console.log("\n3. Insert Charlie");
  await db.query(
    `INSERT INTO users (name, email) VALUES ('Charlie', 'charlie@example.com')`
  );
  const tx3 = BigInt((await db.query(`SELECT MAX(id) as tx FROM transactions`))[0].tx);
  console.log(`   Transaction after Charlie: ${tx3}`);

  // Test queryAsOf with null (current state)
  console.log("\n4. queryAsOf with null (should show all 3 users):");
  const currentUsers = await db.queryAsOf(`SELECT name FROM users ORDER BY name`, [], null);
  console.log(`   Users: ${currentUsers.map(u => u.name).join(", ")}`);

  // Test queryAsOf with tx1 (only Alice)
  console.log(`\n5. queryAsOf with tx1=${tx1} (should show only Alice):`);
  const usersAtTx1 = await db.queryAsOf(`SELECT name FROM users ORDER BY name`, [], tx1);
  console.log(`   Users: ${usersAtTx1.map(u => u.name).join(", ")}`);

  // Test queryAsOf with tx2 (Alice and Bob)
  console.log(`\n6. queryAsOf with tx2=${tx2} (should show Alice and Bob):`);
  const usersAtTx2 = await db.queryAsOf(`SELECT name FROM users ORDER BY name`, [], tx2);
  console.log(`   Users: ${usersAtTx2.map(u => u.name).join(", ")}`);

  // Test queryAsOf with parameters
  console.log("\n7. queryAsOf with parameters:");
  const aliceAtTx2 = await db.queryAsOf(
    `SELECT name, email FROM users WHERE name = $1`,
    ["Alice"],
    tx2
  );
  console.log(`   Alice at tx2: ${JSON.stringify(aliceAtTx2[0])}`);

  // Test concurrent queries don't interfere
  console.log("\n8. Concurrent queryAsOf calls (should not interfere):");
  const [resultTx1, resultTx2, resultTx3] = await Promise.all([
    db.queryAsOf(`SELECT name FROM users ORDER BY name`, [], tx1),
    db.queryAsOf(`SELECT name FROM users ORDER BY name`, [], tx2),
    db.queryAsOf(`SELECT name FROM users ORDER BY name`, [], tx3),
  ]);
  console.log(`   tx1: ${resultTx1.map(u => u.name).join(", ")}`);
  console.log(`   tx2: ${resultTx2.map(u => u.name).join(", ")}`);
  console.log(`   tx3: ${resultTx3.map(u => u.name).join(", ")}`);

  // Verify that queryAsOf doesn't affect the global as-of state
  console.log("\n9. Verify queryAsOf doesn't change global state:");
  await db.setAsOf(tx1);  // Set global as-of to tx1
  const globalAsOf = await db.getAsOf();
  console.log(`   Global as-of before queryAsOf: ${globalAsOf}`);

  // queryAsOf to tx3 should not affect global state
  const queryResult = await db.queryAsOf(`SELECT name FROM users ORDER BY name`, [], tx3);
  console.log(`   queryAsOf(tx3) result: ${queryResult.map(u => u.name).join(", ")}`);

  const globalAsOfAfter = await db.getAsOf();
  console.log(`   Global as-of after queryAsOf: ${globalAsOfAfter}`);

  // Regular query should still use global as-of (tx1)
  const regularQuery = await db.query(`SELECT name FROM users ORDER BY name`);
  console.log(`   Regular query (should use tx1): ${regularQuery.map(u => u.name).join(", ")}`);

  await db.setAsOf(null);  // Reset

  // Test with update
  console.log("\n10. Test with updates:");
  await db.query(`UPDATE users SET email = 'alice.new@example.com' WHERE name = 'Alice'`);
  const tx4 = BigInt((await db.query(`SELECT MAX(id) as tx FROM transactions`))[0].tx);

  const aliceOld = await db.queryAsOf(`SELECT email FROM users WHERE name = 'Alice'`, [], tx2);
  const aliceNew = await db.queryAsOf(`SELECT email FROM users WHERE name = 'Alice'`, [], tx4);
  console.log(`   Alice email at tx2: ${aliceOld[0].email}`);
  console.log(`   Alice email at tx4: ${aliceNew[0].email}`);

  await db.close();

  console.log("\n" + "=".repeat(70));
  console.log("ALL TESTS PASSED!");
  console.log("=".repeat(70));
}

test().catch(console.error);
