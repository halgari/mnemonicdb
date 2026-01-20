import { describe, it, expect, beforeEach, afterEach } from "vitest";
import { createMnemonicDB, MnemonicDB } from "./index.js";

describe("MnemonicDB", () => {
  let db: MnemonicDB;

  beforeEach(async () => {
    // Create in-memory database
    db = await createMnemonicDB();
  });

  afterEach(async () => {
    await db.close();
  });

  describe("bootstrap", () => {
    it("should create partitions", async () => {
      const partitions = await db.query<{ ident: string }>(
        "SELECT ident FROM partitions ORDER BY id"
      );
      expect(partitions.map((p) => p.ident)).toEqual(["db", "tx", "user"]);
    });

    it("should create core attributes", async () => {
      const dbIdent = await db.attrId("db/ident");
      const dbValueType = await db.attrId("db/valueType");
      const dbCardinality = await db.attrId("db/cardinality");

      expect(dbIdent).toBe(1n);
      expect(dbValueType).toBe(2n);
      expect(dbCardinality).toBe(3n);
    });

    it("should create value type entities", async () => {
      const textType = await db.attrId("db.type/text");
      const refType = await db.attrId("db.type/ref");

      expect(textType).toBe(100n);
      expect(refType).toBe(112n);
    });

    it("should create cardinality entities", async () => {
      const cardOne = await db.attrId("db.cardinality/one");
      const cardMany = await db.attrId("db.cardinality/many");

      expect(cardOne).toBe(200n);
      expect(cardMany).toBe(201n);
    });
  });

  describe("entity allocation", () => {
    it("should allocate entities from user partition", async () => {
      const e1 = await db.allocateEntity("user");
      const e2 = await db.allocateEntity("user");

      // User partition is 2, so high bits should be 2 << 48
      const userPartitionBits = 2n << 48n;
      expect(e1).toBe(userPartitionBits | 1n);
      expect(e2).toBe(userPartitionBits | 2n);
    });

    it("should allocate entities from db partition", async () => {
      const e1 = await db.allocateEntity("db");

      // db partition is 0, next_id was set to 300
      expect(e1).toBe(300n);
    });
  });

  describe("transactions", () => {
    it("should create new transactions", async () => {
      const tx1 = await db.newTransaction();
      const tx2 = await db.newTransaction();

      // tx partition is 1, so high bits should be 1 << 48
      const txPartitionBits = 1n << 48n;
      expect(tx1).toBe(txPartitionBits | 1n);
      expect(tx2).toBe(txPartitionBits | 2n);
    });

    it("should record transaction in transactions table", async () => {
      const txId = await db.newTransaction();
      const txs = await db.query<{ id: string }>(
        "SELECT id FROM transactions WHERE id = $1",
        [txId.toString()]
      );
      expect(txs.length).toBe(1);
    });
  });

  describe("schema introspection", () => {
    it("should list system attributes via mnemonic_attributes view", async () => {
      const attrs = await db.listAttributes();
      const idents = attrs.map((a) => a.ident);

      expect(idents).toContain("db/ident");
      expect(idents).toContain("db/valueType");
      expect(idents).toContain("db/cardinality");
    });
  });

  describe("attribute definition", () => {
    it("should define a new attribute", async () => {
      await db.defineAttribute({
        ident: "person/name",
        valueType: "db.type/text",
        cardinality: "db.cardinality/one",
      });

      const attrId = await db.attrId("person/name");
      expect(attrId).not.toBeNull();

      const attrs = await db.listAttributes();
      const personName = attrs.find((a) => a.ident === "person/name");
      expect(personName).toBeDefined();
      expect(personName?.value_type).toBe("db.type/text");
      expect(personName?.cardinality).toBe("db.cardinality/one");
    });

    it("should define attribute with uniqueness", async () => {
      await db.defineAttribute({
        ident: "person/email",
        valueType: "db.type/text",
        cardinality: "db.cardinality/one",
        unique: "db.unique/identity",
      });

      const attrs = await db.listAttributes();
      const email = attrs.find((a) => a.ident === "person/email");
      expect(email?.unique_constraint).toBe("db.unique/identity");
    });
  });

  describe("view definition", () => {
    it("should define a view with attributes and auto-create SQL view", async () => {
      // Define attributes first
      await db.defineAttribute({
        ident: "person/name",
        valueType: "db.type/text",
        cardinality: "db.cardinality/one",
      });
      await db.defineAttribute({
        ident: "person/age",
        valueType: "db.type/int4",
        cardinality: "db.cardinality/one",
      });

      // Define view - SQL view should be created automatically
      await db.defineView({
        name: "persons",
        attributes: ["person/name", "person/age"],
        doc: "Person entities",
      });

      const views = await db.listViews();
      const persons = views.find((v) => v.name === "persons");
      expect(persons).toBeDefined();
      expect(persons?.doc).toBe("Person entities");

      // Check view attributes
      const viewAttrs = await db.query<{ view_name: string; attribute_ident: string }>(
        "SELECT * FROM mnemonic_view_attributes WHERE view_name = 'persons'"
      );
      expect(viewAttrs.map((a) => a.attribute_ident).sort()).toEqual([
        "person/age",
        "person/name",
      ]);

      // SQL view should already exist - no regenerateViews() needed!
      const result = await db.query("SELECT * FROM persons");
      expect(result).toEqual([]);
    });

    it("should update view definition and auto-regenerate SQL view", async () => {
      await db.defineAttribute({
        ident: "person/name",
        valueType: "db.type/text",
        cardinality: "db.cardinality/one",
      });
      await db.defineAttribute({
        ident: "person/age",
        valueType: "db.type/int4",
        cardinality: "db.cardinality/one",
      });
      await db.defineAttribute({
        ident: "person/email",
        valueType: "db.type/text",
        cardinality: "db.cardinality/one",
      });

      // Create view with just name and age
      await db.defineView({
        name: "persons",
        attributes: ["person/name", "person/age"],
      });

      // Insert a person
      await db.exec("INSERT INTO persons (name, age) VALUES ('Test', 25)");

      // Update view to add email column
      await db.updateView("persons", {
        attributes: ["person/name", "person/age", "person/email"],
      });

      // New column should exist
      const cols = await db.query<{ column_name: string }>(
        `SELECT column_name FROM information_schema.columns
         WHERE table_name = 'persons' ORDER BY column_name`
      );
      expect(cols.map((c) => c.column_name).sort()).toEqual(["age", "email", "id", "name"]);
    });

    it("should delete view definition and drop SQL view", async () => {
      await db.defineAttribute({
        ident: "person/name",
        valueType: "db.type/text",
        cardinality: "db.cardinality/one",
      });
      await db.defineView({
        name: "persons",
        attributes: ["person/name"],
      });

      // View should exist
      let views = await db.listViews();
      expect(views.find((v) => v.name === "persons")).toBeDefined();

      // Delete view
      await db.deleteView("persons");

      // View definition should be gone
      views = await db.listViews();
      expect(views.find((v) => v.name === "persons")).toBeUndefined();

      // SQL view should be dropped
      await expect(db.query("SELECT * FROM persons")).rejects.toThrow();
    });
  });

  describe("view DML operations", () => {
    beforeEach(async () => {
      // Define schema - views auto-create now
      await db.defineAttribute({
        ident: "person/name",
        valueType: "db.type/text",
        cardinality: "db.cardinality/one",
      });
      await db.defineAttribute({
        ident: "person/age",
        valueType: "db.type/int4",
        cardinality: "db.cardinality/one",
      });
      await db.defineView({
        name: "persons",
        attributes: ["person/name", "person/age"],
      });
      // No regenerateViews() needed!
    });

    it("should INSERT via view", async () => {
      await db.exec("INSERT INTO persons (name, age) VALUES ('Alice', 30)");

      const persons = await db.query<{ id: string; name: string; age: number }>(
        "SELECT * FROM persons"
      );
      expect(persons.length).toBe(1);
      expect(persons[0].name).toBe("Alice");
      expect(persons[0].age).toBe(30);
      expect(persons[0].id).toBeDefined();
    });

    it("should create datoms on INSERT", async () => {
      await db.exec("INSERT INTO persons (name, age) VALUES ('Bob', 25)");

      // Check datoms were created
      const nameDatoms = await db.query<{ e: string; v: string }>(
        `SELECT e, v FROM datoms_text
         WHERE a = (SELECT mnemonic_attr_id('person/name'))
         AND retracted_by IS NULL`
      );
      expect(nameDatoms.length).toBe(1);
      expect(nameDatoms[0].v).toBe("Bob");

      const ageDatoms = await db.query<{ e: string; v: number }>(
        `SELECT e, v FROM datoms_int4
         WHERE a = (SELECT mnemonic_attr_id('person/age'))
         AND retracted_by IS NULL`
      );
      expect(ageDatoms.length).toBe(1);
      expect(ageDatoms[0].v).toBe(25);
    });

    it("should UPDATE via view", async () => {
      await db.exec("INSERT INTO persons (name, age) VALUES ('Charlie', 35)");

      const before = await db.query<{ id: string }>("SELECT id FROM persons");
      const id = before[0].id;

      await db.exec(`UPDATE persons SET name = 'Charles', age = 36 WHERE id = ${id}`);

      const after = await db.query<{ name: string; age: number }>(
        `SELECT name, age FROM persons WHERE id = ${id}`
      );
      expect(after[0].name).toBe("Charles");
      expect(after[0].age).toBe(36);
    });

    it("should retract old values on UPDATE", async () => {
      await db.exec("INSERT INTO persons (name, age) VALUES ('Dave', 40)");

      const before = await db.query<{ id: string }>("SELECT id FROM persons");
      const id = before[0].id;

      await db.exec(`UPDATE persons SET name = 'David' WHERE id = ${id}`);

      // Old name should be retracted
      const retractedNames = await db.query<{ v: string; retracted_by: string }>(
        `SELECT v, retracted_by FROM datoms_text
         WHERE e = ${id}
         AND a = (SELECT mnemonic_attr_id('person/name'))
         AND retracted_by IS NOT NULL`
      );
      expect(retractedNames.length).toBe(1);
      expect(retractedNames[0].v).toBe("Dave");

      // New name should be current
      const currentNames = await db.query<{ v: string }>(
        `SELECT v FROM datoms_text
         WHERE e = ${id}
         AND a = (SELECT mnemonic_attr_id('person/name'))
         AND retracted_by IS NULL`
      );
      expect(currentNames.length).toBe(1);
      expect(currentNames[0].v).toBe("David");
    });

    it("should DELETE via view", async () => {
      await db.exec("INSERT INTO persons (name, age) VALUES ('Eve', 28)");

      const before = await db.query<{ id: string }>("SELECT id FROM persons");
      expect(before.length).toBe(1);
      const id = before[0].id;

      await db.exec(`DELETE FROM persons WHERE id = ${id}`);

      const after = await db.query("SELECT * FROM persons");
      expect(after.length).toBe(0);
    });

    it("should retract datoms on DELETE", async () => {
      await db.exec("INSERT INTO persons (name, age) VALUES ('Frank', 50)");

      const before = await db.query<{ id: string }>("SELECT id FROM persons");
      const id = before[0].id;

      await db.exec(`DELETE FROM persons WHERE id = ${id}`);

      // All datoms should be retracted - check each table separately
      const currentTextDatoms = await db.query(
        `SELECT * FROM datoms_text WHERE e = ${id} AND retracted_by IS NULL`
      );
      const currentIntDatoms = await db.query(
        `SELECT * FROM datoms_int4 WHERE e = ${id} AND retracted_by IS NULL`
      );
      expect(currentTextDatoms.length).toBe(0);
      expect(currentIntDatoms.length).toBe(0);

      // But retracted datoms should still exist
      const retractedTextDatoms = await db.query(
        `SELECT * FROM datoms_text WHERE e = ${id} AND retracted_by IS NOT NULL`
      );
      const retractedIntDatoms = await db.query(
        `SELECT * FROM datoms_int4 WHERE e = ${id} AND retracted_by IS NOT NULL`
      );
      expect(retractedTextDatoms.length).toBe(1); // name
      expect(retractedIntDatoms.length).toBe(1); // age
    });
  });
});
