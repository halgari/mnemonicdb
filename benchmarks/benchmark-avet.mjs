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
  console.log(`SQL: ${sql.replace(/\s+/g, ' ').trim()}`);
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

async function timeQuery(db, name, sql, params = []) {
  const start = performance.now();
  const result = await db.query(sql, params);
  const elapsed = performance.now() - start;
  console.log(`${name}: ${elapsed.toFixed(2)}ms (${result.length} rows)`);
  return result;
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

  await db.defineView({
    name: "users",
    attributes: ["user/name", "user/email", "user/birth-year"],
  });

  // Insert users
  const insertedNames = [];
  console.log(`Inserting ${NUM_USERS} users...`);
  const start = performance.now();

  for (let i = 0; i < NUM_USERS; i++) {
    const name = randomName();
    const email = randomEmail(name, i);
    const birthYear = randomBirthYear();
    insertedNames.push(name);

    await db.query(
      `INSERT INTO users (name, email, birth_year) VALUES ($1, $2, $3)`,
      [name, email, birthYear]
    );

    if ((i + 1) % 5000 === 0) {
      console.log(`  ${i + 1} inserted...`);
    }
  }
  console.log(`Insert complete: ${((performance.now() - start) / 1000).toFixed(1)}s`);

  const targetName = insertedNames[1000];
  console.log(`\nTarget name: "${targetName}"`);

  // Get attribute IDs
  const nameAttrId = 300;  // user/name
  const emailAttrId = 301; // user/email
  const birthYearAttrId = 302; // user/birth-year

  console.log("\n" + "#".repeat(70));
  console.log("# COMPARING QUERY STRATEGIES");
  console.log("#".repeat(70));

  // Strategy 1: Current view (base = birth_year, join name/email)
  await explainQuery(
    db,
    "1. Current view: WHERE name = ?",
    `SELECT * FROM users WHERE name = $1`,
    [targetName]
  );

  // Strategy 2: Subquery using AVET index on name, then join
  await explainQuery(
    db,
    "2. Subquery: Find E via AVET, then get full record",
    `SELECT u.* FROM users u
     WHERE u.id IN (
       SELECT e FROM datoms_text
       WHERE a = $1 AND v = $2 AND retracted_by IS NULL
     )`,
    [nameAttrId, targetName]
  );

  // Strategy 3: Start from the filtered attribute, join others
  await explainQuery(
    db,
    "3. Reordered joins: name table first, join others",
    `SELECT
       name_t.e AS id,
       name_t.v AS name,
       email_t.v AS email,
       birth_t.v AS birth_year
     FROM datoms_text name_t
     JOIN datoms_text email_t ON name_t.e = email_t.e
       AND email_t.a = $2 AND email_t.retracted_by IS NULL
     JOIN datoms_int4 birth_t ON name_t.e = birth_t.e
       AND birth_t.a = $3 AND birth_t.retracted_by IS NULL
     WHERE name_t.a = $1 AND name_t.v = $4 AND name_t.retracted_by IS NULL`,
    [nameAttrId, emailAttrId, birthYearAttrId, targetName]
  );

  // Strategy 4: CTE to isolate the filter
  await explainQuery(
    db,
    "4. CTE: Filter first, then join",
    `WITH matching_entities AS (
       SELECT e FROM datoms_text
       WHERE a = $1 AND v = $2 AND retracted_by IS NULL
     )
     SELECT
       m.e AS id,
       name_t.v AS name,
       email_t.v AS email,
       birth_t.v AS birth_year
     FROM matching_entities m
     JOIN datoms_text name_t ON m.e = name_t.e
       AND name_t.a = $1 AND name_t.retracted_by IS NULL
     JOIN datoms_text email_t ON m.e = email_t.e
       AND email_t.a = $3 AND email_t.retracted_by IS NULL
     JOIN datoms_int4 birth_t ON m.e = birth_t.e
       AND birth_t.a = $4 AND birth_t.retracted_by IS NULL`,
    [nameAttrId, targetName, emailAttrId, birthYearAttrId]
  );

  // Strategy 5: Lateral join
  await explainQuery(
    db,
    "5. LATERAL: Filter drives the join",
    `SELECT
       filtered.e AS id,
       filtered.v AS name,
       email_t.v AS email,
       birth_t.v AS birth_year
     FROM datoms_text filtered
     JOIN LATERAL (
       SELECT v FROM datoms_text
       WHERE e = filtered.e AND a = $2 AND retracted_by IS NULL
     ) email_t ON true
     JOIN LATERAL (
       SELECT v FROM datoms_int4
       WHERE e = filtered.e AND a = $3 AND retracted_by IS NULL
     ) birth_t ON true
     WHERE filtered.a = $1 AND filtered.v = $4 AND filtered.retracted_by IS NULL`,
    [nameAttrId, emailAttrId, birthYearAttrId, targetName]
  );

  // Now time them without EXPLAIN overhead
  console.log("\n" + "#".repeat(70));
  console.log("# TIMING (5 iterations each)");
  console.log("#".repeat(70));

  for (let run = 0; run < 3; run++) {
    console.log(`\n--- Run ${run + 1} ---`);

    await timeQuery(db, "1. Current view       ",
      `SELECT * FROM users WHERE name = $1`, [targetName]);

    await timeQuery(db, "2. Subquery (IN)      ",
      `SELECT u.* FROM users u WHERE u.id IN (
         SELECT e FROM datoms_text WHERE a = $1 AND v = $2 AND retracted_by IS NULL
       )`, [nameAttrId, targetName]);

    await timeQuery(db, "3. Reordered joins    ",
      `SELECT name_t.e AS id, name_t.v AS name, email_t.v AS email, birth_t.v AS birth_year
       FROM datoms_text name_t
       JOIN datoms_text email_t ON name_t.e = email_t.e AND email_t.a = $2 AND email_t.retracted_by IS NULL
       JOIN datoms_int4 birth_t ON name_t.e = birth_t.e AND birth_t.a = $3 AND birth_t.retracted_by IS NULL
       WHERE name_t.a = $1 AND name_t.v = $4 AND name_t.retracted_by IS NULL`,
      [nameAttrId, emailAttrId, birthYearAttrId, targetName]);

    await timeQuery(db, "4. CTE                ",
      `WITH matching_entities AS (
         SELECT e FROM datoms_text WHERE a = $1 AND v = $2 AND retracted_by IS NULL
       )
       SELECT m.e AS id, name_t.v AS name, email_t.v AS email, birth_t.v AS birth_year
       FROM matching_entities m
       JOIN datoms_text name_t ON m.e = name_t.e AND name_t.a = $1 AND name_t.retracted_by IS NULL
       JOIN datoms_text email_t ON m.e = email_t.e AND email_t.a = $3 AND email_t.retracted_by IS NULL
       JOIN datoms_int4 birth_t ON m.e = birth_t.e AND birth_t.a = $4 AND birth_t.retracted_by IS NULL`,
      [nameAttrId, targetName, emailAttrId, birthYearAttrId]);

    await timeQuery(db, "5. LATERAL            ",
      `SELECT filtered.e AS id, filtered.v AS name, email_t.v AS email, birth_t.v AS birth_year
       FROM datoms_text filtered
       JOIN LATERAL (SELECT v FROM datoms_text WHERE e = filtered.e AND a = $2 AND retracted_by IS NULL) email_t ON true
       JOIN LATERAL (SELECT v FROM datoms_int4 WHERE e = filtered.e AND a = $3 AND retracted_by IS NULL) birth_t ON true
       WHERE filtered.a = $1 AND filtered.v = $4 AND filtered.retracted_by IS NULL`,
      [nameAttrId, emailAttrId, birthYearAttrId, targetName]);
  }

  await db.close();
}

benchmark().catch(console.error);
