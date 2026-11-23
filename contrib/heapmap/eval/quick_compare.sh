#!/usr/bin/env bash
# First comparison of heapmap with btree and BRIN on an append-ordered table.
# Same rows in the same physical order in every table, one index each.
#   eval/quick_compare.sh [rows]        (default 10,000,000)
# SKIP_LOAD=1 reuses the tables from a previous run.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CL="$HERE/../cluster.sh"
ROWS="${1:-10000000}"
Q() { "$CL" psql -v ON_ERROR_STOP=1 -qAt "$@"; }
KINDS="btree brin brin8 heapmap"

index_sql() {   # kind table
  case "$1" in
    btree)   echo "CREATE INDEX $2_idx ON $2 USING btree (k)" ;;
    brin)    echo "CREATE INDEX $2_idx ON $2 USING brin (k)" ;;
    brin8)   echo "CREATE INDEX $2_idx ON $2 USING brin (k) WITH (pages_per_range = 8)" ;;
    heapmap) echo "CREATE INDEX $2_idx ON $2 USING heapmap (k)" ;;
  esac
}

if [ -z "${SKIP_LOAD:-}" ]; then
echo "== loading $ROWS rows (keys increase with a varying gap, like event timestamps)"
Q <<SQL
CREATE EXTENSION IF NOT EXISTS heapmap;
CREATE EXTENSION IF NOT EXISTS pg_prewarm;
DROP TABLE IF EXISTS c_src, c_sample, c_ranges, c_more;
CREATE UNLOGGED TABLE c_src AS
  SELECT 1700000000000000 + sum(1 + (random() * 2000)::int) OVER (ORDER BY i) AS k, (i % 100000)::int AS payload
  FROM generate_series(1, $ROWS) i;
CREATE UNLOGGED TABLE c_more AS
  SELECT (SELECT max(k) FROM c_src) + sum(1 + (random() * 2000)::int) OVER (ORDER BY i) AS k, (i % 100000)::int AS payload
  FROM generate_series(1, $ROWS / 5) i;
CREATE TABLE c_sample (k bigint);
INSERT INTO c_sample SELECT k FROM c_src WHERE random() < 200000.0 / $ROWS;
CREATE TABLE c_ranges AS SELECT k AS lo, k + 10000 * 1000 AS hi FROM c_sample LIMIT 200;
SQL

for kind in $KINDS; do
  Q -c "DROP TABLE IF EXISTS c_$kind" -c "CREATE TABLE c_$kind AS SELECT * FROM c_src" -c "VACUUM (ANALYZE) c_$kind"
done
fi

for kind in $KINDS; do
  Q <<SQL
DROP FUNCTION IF EXISTS c_lookup_$kind();
CREATE OR REPLACE FUNCTION c_lookup_$kind(lim int) RETURNS float8 AS \$\$
DECLARE keys bigint[]; key_val bigint; v int; t0 timestamptz; n int;
BEGIN
    SELECT array_agg(k) INTO keys FROM (SELECT k FROM c_sample LIMIT lim) s;
    n := array_length(keys, 1);
    t0 := clock_timestamp();
    FOREACH key_val IN ARRAY keys LOOP
        SELECT payload INTO v FROM c_$kind WHERE k = key_val;
    END LOOP;
    RETURN extract(epoch FROM clock_timestamp() - t0) * 1e6 / n;
END \$\$ LANGUAGE plpgsql;
CREATE OR REPLACE FUNCTION c_range_$kind() RETURNS float8 AS \$\$
DECLARE r record; v bigint; t0 timestamptz; n int := 0;
BEGIN
    t0 := clock_timestamp();
    FOR r IN SELECT lo, hi FROM c_ranges LOOP
        SELECT count(payload) INTO v FROM c_$kind WHERE k BETWEEN r.lo AND r.hi;
        n := n + 1;
    END LOOP;
    RETURN extract(epoch FROM clock_timestamp() - t0) * 1e6 / n;
END \$\$ LANGUAGE plpgsql;
SQL
done

echo "== index build time (seconds, median of 3, one core)"
for kind in $KINDS; do
  times=""
  for r in 1 2 3; do
    Q -c "DROP INDEX IF EXISTS c_${kind}_idx"
    t0=$(date +%s.%N)
    Q -c "SET max_parallel_maintenance_workers = 0; SET maintenance_work_mem = '2GB'; $(index_sql $kind c_$kind)"
    t1=$(date +%s.%N)
    times="$times $(echo "$t1 - $t0" | bc)"
  done
  echo "$kind: $(echo $times | tr ' ' '\n' | sort -n | sed -n 2p)"
done
Q -c "ANALYZE" -c "CHECKPOINT"

echo "== index size"
for kind in $KINDS; do
  echo "$kind: $(Q -c "SELECT pg_size_pretty(pg_relation_size('c_${kind}_idx')) || '  (' || pg_relation_size('c_${kind}_idx') || ' bytes)'")"
done
echo "table: $(Q -c "SELECT pg_size_pretty(pg_relation_size('c_btree'))")"
Q -c "SELECT 'heapmap model: ' || hm.segments || ' segments, window avg ' || round(hm.avg_window::numeric, 2) || ' max ' || hm.max_window || ', ' || hm.exception_entries || ' exceptions' FROM heapmap_stats('c_heapmap_idx') hm"

echo "== pages touched by one point lookup (index + table), averaged over 300 keys"
for kind in $KINDS; do
  Q -c "SELECT pg_prewarm('c_$kind'), pg_prewarm('c_${kind}_idx')" >/dev/null
  Q <<SQL
DO \$\$
DECLARE key_val bigint; j json; idx float8 := 0; tot float8 := 0; n int := 0;
BEGIN
  FOR key_val IN SELECT k FROM c_sample LIMIT 300 LOOP
    EXECUTE format('EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) SELECT payload FROM c_$kind WHERE k = %s', key_val) INTO j;
    tot := tot + (j->0->'Plan'->>'Shared Hit Blocks')::float8 + (j->0->'Plan'->>'Shared Read Blocks')::float8;
    IF j->0->'Plan'->'Plans' IS NOT NULL THEN
      idx := idx + (j->0->'Plan'->'Plans'->0->>'Shared Hit Blocks')::float8 + (j->0->'Plan'->'Plans'->0->>'Shared Read Blocks')::float8;
    END IF;
    n := n + 1;
  END LOOP;
  RAISE NOTICE '$kind: total % pages, of which index % (plan: %)', round((tot / n)::numeric, 2), round((idx / n)::numeric, 2),
     (SELECT j->0->'Plan'->>'Node Type');
END \$\$;
SQL
done 2>&1 | sed 's/^NOTICE:  //'

echo "== warm point lookup inside the server (microseconds, median of 5 alternating rounds)"
declare -A LK RG
for r in 1 2 3 4 5; do
  order="$KINDS"; [ $((r % 2)) -eq 0 ] && order="$(echo $KINDS | tr ' ' '\n' | tac | tr '\n' ' ')"
  for kind in $order; do
    Q -c "SELECT pg_prewarm('c_$kind'), pg_prewarm('c_${kind}_idx')" >/dev/null
    n=200000; [ "$kind" = brin ] && n=2000; [ "$kind" = brin8 ] && n=10000
    Q -c "SELECT c_lookup_$kind($n)" >/dev/null
    LK[$kind]="${LK[$kind]:-} $(Q -c "SELECT round(c_lookup_$kind($n)::numeric, 3)")"
    RG[$kind]="${RG[$kind]:-} $(Q -c "SELECT round(c_range_$kind()::numeric, 1)")"
  done
done
for kind in $KINDS; do echo "$kind: $(echo ${LK[$kind]} | tr ' ' '\n' | sort -n | sed -n 3p)"; done
echo "== range query returning about 10,000 rows (microseconds, median of 5)"
for kind in $KINDS; do echo "$kind: $(echo ${RG[$kind]} | tr ' ' '\n' | sort -n | sed -n 3p)"; done

echo "== loading $((ROWS / 5)) more rows in order (seconds, and WAL written)"
for kind in $KINDS; do
  Q -c "CHECKPOINT"
  l0=$(Q -c "SELECT pg_current_wal_lsn()")
  t0=$(date +%s.%N)
  Q -c "INSERT INTO c_$kind SELECT * FROM c_more"
  t1=$(date +%s.%N)
  wal=$(Q -c "SELECT pg_size_pretty(pg_wal_lsn_diff(pg_current_wal_lsn(), '$l0'))")
  echo "$kind: $(echo "$t1 - $t0" | bc) s, WAL $wal, index now $(Q -c "SELECT pg_size_pretty(pg_relation_size('c_${kind}_idx'))")"
done
Q -c "SELECT 'heapmap after load: ' || hm.segments || ' segments, window avg ' || round(hm.avg_window::numeric, 2) || ', tail ' || hm.tail_pages || ' pages, ' || hm.exception_entries || ' exceptions, ' || hm.folds || ' folds' FROM heapmap_stats('c_heapmap_idx') hm"
echo "== heapmap still agrees with a table scan"
Q -f "$HERE/../test/checks.sql"
Q -c "ANALYZE c_heapmap" -c "SELECT hm_check('c_heapmap', 10)"
