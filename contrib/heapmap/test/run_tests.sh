#!/usr/bin/env bash
# Correctness suite for the heapmap index. Every check compares the rows the
# index returns with a sequential scan. Stops at the first failure.
#   test/run_tests.sh [rows] [rounds]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CL="$HERE/../cluster.sh"
ROWS="${1:-200000}"
ROUNDS="${2:-40}"
Q() { "$CL" psql -v ON_ERROR_STOP=1 -qAt "$@"; }

Q -c 'DROP EXTENSION IF EXISTS heapmap CASCADE' -c 'CREATE EXTENSION heapmap' 2>/dev/null
Q -f "$HERE/checks.sql"

echo "== build and read"
Q <<SQL
DROP TABLE IF EXISTS t_sorted, t_jitter, t_late, t_batch, t_dup, t_random, t_neg, t_null,
                     t_empty, t_one, t_same, t_ts, t_int4, t_date, t_wide;
-- rows in key order, like a log
CREATE TABLE t_sorted AS SELECT i::bigint AS k FROM generate_series(1, $ROWS) i;
-- each row up to 2000 positions (about 9 pages) away from its place
CREATE TABLE t_jitter AS SELECT k FROM (SELECT i::bigint * 10 AS k, i + random() * 2000 AS o FROM generate_series(1, $ROWS) i) s ORDER BY o;
-- in order, except that 2% of rows carry a key from anywhere in the past
CREATE TABLE t_late AS SELECT CASE WHEN random() < 0.02 THEN (random() * i)::bigint ELSE i::bigint END * 10 AS k FROM generate_series(1, $ROWS) i;
-- in order, with a whole batch of old keys and a batch of far-future keys in the middle
CREATE TABLE t_batch AS SELECT k FROM (
    SELECT i::bigint * 10 AS k, i::float8 AS o FROM generate_series(1, $ROWS) i
    UNION ALL SELECT (random() * 1000)::bigint, $ROWS * 0.6 + random() FROM generate_series(1, $ROWS / 20)
    UNION ALL SELECT 4000000000000 + i, $ROWS * 0.3 + random() FROM generate_series(1, $ROWS / 20) i) s ORDER BY o;
-- few distinct keys, each filling many pages
CREATE TABLE t_dup AS SELECT (i / 5000)::bigint * 1000 AS k FROM generate_series(1, $ROWS) i;
-- no order at all: the worst case, everything becomes an exception
CREATE TABLE t_random AS SELECT (random() * 9e18)::bigint AS k FROM generate_series(1, $ROWS / 4);
CREATE TABLE t_neg AS SELECT -4000000000000000000 + i::bigint * 1000000000 AS k FROM generate_series(1, $ROWS) i;
CREATE TABLE t_null AS SELECT CASE WHEN i % 3 = 0 THEN NULL ELSE i::bigint END AS k FROM generate_series(1, $ROWS) i;
CREATE TABLE t_empty (k bigint);
CREATE TABLE t_one AS SELECT 42::bigint AS k;
CREATE TABLE t_same AS SELECT 7::bigint AS k FROM generate_series(1, 5000);
CREATE TABLE t_ts AS SELECT timestamptz '2026-01-01 00:00:00+00' + i * interval '1.5 second' AS k FROM generate_series(1, $ROWS) i;
CREATE TABLE t_int4 AS SELECT (i * 3)::int4 AS k FROM generate_series(1, $ROWS) i;
CREATE TABLE t_date AS SELECT date '2000-01-01' + (i / 300) AS k FROM generate_series(1, $ROWS) i;
-- a second column, so updates that leave the key alone can stay on the page
CREATE TABLE t_wide AS SELECT i::bigint AS k, 0 AS v, repeat('x', 40) AS pad FROM generate_series(1, $ROWS) i;
ALTER TABLE t_wide SET (fillfactor = 70);

CREATE INDEX i_sorted ON t_sorted USING heapmap (k);
CREATE INDEX i_jitter ON t_jitter USING heapmap (k);
CREATE INDEX i_late ON t_late USING heapmap (k);
CREATE INDEX i_batch ON t_batch USING heapmap (k);
CREATE INDEX i_dup ON t_dup USING heapmap (k);
CREATE INDEX i_random ON t_random USING heapmap (k);
CREATE INDEX i_neg ON t_neg USING heapmap (k);
CREATE INDEX i_null ON t_null USING heapmap (k);
CREATE INDEX i_empty ON t_empty USING heapmap (k);
CREATE INDEX i_one ON t_one USING heapmap (k);
CREATE INDEX i_same ON t_same USING heapmap (k);
CREATE INDEX i_ts ON t_ts USING heapmap (k);
CREATE INDEX i_int4 ON t_int4 USING heapmap (k);
CREATE INDEX i_date ON t_date USING heapmap (k);
CREATE INDEX i_wide ON t_wide USING heapmap (k);
ANALYZE;
SQL

TABLES="t_sorted t_jitter t_late t_batch t_dup t_random t_neg t_null t_empty t_one t_same t_ts t_int4 t_date t_wide"
INDEXES="i_sorted i_jitter i_late i_batch i_dup i_random i_neg i_null i_empty i_one i_same i_ts i_int4 i_date i_wide"
check_all() { for t in $TABLES; do Q -c "SELECT hm_check('$t', $ROUNDS)"; done; }
summary() { for i in "$@"; do Q -c "SELECT hm_summary('$i')"; done; }
check_all
summary $INDEXES

echo "== inserts: appended in order, appended late, into empty tables"
Q <<SQL
INSERT INTO t_sorted SELECT i::bigint FROM generate_series($ROWS + 1, $ROWS * 2) i;
INSERT INTO t_sorted SELECT (random() * $ROWS)::bigint FROM generate_series(1, $ROWS / 50);
INSERT INTO t_jitter SELECT k FROM (SELECT ($ROWS + i)::bigint * 10 AS k, i + random() * 2000 AS o FROM generate_series(1, $ROWS) i) s ORDER BY o;
INSERT INTO t_empty SELECT i::bigint FROM generate_series(1, $ROWS) i;
INSERT INTO t_empty SELECT (random() * $ROWS * 2)::bigint FROM generate_series(1, 5000);
INSERT INTO t_one SELECT i::bigint FROM generate_series(1, 20000) i;
INSERT INTO t_same SELECT 7 FROM generate_series(1, 20000);
INSERT INTO t_same SELECT i::bigint FROM generate_series(1, 20000) i;
INSERT INTO t_dup SELECT ($ROWS / 5000 + 1 + i / 5000)::bigint * 1000 FROM generate_series(1, $ROWS / 2) i;
INSERT INTO t_random SELECT (random() * 9e18)::bigint FROM generate_series(1, $ROWS / 4);
INSERT INTO t_ts SELECT timestamptz '2026-01-01 00:00:00+00' + ($ROWS + i) * interval '1.5 second' FROM generate_series(1, $ROWS / 2) i;
INSERT INTO t_neg VALUES (9223372036854775807), (-9223372036854775808), (NULL);
INSERT INTO t_null SELECT CASE WHEN i % 2 = 0 THEN NULL ELSE ($ROWS + i)::bigint END FROM generate_series(1, $ROWS / 2) i;
ANALYZE;
SQL
check_all
summary i_sorted i_jitter i_empty i_one i_same i_dup i_random i_ts

echo "== one row at a time, as an application would insert"
Q -c "DO \$\$ BEGIN FOR i IN 1..30000 LOOP INSERT INTO t_one VALUES (100000 + i); END LOOP; END \$\$" -c "ANALYZE t_one"
Q -c "SELECT hm_check('t_one', $ROUNDS)"
summary i_one

echo "== updates that move rows, updates that do not, deletes, vacuum, reuse of freed space"
Q <<SQL
UPDATE t_late SET k = k + 1 WHERE k % 7 = 0;
UPDATE t_wide SET v = v + 1 WHERE k % 3 = 0;
UPDATE t_wide SET k = k + $ROWS * 3 WHERE k % 50 = 0;
DELETE FROM t_sorted WHERE k % 3 = 0;
DELETE FROM t_dup WHERE k = 0;
DELETE FROM t_random WHERE k % 2 = 0;
VACUUM t_late, t_wide, t_sorted, t_dup, t_random;
INSERT INTO t_sorted SELECT ($ROWS * 2 + i)::bigint FROM generate_series(1, $ROWS / 2) i;
INSERT INTO t_random SELECT (random() * 9e18)::bigint FROM generate_series(1, $ROWS / 8);
ANALYZE;
SQL
check_all
summary i_late i_wide i_sorted i_random

echo "== reindex rebuilds the model and drops the exceptions it no longer needs"
Q -c "REINDEX TABLE t_sorted" -c "REINDEX TABLE t_wide" -c "REINDEX TABLE t_empty" -c "REINDEX TABLE t_one" -c "ANALYZE"
check_all
summary i_sorted i_wide i_empty i_one

echo "== concurrent writers, readers and deleters"
ENV="$("$CL" env)"
eval "export $ENV"
PGB="$BIN/pgbench"
SCRIPT="$(mktemp -d)"
Q -c "DROP TABLE IF EXISTS t_live" -c "DROP SEQUENCE IF EXISTS t_live_seq" \
  -c "CREATE SEQUENCE t_live_seq" -c "CREATE TABLE t_live (k bigint)" \
  -c "CREATE INDEX i_live ON t_live USING heapmap (k) WITH (fold_after = 6, keep = 2)"
cat > "$SCRIPT/append.sql" <<SQL
INSERT INTO t_live VALUES (nextval('t_live_seq'));
SQL
cat > "$SCRIPT/batch.sql" <<SQL
INSERT INTO t_live SELECT nextval('t_live_seq') FROM generate_series(1, 300);
SQL
cat > "$SCRIPT/late.sql" <<SQL
\set k random(1, 200000)
INSERT INTO t_live VALUES (:k);
SQL
cat > "$SCRIPT/read.sql" <<SQL
\set k random(1, 400000)
SELECT count(*) FROM t_live WHERE k = :k;
SELECT count(*) FROM t_live WHERE k BETWEEN :k AND :k + 50;
SQL
cat > "$SCRIPT/del.sql" <<SQL
\set k random(1, 400000)
DELETE FROM t_live WHERE k = :k;
SQL
OUT="$("$PGB" -n -c 16 -j 4 -T 20 -f "$SCRIPT/append.sql@6" -f "$SCRIPT/batch.sql@2" -f "$SCRIPT/late.sql@1" \
       -f "$SCRIPT/read.sql@4" -f "$SCRIPT/del.sql@1" 2>&1)" || { echo "$OUT"; exit 1; }
echo "$OUT" | grep -E 'number of transactions actually|number of failed|tps'
echo "$OUT" | grep -q 'number of failed transactions: 0 ' || { echo "$OUT"; exit 1; }
Q -c "ANALYZE t_live" -c "SELECT hm_check('t_live', $ROUNDS)"
summary i_live
Q -c "VACUUM t_live" -c "ANALYZE t_live" -c "SELECT hm_check('t_live', $ROUNDS)"
summary i_live

echo "== crash recovery (immediate stop right after unflushed inserts)"
Q <<SQL
CREATE TABLE IF NOT EXISTS crash_marker (n bigint);
TRUNCATE crash_marker;
INSERT INTO t_live SELECT nextval('t_live_seq') FROM generate_series(1, 200000);
INSERT INTO t_live SELECT (random() * 100000)::bigint FROM generate_series(1, 5000);
INSERT INTO crash_marker SELECT count(*) FROM t_live;
SQL
BEFORE="$(Q -c 'SELECT n FROM crash_marker')"
"$CL" crash
"$CL" start
AFTER="$(Q -c 'SELECT count(*) FROM t_live')"
[ "$BEFORE" = "$AFTER" ] || { echo "row count changed across crash: $BEFORE vs $AFTER"; exit 1; }
Q -c "ANALYZE t_live" -c "SELECT hm_check('t_live', $ROUNDS)"
summary i_live
echo "recovered $AFTER rows"

rm -rf "$SCRIPT"
echo "ALL CHECKS PASSED"
