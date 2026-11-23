#!/usr/bin/env bash
# How the index holds up when many sessions insert at once. Concurrent inserts
# scatter neighbouring keys over many table pages, which is the hard case for an
# index that stores a model of "key -> page" and nothing per row.
#   eval/concurrent.sh [seconds per run] [client counts...]     (default 10, then 1 4 16 32)
# MIX=1 adds batch inserts, late rows, readers and deleters, as in the test suite.
# WITH="fold_after = 6, keep = 2" passes index options.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CL="$HERE/../cluster.sh"
SECS="${1:-10}"
shift || true
CLIENTS="${*:-1 4 16 32}"
Q() { "$CL" psql -v ON_ERROR_STOP=1 -qAt "$@"; }
eval "export $("$CL" env)"

Q -c "CREATE EXTENSION IF NOT EXISTS heapmap" -f "$HERE/../test/checks.sql"
DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT
echo "INSERT INTO cc_live VALUES (nextval('cc_seq'));" > "$DIR/append.sql"
echo "INSERT INTO cc_live SELECT nextval('cc_seq') FROM generate_series(1, 300);" > "$DIR/batch.sql"
printf '%s\n' '\set k random(1, 200000)' 'INSERT INTO cc_live VALUES (:k);' > "$DIR/late.sql"
printf '%s\n' '\set k random(1, 400000)' 'SELECT count(*) FROM cc_live WHERE k = :k;' \
  'SELECT count(*) FROM cc_live WHERE k BETWEEN :k AND :k + 50;' > "$DIR/read.sql"
printf '%s\n' '\set k random(1, 400000)' 'DELETE FROM cc_live WHERE k = :k;' > "$DIR/del.sql"

SCRIPTS="-f $DIR/append.sql"
[ -n "${MIX:-}" ] && SCRIPTS="-f $DIR/append.sql@6 -f $DIR/batch.sql@2 -f $DIR/late.sql@1 -f $DIR/read.sql@4 -f $DIR/del.sql@1"

printf '%-8s %10s %10s %7s %9s %7s %6s %9s %10s\n' clients rows exceptions exc% segments window tail index btree
for c in $CLIENTS; do
  Q -c "DROP TABLE IF EXISTS cc_live" -c "DROP SEQUENCE IF EXISTS cc_seq" -c "CREATE SEQUENCE cc_seq" \
    -c "CREATE TABLE cc_live (k bigint)" \
    -c "CREATE INDEX cc_idx ON cc_live USING heapmap (k) ${WITH:+WITH ($WITH)}"
  OUT="$("$BIN/pgbench" -n -c "$c" -j "$(( c < 4 ? c : 4 ))" -T "$SECS" $SCRIPTS 2>&1)" || { echo "$OUT"; exit 1; }
  echo "$OUT" | grep -q 'number of failed transactions: 0 ' || { echo "$OUT"; exit 1; }
  Q -c "ANALYZE cc_live" -c "SELECT hm_check('cc_live', 10)" >/dev/null
  Q -c "CREATE INDEX cc_bt ON cc_live (k)"
  Q -F ' ' -c "SELECT format('%-8s %10s %10s %7s %9s %7s %6s %9s %10s', $c, n, exception_entries,
        round(100.0 * exception_entries / greatest(n, 1), 2), segments, round(avg_window::numeric, 1), tail_pages,
        pg_size_pretty(index_bytes), pg_size_pretty(pg_relation_size('cc_bt')))
      FROM heapmap_stats('cc_idx'), (SELECT count(*) n FROM cc_live) t"
done
