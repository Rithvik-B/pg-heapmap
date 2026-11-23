#!/usr/bin/env bash
# A real event stream: one day of public GitHub events from GH Archive, in the
# order they were recorded, indexed on created_at. The question is whether a
# real timestamp column follows physical order well enough for heapmap.
#   eval/gharchive.sh [events.csv]
# The CSV comes from eval/gharchive_extract.py over the hour files of one day:
#   for h in $(seq 0 23); do curl -O https://data.gharchive.org/2024-01-01-$h.json.gz; done
#   eval/gharchive_extract.py 2024-01-01-*.json.gz > events.csv     (keep file order: 0..23)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CL="$HERE/../cluster.sh"
CSV="${1:-$HERE/data/gh_events.csv}"
[ -r "$CSV" ] || { echo "no CSV at $CSV" >&2; exit 1; }
Q() { "$CL" psql -v ON_ERROR_STOP=1 -qAt "$@"; }
KINDS="btree brin heapmap"
index_sql() { case "$1" in
  btree)   echo "CREATE INDEX gh_$1_idx ON gh_$1 USING btree (created_at)" ;;
  brin)    echo "CREATE INDEX gh_$1_idx ON gh_$1 USING brin (created_at)" ;;
  heapmap) echo "CREATE INDEX gh_$1_idx ON gh_$1 USING heapmap (created_at)" ;;
esac; }

echo "== loading $CSV"
Q -c "CREATE EXTENSION IF NOT EXISTS heapmap" -c "CREATE EXTENSION IF NOT EXISTS pg_prewarm" \
  -c "DROP TABLE IF EXISTS gh_src, gh_btree, gh_brin, gh_heapmap, gh_sample, gh_ranges CASCADE" \
  -c "CREATE UNLOGGED TABLE gh_src (seq bigserial, id bigint, created_at timestamptz, type text, repo text, actor text)" \
  -c "\\copy gh_src (id, created_at, type, repo, actor) FROM '$CSV' CSV"
N=$(Q -c "SELECT count(*) FROM gh_src")
CUT=$(( N * 95 / 100 ))
echo "rows: $N  (first $CUT loaded now, the rest appended later)"
echo "== how out of order is the real stream? (rows whose timestamp is behind the running maximum)"
Q -c "SELECT count(*) FILTER (WHERE created_at < m) || ' rows behind, ' ||
             round(100.0 * count(*) FILTER (WHERE created_at < m) / count(*), 3) || '%  (max lag ' ||
             max(m - created_at) || ')'
      FROM (SELECT created_at, max(created_at) OVER (ORDER BY seq ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING) m FROM gh_src) s"
Q -c "SELECT 'distinct seconds: ' || count(DISTINCT created_at) || ', events per second: ' || round(count(*)::numeric / count(DISTINCT created_at), 1) FROM gh_src"

for kind in $KINDS; do
  Q -c "CREATE TABLE gh_$kind AS SELECT id, created_at, type, repo, actor FROM gh_src WHERE seq <= $CUT ORDER BY seq" -c "VACUUM (ANALYZE) gh_$kind"
done
Q -c "CREATE TABLE gh_sample AS SELECT created_at FROM gh_btree WHERE random() < 2000.0 / $CUT"
Q -c "CREATE TABLE gh_ranges AS SELECT created_at AS lo, created_at + interval '5 minutes' AS hi FROM gh_sample LIMIT 200"
echo "table: $(Q -c "SELECT pg_size_pretty(pg_relation_size('gh_btree'))")"

echo "== index build (seconds, one core) and size"
for kind in $KINDS; do
  t0=$(date +%s.%N)
  Q -c "SET max_parallel_maintenance_workers = 0; SET maintenance_work_mem = '2GB'; $(index_sql $kind)"
  t1=$(date +%s.%N)
  echo "$kind: $(echo "$t1 - $t0" | bc) s, $(Q -c "SELECT pg_size_pretty(pg_relation_size('gh_${kind}_idx'))")"
done
Q -c "ANALYZE" -c "CHECKPOINT"
Q -c "SELECT 'heapmap model: ' || segments || ' segments, window avg ' || round(avg_window::numeric, 2) || ' max ' || max_window || ', ' || exception_entries || ' exceptions, tail ' || tail_pages || ' pages' FROM heapmap_stats('gh_heapmap_idx')"

for kind in $KINDS; do
  Q <<SQL
CREATE OR REPLACE FUNCTION gh_lookup_$kind(lim int) RETURNS float8 AS \$\$
DECLARE keys timestamptz[]; key_val timestamptz; v int; t0 timestamptz; n int;
BEGIN
    SELECT array_agg(created_at) INTO keys FROM (SELECT created_at FROM gh_sample LIMIT lim) s;
    n := array_length(keys, 1);
    t0 := clock_timestamp();
    FOREACH key_val IN ARRAY keys LOOP
        SELECT count(*) INTO v FROM gh_$kind WHERE created_at = key_val;
    END LOOP;
    RETURN extract(epoch FROM clock_timestamp() - t0) * 1e6 / n;
END \$\$ LANGUAGE plpgsql;
CREATE OR REPLACE FUNCTION gh_range_$kind() RETURNS float8 AS \$\$
DECLARE r record; v bigint; t0 timestamptz; n int := 0;
BEGIN
    t0 := clock_timestamp();
    FOR r IN SELECT lo, hi FROM gh_ranges LOOP
        SELECT count(*) INTO v FROM gh_$kind WHERE created_at >= r.lo AND created_at < r.hi;
        n := n + 1;
    END LOOP;
    RETURN extract(epoch FROM clock_timestamp() - t0) * 1e6 / n;
END \$\$ LANGUAGE plpgsql;
SQL
done

echo "== pages touched by 'events at this second' (index + table), averaged over 300 timestamps"
for kind in $KINDS; do
  Q -c "SELECT pg_prewarm('gh_$kind'), pg_prewarm('gh_${kind}_idx')" >/dev/null
  Q <<SQL 2>&1 | sed 's/^NOTICE:  //'
DO \$\$
DECLARE key_val timestamptz; j json; tot float8 := 0; n int := 0; rows_ float8 := 0;
BEGIN
  FOR key_val IN SELECT created_at FROM gh_sample LIMIT 300 LOOP
    EXECUTE format('EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) SELECT count(*) FROM gh_$kind WHERE created_at = %L', key_val) INTO j;
    tot := tot + (j->0->'Plan'->>'Shared Hit Blocks')::float8 + (j->0->'Plan'->>'Shared Read Blocks')::float8;
    rows_ := rows_ + (j->0->'Plan'->'Plans'->0->>'Actual Rows')::float8;
    n := n + 1;
  END LOOP;
  RAISE NOTICE '$kind: % pages per lookup, % rows returned on average', round((tot / n)::numeric, 1), round((rows_ / n)::numeric, 1);
END \$\$;
SQL
done

echo "== warm lookup time in the server (microseconds, median of 5 alternating rounds)"
declare -A LK RG
for r in 1 2 3 4 5; do
  order="$KINDS"; [ $((r % 2)) -eq 0 ] && order="heapmap brin btree"
  for kind in $order; do
    Q -c "SELECT pg_prewarm('gh_$kind'), pg_prewarm('gh_${kind}_idx')" >/dev/null
    n=2000; [ "$kind" = brin ] && n=300
    Q -c "SELECT gh_lookup_$kind($n)" >/dev/null
    LK[$kind]="${LK[$kind]:-} $(Q -c "SELECT round(gh_lookup_$kind($n)::numeric, 1)")"
    RG[$kind]="${RG[$kind]:-} $(Q -c "SELECT round(gh_range_$kind()::numeric, 1)")"
  done
done
for kind in $KINDS; do echo "$kind: $(echo ${LK[$kind]} | tr ' ' '\n' | sort -n | sed -n 3p)"; done
echo "== 'everything in a 5 minute window' (about $(Q -c "SELECT round(count(*)::numeric / count(DISTINCT created_at) * 300) FROM gh_src") rows; microseconds, median of 5)"
for kind in $KINDS; do echo "$kind: $(echo ${RG[$kind]} | tr ' ' '\n' | sort -n | sed -n 3p)"; done

echo "== appending the remaining $(( N - CUT )) events in arrival order (seconds, WAL, index size after)"
for kind in $KINDS; do
  Q -c "CHECKPOINT"
  l0=$(Q -c "SELECT pg_current_wal_lsn()")
  t0=$(date +%s.%N)
  Q -c "INSERT INTO gh_$kind SELECT id, created_at, type, repo, actor FROM gh_src WHERE seq > $CUT ORDER BY seq"
  t1=$(date +%s.%N)
  wal=$(Q -c "SELECT pg_size_pretty(pg_wal_lsn_diff(pg_current_wal_lsn(), '$l0'))")
  echo "$kind: $(echo "$t1 - $t0" | bc) s, WAL $wal, index now $(Q -c "SELECT pg_size_pretty(pg_relation_size('gh_${kind}_idx'))")"
done
Q -c "SELECT 'heapmap after append: ' || segments || ' segments, window avg ' || round(avg_window::numeric, 2) || ', ' || exception_entries || ' exceptions, ' || folds || ' folds' FROM heapmap_stats('gh_heapmap_idx')"
echo "== heapmap agrees with a sequential scan"
Q -f "$HERE/../test/checks.sql"
Q -c "CREATE OR REPLACE VIEW gh_check AS SELECT ctid, created_at AS k FROM gh_heapmap" -c "SELECT hm_check('gh_check', 10)"
