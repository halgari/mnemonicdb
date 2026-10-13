#!/usr/bin/env python3
"""MnemonicDB benchmark: runs the same workload against MnemonicDB and native DuckDB tables.

Usage: bench/bench.py [--rows N] [--duckdb PATH] [--only NAME,...]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--rows", type=int, default=1_000_000)
parser.add_argument("--duckdb", default="build/reldebug/duckdb")
parser.add_argument("--small-tx", type=int, default=2000)
parser.add_argument("--engines", default="mnemonic,native")
args = parser.parse_args()

N = args.rows
HALF = N // 2


def workload(engine, path):
    """(label, sql, timed) steps; ids are stable so both engines see the same data."""
    steps = []
    if engine == "mnemonic":
        steps.append(("attach", f"ATTACH '{path}' AS d (TYPE mnemonicdb);", False))
        steps.append(("schema", "CREATE TABLE d.person (name VARCHAR, age INTEGER, email VARCHAR UNIQUE, score DOUBLE);"
                                " CREATE TABLE d.account (balance BIGINT);", False))
        steps.append(("load_person", f"INSERT INTO d.person (name, age, email, score) SELECT 'p' || i, i % 100, 'e' || i, i / 7.0 FROM range({N}) t(i);", True))
        steps.append(("load_account", f"INSERT INTO d.account (id, balance) SELECT id, age * 10 FROM d.person WHERE age < 50;", True))
        steps.append(("load_vip", "CREATE TABLE d.vip (level INTEGER); INSERT INTO d.vip (id, level) SELECT id, 1 FROM d.person WHERE age = 0;", False))
        steps.append(("load_tagged", f"CREATE TABLE d.tagged (tags VARCHAR[]); INSERT INTO d.tagged (tags) SELECT ['t' || (i % 7), 'u' || (i % 11), 'v' || (i % 13)] FROM range({N // 3}) t(i);", False))
        idq = "(SELECT id FROM d.person WHERE email = 'e123456')"
    else:
        steps.append(("attach", f"ATTACH '{path}' AS d;", False))
        steps.append(("schema", "CREATE TABLE d.person (id BIGINT PRIMARY KEY, name VARCHAR, age INTEGER, email VARCHAR UNIQUE, score DOUBLE);"
                                " CREATE TABLE d.account (id BIGINT PRIMARY KEY, balance BIGINT);", False))
        steps.append(("load_person", f"INSERT INTO d.person SELECT i + 1, 'p' || i, i % 100, 'e' || i, i / 7.0 FROM range({N}) t(i);", True))
        steps.append(("load_account", f"INSERT INTO d.account SELECT id, age * 10 FROM d.person WHERE age < 50;", True))
        steps.append(("load_vip", "CREATE TABLE d.vip (id BIGINT PRIMARY KEY, level INTEGER); INSERT INTO d.vip SELECT id, 1 FROM d.person WHERE age = 0;", False))
        steps.append(("load_tagged", f"CREATE TABLE d.tagged (id BIGINT, tags VARCHAR[]); INSERT INTO d.tagged SELECT i, ['t' || (i % 7), 'u' || (i % 11), 'v' || (i % 13)] FROM range({N // 3}) t(i);", False))
        idq = "(SELECT id FROM d.person WHERE email = 'e123456')"
    steps += [
        ("checkpoint", "CHECKPOINT d;", True),
        ("scan_count", "SELECT count(*) FROM d.person;", True),
        ("scan_one_col", "SELECT sum(age) FROM d.person;", True),
        ("scan_all_cols", "SELECT sum(length(name)), sum(age), sum(length(email)), sum(score) FROM d.person;", True),
        ("scan_lists", "SELECT sum(len(tags)), count(DISTINCT tags[1]) FROM d.tagged;", True),
        ("filter_unindexed", "SELECT count(*) FROM d.person WHERE age = 42;", True),
        ("filter_projected", "SELECT sum(score) FROM d.person WHERE age > 90;", True),
        ("point_unique", "SELECT name FROM d.person WHERE email = 'e123456';", True),
        ("point_id", f"SET VARIABLE pid = {idq}; SELECT name FROM d.person WHERE id = getvariable('pid');", True),
    ]
    if engine == "mnemonic":
        steps.append(("create_index", "CREATE INDEX person_age ON d.person (age);", True))
    else:
        steps.append(("create_index", "CREATE INDEX person_age ON d.person (age);", True))
    steps += [
        ("range_indexed", "SELECT count(*), sum(score) FROM d.person WHERE age BETWEEN 10 AND 12;", True),
        ("join_2way", "SELECT count(*), sum(a.balance), sum(p.score) FROM d.person p JOIN d.account a ON a.id = p.id;", True),
        ("join_selective", "SELECT count(*), sum(p.score) FROM d.person p JOIN d.vip v ON v.id = p.id;", True),
    ]
    if engine == "mnemonic":
        steps += [
            ("join_2way_unfused", "SET mnemonicdb_fuse_scans = false; SELECT count(*), sum(a.balance), sum(p.score) FROM d.person p JOIN d.account a ON a.id = p.id; SET mnemonicdb_fuse_scans = true;", True),
            ("join_selective_unfused", "SET mnemonicdb_fuse_scans = false; SELECT count(*), sum(p.score) FROM d.person p JOIN d.vip v ON v.id = p.id; SET mnemonicdb_fuse_scans = true;", True),
            ("join_filtered", "SELECT count(*), sum(a.balance) FROM d.person p JOIN d.account a ON a.id = p.id WHERE p.age = 7;", True),
        ]
    else:
        steps.append(("join_filtered", "SELECT count(*), sum(a.balance) FROM d.person p JOIN d.account a ON a.id = p.id WHERE p.age = 7;", True))
    steps += [
        ("update_10pct", "UPDATE d.person SET score = score + 1 WHERE age < 10;", True),
        ("update_point", "UPDATE d.person SET score = 0 WHERE email = 'e777';", True),
        ("delete_1pct", "DELETE FROM d.account WHERE balance = 70;", True),
    ]
    if engine == "mnemonic":
        steps += [
            ("at_scan", "SELECT sum(score) FROM d.person AT (VERSION => 4);", True),
            ("at_point", "SELECT score FROM d.person AT (VERSION => 4) WHERE email = 'e777';", True),
        ]
    return steps


def run(engine, workdir):
    path = os.path.join(workdir, "db." + engine)
    steps = workload(engine, path)
    script = [".timer on"]
    for label, sql, timed in steps:
        script.append(f".print @@{label}")
        script.append(sql)
    out = subprocess.run([args.duckdb, "-list", "-noheader"], input="\n".join(script) + "\n", capture_output=True,
                         text=True)
    if out.returncode != 0 or "Error" in out.stdout or "Error" in out.stderr:
        print(out.stdout[-3000:], out.stderr[-3000:], file=sys.stderr)
    times = {}
    label = None
    for line in out.stdout.splitlines():
        if line.startswith("@@"):
            label = line[2:]
            continue
        m = re.match(r"Run Time \(s\): real ([0-9.]+)", line)
        if m and label:
            times[label] = times.get(label, 0.0) + float(m.group(1))
    timed = {l for l, _, t in steps if t}
    times = {k: v for k, v in times.items() if k in timed}

    # Many small autocommit transactions
    statements = [".timer off", f"ATTACH '{path}' AS d" + (" (TYPE mnemonicdb);" if engine == "mnemonic" else ";")]
    statements.append(".timer on")
    for i in range(args.small_tx):
        if engine == "mnemonic":
            statements.append(f"INSERT INTO d.account (balance) VALUES ({i});")
        else:
            statements.append(f"INSERT INTO d.account VALUES ({10_000_000 + i}, {i});")
    out = subprocess.run([args.duckdb, "-list", "-noheader"], input="\n".join(statements) + "\n", capture_output=True,
                         text=True)
    small = sum(float(m) for m in re.findall(r"Run Time \(s\): real ([0-9.]+)", out.stdout))
    times[f"small_tx_x{args.small_tx}"] = small

    size = 0
    if os.path.isdir(path):
        for root, _, files in os.walk(path):
            size += sum(os.path.getsize(os.path.join(root, f)) for f in files)
    else:
        size = os.path.getsize(path) + (os.path.getsize(path + ".wal") if os.path.exists(path + ".wal") else 0)
    times["size_mb"] = size / 1e6
    return times


workdir = tempfile.mkdtemp(prefix="mnemonic-bench-", dir=os.environ.get("BENCH_DIR"))
try:
    results = {}
    for engine in args.engines.split(","):
        results[engine] = run(engine, workdir)
    labels = []
    for engine in results:
        for label in results[engine]:
            if label not in labels:
                labels.append(label)
    print(f"rows={N}")
    print(f"{'benchmark':22}" + "".join(f"{e:>14}" for e in results) + ("      ratio" if len(results) == 2 else ""))
    for label in labels:
        values = [results[e].get(label) for e in results]
        line = f"{label:22}" + "".join(f"{v:14.4f}" if v is not None else f"{'-':>14}" for v in values)
        if len(values) == 2 and None not in values and values[1] > 0:
            line += f"{values[0] / values[1]:10.1f}x"
        print(line)
finally:
    shutil.rmtree(workdir, ignore_errors=True)
