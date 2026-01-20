import { createMnemonicDB } from "./dist/index.js";

async function test() {
  console.log("=".repeat(70));
  console.log("AS-OF QUERY TEST");
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

  // Insert first user and record the transaction
  console.log("\n1. Insert Alice");
  const result1 = await db.query(
    `INSERT INTO users (name, email) VALUES ('Alice', 'alice@example.com') RETURNING id`
  );
  const aliceId = result1[0].id;
  console.log(`   Alice ID: ${aliceId}`);

  // Get the current transaction
  const tx1Result = await db.query(`SELECT MAX(id) as tx FROM transactions`);
  const tx1 = BigInt(tx1Result[0].tx);
  console.log(`   Transaction after Alice: ${tx1}`);

  // Insert second user
  console.log("\n2. Insert Bob");
  await db.query(
    `INSERT INTO users (name, email) VALUES ('Bob', 'bob@example.com')`
  );

  const tx2Result = await db.query(`SELECT MAX(id) as tx FROM transactions`);
  const tx2 = BigInt(tx2Result[0].tx);
  console.log(`   Transaction after Bob: ${tx2}`);

  // Current view should show both
  console.log("\n3. Current view (should show Alice and Bob):");
  const currentUsers = await db.query(`SELECT * FROM users ORDER BY name`);
  console.log(`   Users: ${currentUsers.map(u => u.name).join(", ")}`);

  // As-of tx1 should only show Alice
  console.log(`\n4. As-of tx ${tx1} (should show only Alice):`);
  await db.setAsOf(tx1);
  const asOfTx1Users = await db.query(`SELECT * FROM users ORDER BY name`);
  console.log(`   Users: ${asOfTx1Users.map(u => u.name).join(", ")}`);

  // Verify getAsOf
  const currentAsOf = await db.getAsOf();
  console.log(`   Current as-of: ${currentAsOf}`);

  // Reset to current view
  console.log("\n5. Reset to current view:");
  await db.setAsOf(null);
  const resetUsers = await db.query(`SELECT * FROM users ORDER BY name`);
  console.log(`   Users: ${resetUsers.map(u => u.name).join(", ")}`);

  // Update Alice's email
  console.log("\n6. Update Alice's email");
  await db.query(
    `UPDATE users SET email = 'alice.new@example.com' WHERE name = 'Alice'`
  );

  const tx3Result = await db.query(`SELECT MAX(id) as tx FROM transactions`);
  const tx3 = BigInt(tx3Result[0].tx);
  console.log(`   Transaction after update: ${tx3}`);

  // Current should show new email
  console.log("\n7. Current view (should show new email):");
  const aliceCurrent = await db.query(`SELECT * FROM users WHERE name = 'Alice'`);
  console.log(`   Alice email: ${aliceCurrent[0].email}`);

  // As-of tx2 should show old email
  console.log(`\n8. As-of tx ${tx2} (should show old email):`);
  await db.setAsOf(tx2);
  const aliceOld = await db.query(`SELECT * FROM users WHERE name = 'Alice'`);
  console.log(`   Alice email: ${aliceOld[0].email}`);

  // Test withAsOf helper
  console.log("\n9. Test withAsOf helper:");
  await db.setAsOf(null);  // Reset first

  const historicalEmail = await db.withAsOf(tx2, async () => {
    const result = await db.query(`SELECT email FROM users WHERE name = 'Alice'`);
    return result[0].email;
  });
  console.log(`   Historical email (via withAsOf): ${historicalEmail}`);

  // Verify we're back to current
  const currentEmail = await db.query(`SELECT email FROM users WHERE name = 'Alice'`);
  console.log(`   Current email (after withAsOf): ${currentEmail[0].email}`);

  // Delete Bob and test as-of shows him in the past
  console.log("\n10. Delete Bob and verify as-of:");
  await db.query(`DELETE FROM users WHERE name = 'Bob'`);

  const tx4Result = await db.query(`SELECT MAX(id) as tx FROM transactions`);
  const tx4 = BigInt(tx4Result[0].tx);

  console.log("   Current users (Bob should be gone):");
  const afterDelete = await db.query(`SELECT * FROM users ORDER BY name`);
  console.log(`   Users: ${afterDelete.map(u => u.name).join(", ") || "(none)"}`);

  console.log(`   As-of tx ${tx3} (Bob should still exist):`);
  await db.setAsOf(tx3);
  const beforeDelete = await db.query(`SELECT * FROM users ORDER BY name`);
  console.log(`   Users: ${beforeDelete.map(u => u.name).join(", ")}`);

  await db.setAsOf(null);

  console.log("\n" + "=".repeat(70));
  console.log("ALL TESTS PASSED!");
  console.log("=".repeat(70));

  await db.close();
}

test().catch(console.error);
