#!/usr/bin/env python3
"""Key-set benchmark (MN-6): small key sets joined against large tables.

Builds a Mauvi-shaped database (N file items under N/200 groups, with `files`
and `sourced` rows for every file), opens a transaction that changes K items,
and times the query shapes commit assertions use: semi joins from
mnemonicdb_tx_changes on `id` and on an indexed REF column, explicit IN lists,
empty key sets, and the same under AT. Finally it times single-row commits with
a set of assertions installed.

Usage: bench/keyset_bench.py [--rows N] [--duckdb PATH] [--db DIR] [--repeat R]

--db keeps the generated database in DIR and reuses it on later runs (it is
only read by the timed part: every transaction rolls back except the commit
benchmark, which toggles rows back and forth).
"""
import argparse
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--rows", type=int, default=1_000_000)
parser.add_argument("--duckdb", default="build/reldebug/duckdb")
parser.add_argument("--db", default=None, help="keep/reuse the database in this directory")
parser.add_argument("--repeat", type=int, default=5)
parser.add_argument("--commits", type=int, default=7)
parser.add_argument("--stat", choices=["median", "min"], default="median",
                    help="min (best of the repeats) is steadier on a shared machine")
parser.add_argument("--rule", action="store_true",
                    help="also time commits with assertions that all follow Mauvi's key-set rule")
parser.add_argument("--minted", action="store_true",
                    help="--rule, plus the same assertions skipping minted entities (needs the minted column)")
parser.add_argument("--only-commits", action="store_true", help="skip the query timings")
args = parser.parse_args()

N = args.rows
GROUPS = max(N // 200, 1)
ATTACH = "ATTACH '{path}' AS d (TYPE mnemonicdb, MAX_BACKGROUND_JOBS 2); USE d;"


def run_script(lines):
    out = subprocess.run([args.duckdb, "-list", "-noheader"], input="\n".join(lines) + "\n", capture_output=True,
                         text=True)
    if out.returncode != 0 or "Error" in out.stdout or "Error" in out.stderr:
        print(out.stdout[-4000:], out.stderr[-4000:], file=sys.stderr)
        sys.exit(1)
    return out.stdout


def build(path):
    run_script([
        ATTACH.format(path=path),
        "CREATE TABLE loadouts (name VARCHAR);",
        "CREATE TABLE items (parent REF, enabled BOOLEAN, name VARCHAR);",
        "CREATE INDEX items_parent ON items (parent);",
        "CREATE TABLE files (root VARCHAR, path VARCHAR);",
        "CREATE INDEX files_path ON files (path);",
        "CREATE TABLE content (size BIGINT);",
        "CREATE TABLE sourced (content REF);",
        "INSERT INTO loadouts (name) VALUES ('main');",
        f"INSERT INTO items (parent, enabled, name) SELECT (SELECT id FROM loadouts), true, 'g' || i FROM range({GROUPS}) t(i);",
        # Files: group of file i is i % GROUPS, so a group's files are spread over the id range
        f"INSERT INTO items (parent, enabled, name) SELECT g.id, true, 'f' || i FROM range({N}) t(i) "
        f"JOIN (SELECT id, row_number() OVER (ORDER BY id) - 1 AS n FROM items WHERE name LIKE 'g%') g ON g.n = i % {GROUPS};",
        "INSERT INTO files (id, root, path) SELECT id, 'game', 'Data/' || name FROM items WHERE name LIKE 'f%';",
        f"INSERT INTO content (size) SELECT i FROM range({N}) t(i);",
        "INSERT INTO sourced (id, content) SELECT f.id, c.id FROM "
        "(SELECT id, row_number() OVER (ORDER BY id) AS n FROM files) f JOIN "
        "(SELECT id, row_number() OVER (ORDER BY id) AS n FROM content) c ON c.n = f.n;",
        "SELECT * FROM mnemonicdb_compact('d');",
    ])


def key_sets():
    """Variables holding key lists; ids of files are found from their (indexed) paths"""
    lines = []
    def file_ids(var, numbers):
        values = ", ".join(f"'Data/f{n}'" for n in numbers)
        lines.append(f"SET VARIABLE {var} = (SELECT list(id ORDER BY id) FROM files WHERE path IN ({values}));")
    file_ids("k1", [N // 2])
    file_ids("k2", [N // 3, 2 * N // 3])
    file_ids("k50", [(i * 19997) % N for i in range(50)])
    file_ids("k200", [(i * 4999) % N for i in range(200)])
    # 200 consecutive entity ids: the files of one group are spread, so take
    # consecutive file numbers' items, which were allocated consecutively
    file_ids("k200dense", [N // 2 + i for i in range(200)])
    lines.append("SET VARIABLE g2 = (SELECT list(id ORDER BY id) FROM items WHERE name IN ('g7', 'g1234'));")
    # Installs go into a group of their own, so they don't change the key sets above
    lines.append("SET VARIABLE install_group = (SELECT id FROM items WHERE name = 'g4999');")
    return lines


def change(var):
    # Toggle `enabled` of the keyed items: the pending tx has len(var) changed entities
    return f"UPDATE items SET enabled = NOT enabled WHERE id IN (SELECT unnest(getvariable('{var}')));"


SEMI = ("SELECT count(*), max(length(f.path)) FROM files f WHERE f.id IN "
        "(SELECT unnest(list(k)) FROM (SELECT DISTINCT entity FROM mnemonicdb_tx_changes(table_name := 'items')) s(k));")
JOIN = ("SELECT count(*), max(length(f.path)) FROM (SELECT DISTINCT entity AS id FROM mnemonicdb_tx_changes()) c "
        "JOIN files f ON f.id = c.id;")
CHILDREN = ("SELECT count(*) FROM items WHERE parent IN "
            "(SELECT unnest(list(k)) FROM (SELECT DISTINCT entity FROM mnemonicdb_tx_changes(table_name := 'items')) s(k));")


def queries():
    """(label, setup lines, timed sql, teardown lines)"""
    q = []
    for var in ["k1", "k2", "k50", "k200", "k200dense"]:
        tx = ["BEGIN;", change(var)]
        q.append((f"semi_id_{var}", tx, SEMI, ["ROLLBACK;"]))
        q.append((f"join_id_{var}", tx, JOIN, ["ROLLBACK;"]))
    q.append(("semi_id_empty", ["BEGIN;", "INSERT INTO loadouts (name) VALUES ('x');"], SEMI, ["ROLLBACK;"]))
    q.append(("in_list_id_k2", [], "SELECT count(*) FROM files WHERE id IN (SELECT unnest(getvariable('k2')));", []))
    q.append(("in_const_id_200", [], "SELECT count(*) FROM files WHERE id IN ({});", []))  # filled in below
    q.append(("children_ref_g2", ["BEGIN;", change("g2")], CHILDREN, ["ROLLBACK;"]))
    q.append(("at_semi_id_k200", ["BEGIN;", change("k200")],
              SEMI.replace("FROM files f", "FROM files f AT (VERSION => {version})"), ["ROLLBACK;"]))
    q.append(("full_scan_files", [], "SELECT count(*), max(length(path)) FROM files;", []))
    return q


def timed_queries(path):
    lines = [ATTACH.format(path=path)] + key_sets()
    # The constant IN list needs the ids as literals
    out = run_script(lines + ["SELECT array_to_string(getvariable('k200'), ',');",
                              "SELECT max(id::UBIGINT & 1099511627775) FROM sys.tx;"])
    k200, version = out.strip().splitlines()[-2:]
    lines.append(".timer on")
    for label, setup, sql, teardown in queries():
        sql = sql.replace("({})", f"({k200})").replace("{version}", version)
        lines += setup
        for _ in range(args.repeat):
            lines.append(f".print @@{label}")
            lines.append(sql)
            lines.append(".print @@-")
        lines += teardown
    out = run_script(lines)
    return parse_times(out)


def parse_times(out):
    """Each @@label ... @@- block is one sample: the sum of its statements' times"""
    times = {}
    label = None
    total = 0.0
    for line in out.splitlines():
        if line.startswith("@@"):
            if line == "@@-" and label:
                times.setdefault(label, []).append(total)
            label = None if line == "@@-" else line[2:]
            total = 0.0
            continue
        m = re.match(r"Run Time \(s\): real ([0-9.]+)", line)
        if m and label:
            total += float(m.group(1))
    pick = min if args.stat == "min" else statistics.median
    return {k: pick(v) for k, v in times.items()}


# A few Mauvi-style assertions (DEC-081 shapes): each reads large tables only
# through small key sets from mnemonicdb_tx_changes
ASSERTIONS = {
    "a_dest_iff_content": """
WITH c AS MATERIALIZED (SELECT DISTINCT entity AS id FROM mnemonicdb_tx_changes() WHERE table_name IN ('items', 'files', 'sourced')),
f AS MATERIALIZED (SELECT id FROM files WHERE id IN (SELECT unnest(list(k)) FROM (SELECT id FROM c) AS s(k))),
b AS MATERIALIZED (SELECT id FROM sourced WHERE id IN (SELECT unnest(list(k)) FROM (SELECT id FROM c) AS s(k)))
SELECT id FROM f WHERE id NOT IN (SELECT id FROM b)""",
    "a_files_no_children": """
WITH c AS MATERIALIZED (SELECT entity AS id FROM mnemonicdb_tx_changes(table_name := 'files') WHERE added
  UNION SELECT CAST(value::UBIGINT AS REF) FROM mnemonicdb_tx_changes(table_name := 'items', column_name := 'parent') WHERE added),
f AS MATERIALIZED (SELECT id FROM files WHERE id IN (SELECT unnest(list(k)) FROM (SELECT id FROM c) AS s(k))),
ch AS MATERIALIZED (SELECT id, parent FROM items WHERE parent IN (SELECT unnest(list(k)) FROM (SELECT id FROM f) AS s(k)))
SELECT f.id FROM f JOIN ch ON ch.parent = f.id""",
    "a_parent_exists": """
WITH c AS MATERIALIZED (SELECT DISTINCT entity AS id FROM mnemonicdb_tx_changes(table_name := 'items')),
i AS MATERIALIZED (SELECT id, parent FROM items WHERE id IN (SELECT unnest(list(k)) FROM (SELECT id FROM c) AS s(k))),
p AS MATERIALIZED (SELECT id FROM items WHERE id IN (SELECT unnest(list(k)) FROM (SELECT parent FROM i) AS s(k))
  UNION ALL SELECT id FROM loadouts WHERE id IN (SELECT unnest(list(k)) FROM (SELECT parent FROM i) AS s(k)))
SELECT i.id FROM i WHERE i.parent IS NULL OR i.parent NOT IN (SELECT id FROM p)""",
    "a_content_exists": """
WITH r AS MATERIALIZED (SELECT id, content FROM sourced WHERE id IN (SELECT unnest(list(k)) FROM
    (SELECT entity FROM mnemonicdb_tx_changes(table_name := 'sourced') WHERE added) AS s(k))),
t AS MATERIALIZED (SELECT id FROM content WHERE id IN (SELECT unnest(list(k)) FROM (SELECT content FROM r) AS s(k)))
SELECT r.id FROM r WHERE r.content NOT IN (SELECT id FROM t)""",
    "a_plain_join": """
SELECT f.id FROM (SELECT DISTINCT entity AS id FROM mnemonicdb_tx_changes(table_name := 'items')) c
JOIN files f ON f.id = c.id JOIN items i ON i.id = f.id WHERE i.parent IS NULL""",
    # Children of items whose parent changed must still have an enabled parent
    # (a walk down from moved items). This shape joins the second fetch
    # directly; DuckDB estimates the semi join at 20% of `items` and builds
    # the hash table on a scan of `items`
    "a_children_of_moved": """
WITH m AS MATERIALIZED (SELECT entity AS id FROM mnemonicdb_tx_changes(table_name := 'items', column_name := 'parent') WHERE added),
ch AS MATERIALIZED (SELECT id, parent FROM items WHERE parent IN (SELECT unnest(list(k)) FROM (SELECT id FROM m) AS s(k)))
SELECT ch.id FROM ch JOIN items p ON p.id = ch.parent WHERE p.enabled IS NULL""",
}

# The same check following Mauvi's rule (DEC-081): every read of a large table
# is `id IN (<small set>)`
RULE_ASSERTIONS = dict(ASSERTIONS)
RULE_ASSERTIONS["a_children_of_moved"] = """
WITH m AS MATERIALIZED (SELECT entity AS id FROM mnemonicdb_tx_changes(table_name := 'items', column_name := 'parent') WHERE added),
ch AS MATERIALIZED (SELECT id, parent FROM items WHERE parent IN (SELECT unnest(list(k)) FROM (SELECT id FROM m) AS s(k))),
p AS MATERIALIZED (SELECT id, enabled FROM items WHERE id IN (SELECT unnest(list(k)) FROM (SELECT parent FROM ch) AS s(k)))
SELECT ch.id FROM ch JOIN p ON p.id = ch.parent WHERE p.enabled IS NULL"""

# And skipping minted items: an item minted by the transaction has no
# children from before it, so it is no starting point for the walk
MINTED_ASSERTIONS = dict(RULE_ASSERTIONS)
MINTED_ASSERTIONS["a_children_of_moved"] = RULE_ASSERTIONS["a_children_of_moved"].replace(
    "WHERE added)", "WHERE added AND NOT minted)", 1)

INSTALL_COUNT = [0]


def install(n):
    """One transaction adding n new files (item, file, sourced, content)"""
    INSTALL_COUNT[0] += 1
    run = INSTALL_COUNT[0]
    return [
        "BEGIN;",
        f"INSERT INTO content (id, size) SELECT tempid('c' || i), i FROM range({n}) t(i);",
        f"INSERT INTO items (id, parent, enabled, name) SELECT tempid('f' || i), getvariable('install_group'), true, 'new{run}_' || i FROM range({n}) t(i);",
        f"INSERT INTO files (id, root, path) SELECT tempid('f' || i), 'game', 'New{run}/' || i FROM range({n}) t(i);",
        f"INSERT INTO sourced (id, content) SELECT tempid('f' || i), tempid('c' || i) FROM range({n}) t(i);",
        "COMMIT;",
    ]


def commit_bench(path, assertions, suffix=""):
    def sql_str(s):
        return "'" + s.replace("'", "''") + "'"
    setup = [ATTACH.format(path=path), "DELETE FROM sys.assertions;"]
    for name, query in assertions.items():
        setup.append(f"INSERT INTO sys.assertions (name, query) VALUES ({sql_str(name)}, {sql_str(' '.join(query.split()))});")
    run_script(setup)
    lines = [ATTACH.format(path=path)] + key_sets() + [".timer on"]

    def workloads(tag):
        for label, var in [("commit_1_row", "k1"), ("commit_200_rows", "k200")]:
            for _ in range(args.commits):
                lines.append(f".print @@{label}{tag}")
                lines.append(change(var))
                lines.append(".print @@-")
        for _ in range(3):
            lines.append(f".print @@install_1000{tag}")
            lines.extend(install(1000))
            lines.append(".print @@-")
    workloads(suffix)
    if not suffix:
        # Without assertions, for reference
        lines.extend([".timer off", "DELETE FROM sys.assertions;", ".timer on"])
        workloads("_no_assertions")
    return parse_times(run_script(lines))


def main():
    if args.db:
        workdir = args.db
        os.makedirs(workdir, exist_ok=True)
    else:
        workdir = tempfile.mkdtemp(prefix="mnemonic-keyset-", dir=os.environ.get("BENCH_DIR"))
    path = os.path.join(workdir, f"keyset-{N}.mnemonic")
    try:
        if not os.path.exists(path):
            build(path)
        results = {} if args.only_commits else timed_queries(path)
        results.update(commit_bench(path, ASSERTIONS))
        if args.rule or args.minted:
            results.update(commit_bench(path, RULE_ASSERTIONS, "_rule"))
        if args.minted:
            results.update(commit_bench(path, MINTED_ASSERTIONS, "_rule_minted"))
        print(f"rows={N} groups={GROUPS} repeat={args.repeat} commits={args.commits} ({args.stat} seconds)")
        for label, value in results.items():
            print(f"{label:32}{value:10.4f}")
    finally:
        if not args.db:
            shutil.rmtree(workdir, ignore_errors=True)


main()
