#!/usr/bin/env bash
# How build time, index size and lookup cost scale with the table size, for
# btree, BRIN and heapmap on the same append-ordered rows.
#   eval/scale.sh [row counts...]        (default 1000000 5000000 20000000 50000000)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CL="$HERE/../cluster.sh"
SIZES="${*:-1000000 5000000 20000000 50000000}"
Q() { "$CL" psql -v ON_ERROR_STOP=1 -qAt "$@"; }
KINDS="btree brin heapmap"
index_sql() { case "$1" in
  btree)   echo "CREATE INDEX s_$1_idx ON s_$1 USING btree (k)" ;;
  brin)    echo "CREATE INDEX s_$1_idx ON s_$1 USING brin (k)" ;;
  heapmap) echo "CREATE INDEX s_$1_idx ON s_$1 USING heapmap (k)" ;;
esac; }

Q -c "CREATE EXTENSION IF NOT EXISTS heapmap" -c "CREATE EXTENSION IF NOT EXISTS pg_prewarm"
for kind in $KINDS; do Q <<SQL
CREATE OR REPLACE FUNCTION s_lookup_$kind(lim int) RETURNS float8 AS \$\$
DECLARE keys bigint[]; key_val bigint; v int; t0 timestamptz; n int;
BEGIN
    SELECT array_agg(k) INTO keys FROM (SELECT k FROM s_sample LIMIT lim) s;
    n := array_length(keys, 1);
    t0 := clock_timestamp();
    FOREACH key_val IN ARRAY keys LOOP
        SELECT payload INTO v FROM s_$kind WHERE k = key_val;
    END LOOP;
    RETURN extract(epoch FROM clock_timestamp() - t0) * 1e6 / n;
END \$\$ LANGUAGE plpgsql;
SQL
done

printf '%-10s %-8s %9s %14s %10s %11s %s\n' rows index build_s index_bytes pages/look lookup_us heapmap_model
for N in $SIZES; do
  Q -c "DROP TABLE IF EXISTS s_src, s_sample, s_btree, s_brin, s_heapmap" \
    -c "CREATE UNLOGGED TABLE s_src AS SELECT 1700000000000000 + sum(1 + (random() * 2000)::int) OVER (ORDER BY i) AS k, (i % 100000)::int AS payload FROM generate_series(1, $N) i" \
    -c "CREATE TABLE s_sample AS SELECT k FROM s_src WHERE random() < 30000.0 / $N"
  for kind in $KINDS; do
    Q -c "CREATE TABLE s_$kind AS SELECT * FROM s_src" -c "VACUUM (ANALYZE) s_$kind"
    t0=$(date +%s.%N)
    Q -c "SET max_parallel_maintenance_workers = 0; SET maintenance_work_mem = '4GB'; $(index_sql $kind)"
    t1=$(date +%s.%N)
    build=$(echo "$t1 - $t0" | bc)
    Q -c "ANALYZE s_$kind" -c "SELECT pg_prewarm('s_$kind'), pg_prewarm('s_${kind}_idx')" >/dev/null
    size=$(Q -c "SELECT pg_relation_size('s_${kind}_idx')")
    pages=$(Q <<SQL 2>&1 | sed 's/^NOTICE:  //'
DO \$\$
DECLARE key_val bigint; j json; tot float8 := 0; n int := 0;
BEGIN
  FOR key_val IN SELECT k FROM s_sample LIMIT 200 LOOP
    EXECUTE format('EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) SELECT payload FROM s_$kind WHERE k = %s', key_val) INTO j;
    tot := tot + (j->0->'Plan'->>'Shared Hit Blocks')::float8 + (j->0->'Plan'->>'Shared Read Blocks')::float8;
    n := n + 1;
  END LOOP;
  RAISE NOTICE '%', round((tot / n)::numeric, 1);
END \$\$;
SQL
)
    n=20000; [ "$kind" = brin ] && n=300
    Q -c "SELECT s_lookup_$kind($n)" >/dev/null
    us=$(Q -c "SELECT round(s_lookup_$kind($n)::numeric, 1)")
    model=""
    [ "$kind" = heapmap ] && model=$(Q -c "SELECT segments || ' segments, window ' || round(avg_window::numeric, 1) || ', ' || exception_entries || ' exceptions' FROM heapmap_stats('s_heapmap_idx')")
    printf '%-10s %-8s %9s %14s %10s %11s %s\n' "$N" "$kind" "$build" "$size" "$pages" "$us" "$model"
    Q -c "DROP TABLE s_$kind"
  done
done
