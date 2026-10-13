#!/usr/bin/env bash
# Small transactions against a large database spread over many SST files.
# Usage: bench/warm_writes.sh DUCKDB DBDIR [ROWS] [OPS]
set -e
DUCKDB=$1; DB=$2; ROWS=${3:-1000000}; OPS=${4:-2000}
rm -rf "$DB"
setup="ATTACH '$DB' AS d (TYPE mnemonicdb);
CREATE TABLE d.person (name VARCHAR, age INTEGER, email VARCHAR UNIQUE, score DOUBLE);"
for chunk in 0 1 2 3 4 5 6 7 8 9; do
  lo=$((chunk * ROWS / 10)); hi=$(((chunk + 1) * ROWS / 10))
  setup="$setup
INSERT INTO d.person (name, age, email, score) SELECT 'p' || i, i % 100, 'e' || i, i / 7.0 FROM range($lo, $hi) t(i);
CHECKPOINT d;"
done
setup="$setup
UPDATE d.person SET score = score + 1 WHERE age < 20; CHECKPOINT d;
UPDATE d.person SET score = score + 1 WHERE age >= 80; CHECKPOINT d;"
echo "$setup" | $DUCKDB > /dev/null
echo "sst files: $(ls $DB/*.sst | wc -l)"
run() {
  local label=$1 file=$2
  local t=$($DUCKDB -list -noheader -f "$file" 2>&1 | grep "Run Time" | awk '{s+=$5} END {printf "%.3f", s}')
  echo "$label: $t s"
}
f=$(mktemp)
{ echo "ATTACH '$DB' AS d (TYPE mnemonicdb);"; echo ".timer on"; for i in $(seq 1 $OPS); do echo "INSERT INTO d.person (name, age, email) VALUES ('n$i', 1, 'new$i');"; done; } > $f
run "insert_new_entity_x$OPS" $f
{ echo "ATTACH '$DB' AS d (TYPE mnemonicdb);"; echo ".timer on"; for i in $(seq 1 $OPS); do echo "UPDATE d.person SET score = $i WHERE email = 'e$(( (i * 7919) % ROWS ))';"; done; } > $f
run "update_by_email_x$OPS" $f
{ echo "ATTACH '$DB' AS d (TYPE mnemonicdb);"; echo ".timer on"; echo "INSERT INTO d.person (name, age, email) SELECT 'b' || i, 2, 'bulk' || i FROM range(100000) t(i);"; } > $f
run "insert_100k_one_tx" $f
{ echo "ATTACH '$DB' AS d (TYPE mnemonicdb);"; echo ".timer on"; echo "UPDATE d.person SET score = 0 WHERE age = 50;"; } > $f
run "update_10k_one_tx" $f
rm -f $f
