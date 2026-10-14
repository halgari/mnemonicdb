#!/usr/bin/env bash
# Fails if any global (strong) symbol is defined in more than one object across the given static archives.
# Usage: scripts/check_duplicate_symbols.sh [--xxh-only] ARCHIVE.a [ARCHIVE.a ...]
# Pass archives that do NOT contain each other (e.g. libmnemonicdb_extension.a, librocksdb.a, libmnemonicdb_lz4.a,
# libmnemonicdb_zstd.a, DuckDB's component archives); a merged archive plus its parts would report false duplicates.
# Weak/COMDAT symbols (C++ inline functions, templates) are ignored: only T, D, B, R and their data variants count.
set -euo pipefail
filter='.'
if [ "${1:-}" = "--xxh-only" ]; then filter='XXH'; shift; fi
[ $# -gt 0 ] || { echo "usage: $0 [--xxh-only] archive.a..." >&2; exit 2; }
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
for a in "$@"; do
  [ -f "$a" ] || { echo "missing archive: $a" >&2; exit 2; }
  # nm -A line: "archive:member: addr type name" (paths with spaces are not supported)
  nm -A --defined-only "$a" 2>/dev/null | awk '$(NF-1) ~ /^[TDBR]$/ {sub(/:[0-9a-fA-F]+$/, "", $1); print $NF "\t" $1}' >> "$tmp"
done
dups=$(grep -E "$filter" "$tmp" | awk -F'\t' '{c[$1]++; o[$1]=o[$1] "\n    " $2} END {for (s in c) if (c[s] > 1) print s ":" o[s]}')
if [ -n "$dups" ]; then
  echo "FAIL: symbols defined in more than one object:" >&2
  echo "$dups" >&2
  exit 1
fi
echo "OK: no duplicate global symbols across $# archive(s)"
