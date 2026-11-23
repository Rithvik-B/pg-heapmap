#!/usr/bin/env bash
# A live walk-through: the same event table under a B-tree, a BRIN index and a
# heapmap index, side by side, on a running PostgreSQL 18.
#
#   demo/demo.sh [rows] [clients]     default 5,000,000 rows, 8 inserting sessions
#   AUTO=1 demo/demo.sh               do not wait for Enter between steps
#   HM_SYSTEM=1 demo/demo.sh          use the PostgreSQL that psql connects to
#                                     (PGHOST, PGUSER, PGPASSWORD, PGDATABASE),
#                                     not the private test cluster. Needs the
#                                     extension installed there:
#                                     make USE_PGXS=1 && sudo make USE_PGXS=1 install
#
# Every number on screen is measured while you watch. Nothing is precomputed.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROWS="${1:-5000000}"
CLIENTS="${2:-8}"
SECS="${DEMO_SECS:-12}"
KINDS="btree brin heapmap"

if [ -n "${HM_SYSTEM:-}" ]; then
  PSQL=(psql)
  PGBENCH=(pgbench)
else
  eval "export $("$HERE/../cluster.sh" env)"
  PSQL=("$BIN/psql")
  PGBENCH=("$BIN/pgbench")
fi
PSQL+=(-X -q -v ON_ERROR_STOP=1 --pset=pager=off)

bold() { printf '\033[1m%s\033[0m\n' "$*"; }
dim() { printf '\033[2m%s\033[0m\n' "$*"; }
step() { echo; bold "== $*"; }
note() { echo "   $*"; }
pause() { [ -n "${AUTO:-}" ] || { printf '\033[2m   (Enter to continue)\033[0m'; read -r _; }; }
# show a statement, then run it
show() { dim "$(echo "$1" | sed 's/^/   | /')"; "${PSQL[@]}" -c "$1"; }
# run quietly and return one value
val() { "${PSQL[@]}" -At -c "$1"; }
quiet() { "${PSQL[@]}" -At "$@" >/dev/null; }
# seconds a statement takes, measured in the server
timed() { val "DO \$\$ DECLARE t0 timestamptz := clock_timestamp(); BEGIN $1; RAISE NOTICE '%', round(extract(epoch FROM clock_timestamp() - t0)::numeric, 2); END \$\$" 2>&1 | sed 's/^NOTICE:  //'; }
size_of() { val "SELECT pg_size_pretty(pg_relation_size('$1'))"; }

declare -A BUILD SIZE0 SIZE1 SIZE2 PAGES MICROS RANGE LOAD WAL BASE
row() { printf '   %-34s %14s %14s %14s\n' "$@"; }
header() { row "" "B-tree" "BRIN" "heapmap"; row "" "------" "----" "-------"; }

index_sql() {
  case "$1" in
    btree)   echo "CREATE INDEX ev_btree_ts ON ev_btree USING btree (ts)" ;;
    brin)    echo "CREATE INDEX ev_brin_ts ON ev_brin USING brin (ts)" ;;
    heapmap) echo "CREATE INDEX ev_heapmap_ts ON ev_heapmap USING heapmap (ts)" ;;
  esac
}

"${PSQL[@]}" -At -c "SELECT 1" >/dev/null || {
  echo "Cannot connect to PostgreSQL. With HM_SYSTEM=1, set PGHOST, PGUSER, PGPASSWORD and PGDATABASE" >&2
  echo "to what you use in pgAdmin. Without it, start the test cluster: ./cluster.sh start" >&2
  exit 1
}
"${PSQL[@]}" -At -c "SELECT 1 FROM pg_available_extensions WHERE name = 'heapmap'" | grep -q 1 || {
  echo "The heapmap extension is not installed in this PostgreSQL." >&2
  echo "Private test cluster: ./cluster.sh init && ./cluster.sh install" >&2
  echo "System PostgreSQL:    make USE_PGXS=1 && sudo make USE_PGXS=1 install" >&2
  exit 1
}

bold "heapmap: a learned block-range index for PostgreSQL"
note "$(val "SELECT version()" | cut -d, -f1), database $(val "SELECT current_database()")"
note "An event log of $ROWS rows. Timestamps only move forward, as in any log."
note "Three copies of the table, identical row for row, one index each."

# ---------------------------------------------------------------------------
step "1. Load the table"
quiet -c "CREATE EXTENSION IF NOT EXISTS heapmap" \
      -c "DROP TABLE IF EXISTS ev_src, ev_btree, ev_brin, ev_heapmap, ev_more, ev_keys"
show "CREATE UNLOGGED TABLE ev_src AS
SELECT timestamptz '2026-01-01' + sum(1 + (random() * 2000)::int) OVER (ORDER BY i) * interval '1 millisecond' AS ts,
       (random() * 1000)::int AS device, random() AS reading
FROM generate_series(1, $ROWS) i;"
for k in $KINDS; do quiet -c "CREATE TABLE ev_$k AS SELECT * FROM ev_src"; done
# the next 20% of the log, loaded later, and a sample of timestamps to look up
quiet -c "CREATE UNLOGGED TABLE ev_more AS
          SELECT (SELECT max(ts) FROM ev_src) + sum(1 + (random() * 2000)::int) OVER (ORDER BY i) * interval '1 millisecond' AS ts,
                 (random() * 1000)::int AS device, random() AS reading
          FROM generate_series(1, $ROWS / 5) i" \
      -c "CREATE TABLE ev_keys AS SELECT ts FROM ev_src WHERE random() < 3000.0 / $ROWS"
show "SELECT count(*) AS rows, min(ts) AS first_event, max(ts) AS last_event,
       pg_size_pretty(pg_relation_size('ev_heapmap')) AS table_size FROM ev_heapmap;"
pause

# ---------------------------------------------------------------------------
step "2. Build one index on each copy"
for k in $KINDS; do
  dim "   | $(index_sql "$k");"
  BUILD[$k]="$(timed "SET LOCAL max_parallel_maintenance_workers = 0; $(index_sql "$k")")"
  SIZE0[$k]="$(size_of "ev_${k}_ts")"
done
for k in $KINDS; do quiet -c "VACUUM (ANALYZE) ev_$k"; done
echo
header
row "build time, one core (s)" "${BUILD[btree]}" "${BUILD[brin]}" "${BUILD[heapmap]}"
row "index size" "${SIZE0[btree]}" "${SIZE0[brin]}" "${SIZE0[heapmap]}"
note ""
note "The B-tree stores every timestamp with a row pointer. BRIN stores a min and"
note "max per 128 table pages. heapmap stores a few line segments that say which"
note "table page each timestamp is on, and nothing per row."
pause

# ---------------------------------------------------------------------------
step "3. Find one event by its timestamp"
T="$(val "SELECT ts FROM ev_src ORDER BY ts OFFSET $ROWS / 2 LIMIT 1")"
for k in $KINDS; do
  show "EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT * FROM ev_$k WHERE ts = '$T';"
done
note "\"Buffers\" is the number of 8 kB pages the query touched, index and table together."
pause

step "   The same, averaged over many timestamps"
for k in $KINDS; do
  quiet <<SQL
CREATE OR REPLACE FUNCTION ev_lookup_$k(n int, OUT micros float8, OUT pages float8) AS \$\$
DECLARE keys timestamptz[]; t timestamptz; v float8; t0 timestamptz; j json; tot float8 := 0; m int := 0;
BEGIN
    SELECT array_agg(ts) INTO keys FROM (SELECT ts FROM ev_keys LIMIT n) s;
    FOREACH t IN ARRAY keys LOOP          -- once to warm up
        SELECT reading INTO v FROM ev_$k WHERE ts = t;
    END LOOP;
    t0 := clock_timestamp();
    FOREACH t IN ARRAY keys LOOP
        SELECT reading INTO v FROM ev_$k WHERE ts = t;
    END LOOP;
    micros := extract(epoch FROM clock_timestamp() - t0) * 1e6 / array_length(keys, 1);
    FOREACH t IN ARRAY keys[1:100] LOOP
        EXECUTE format('EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) SELECT reading FROM ev_$k WHERE ts = %L', t) INTO j;
        tot := tot + (j->0->'Plan'->>'Shared Hit Blocks')::float8 + (j->0->'Plan'->>'Shared Read Blocks')::float8;
        m := m + 1;
    END LOOP;
    pages := tot / m;
END \$\$ LANGUAGE plpgsql;
CREATE OR REPLACE FUNCTION ev_range_$k(n int) RETURNS float8 AS \$\$
DECLARE t timestamptz; v float8; t0 timestamptz := clock_timestamp(); m int := 0;
BEGIN
    FOR t IN SELECT ts FROM ev_keys LIMIT n LOOP
        SELECT avg(reading) INTO v FROM ev_$k WHERE ts >= t AND ts < t + interval '1 hour';
        m := m + 1;
    END LOOP;
    RETURN extract(epoch FROM clock_timestamp() - t0) * 1e3 / m;
END \$\$ LANGUAGE plpgsql;
SQL
  n=2000; [ "$k" = brin ] && n=300
  IFS='|' read -r MICROS[$k] PAGES[$k] <<<"$(val "SELECT round(micros::numeric, 1), round(pages::numeric, 1) FROM ev_lookup_$k($n)")"
done
header
row "pages touched per lookup" "${PAGES[btree]}" "${PAGES[brin]}" "${PAGES[heapmap]}"
row "time per lookup, cached (µs)" "${MICROS[btree]}" "${MICROS[brin]}" "${MICROS[heapmap]}"
pause

# ---------------------------------------------------------------------------
step "4. One hour of events"
show "EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*), avg(reading) FROM ev_heapmap WHERE ts >= '$T' AND ts < timestamptz '$T' + interval '1 hour';"
for k in $KINDS; do RANGE[$k]="$(val "SELECT round(ev_range_$k(200)::numeric, 2)")"; done
header
row "one-hour query, cached (ms)" "${RANGE[btree]}" "${RANGE[brin]}" "${RANGE[heapmap]}"
pause

# ---------------------------------------------------------------------------
step "5. What is inside the heapmap index"
show "SELECT segments, round(avg_window::numeric, 1) AS pages_per_lookup, tail_pages, exception_entries,
       pg_size_pretty(index_bytes) AS size FROM heapmap_stats('ev_heapmap_ts');"
show "SELECT segment, to_timestamp(start_key / 1e6 + 946684800) AS from_time,
       round((pages_per_key * 1e6)::numeric, 4) AS pages_per_second, round(page_at_start::numeric, 1) AS page_at_start,
       window_from, window_to, is_open
FROM heapmap_segments('ev_heapmap_ts') ORDER BY segment LIMIT 6;"
note "Each segment is a straight line from timestamp to table page. The window is"
note "how far a row can be from the line. A lookup reads only the pages in it:"
show "SELECT first_page, last_page, scans_tail
FROM heapmap_window('ev_heapmap_ts', (extract(epoch FROM timestamptz '$T' - timestamptz '2000-01-01 00:00:00+00') * 1e6)::bigint);"
pause

# ---------------------------------------------------------------------------
step "6. Keep loading: $((ROWS / 5)) more rows into each table"
for k in $KINDS; do
  quiet -c "CHECKPOINT"
  l0="$(val "SELECT pg_current_wal_lsn()")"
  LOAD[$k]="$(timed "INSERT INTO ev_$k SELECT * FROM ev_more")"
  WAL[$k]="$(val "SELECT pg_size_pretty(pg_wal_lsn_diff(pg_current_wal_lsn(), '$l0'))")"
  SIZE1[$k]="$(size_of "ev_${k}_ts")"
done
header
row "load time (s)" "${LOAD[btree]}" "${LOAD[brin]}" "${LOAD[heapmap]}"
row "write-ahead log produced" "${WAL[btree]}" "${WAL[brin]}" "${WAL[heapmap]}"
row "index size now" "${SIZE1[btree]}" "${SIZE1[brin]}" "${SIZE1[heapmap]}"
note ""
note "A row that arrives in order costs heapmap nothing: the newest table pages"
note "are always searched, and every few pages the model is extended over them."
pause

# ---------------------------------------------------------------------------
step "7. Rows that do not fit the pattern"
note "1,000 events arrive late, with timestamps from all over the past:"
E0="$(val "SELECT exception_entries FROM heapmap_stats('ev_heapmap_ts')")"
show "INSERT INTO ev_heapmap
SELECT timestamptz '2026-01-01' + random() * (timestamptz '$T' - timestamptz '2026-01-01') + interval '0.5 microsecond', -1, 0
FROM generate_series(1, 1000);"
note "Then the log carries on as usual, and the pages holding the late rows join the model:"
show "INSERT INTO ev_heapmap
SELECT (SELECT max(ts) FROM ev_heapmap) + i * interval '1 second', 0, 0 FROM generate_series(1, 20000) i;"
E1="$(val "SELECT exception_entries FROM heapmap_stats('ev_heapmap_ts')")"
note "exception entries: $E0 before, $E1 after. Each late row is stored exactly, like a B-tree entry."
LATE="$(val "SELECT ts FROM ev_heapmap WHERE device = -1 LIMIT 1")"
show "SELECT ts, device FROM ev_heapmap WHERE ts = '$LATE';"
note "And a row whose timestamp is changed is still found under the new one:"
show "UPDATE ev_heapmap SET ts = ts + interval '3 days' WHERE ts = '$LATE';
SELECT ts, device FROM ev_heapmap WHERE ts = timestamptz '$LATE' + interval '3 days';"
pause

# ---------------------------------------------------------------------------
step "8. $CLIENTS sessions inserting at once, for $SECS seconds"
note "Every transaction adds the same event to all three tables. Growth of each index, live:"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cat > "$WORK/insert.sql" <<'SQL'
\set d random(1, 1000)
BEGIN;
INSERT INTO ev_btree VALUES (clock_timestamp(), :d, 0.5);
INSERT INTO ev_brin VALUES (clock_timestamp(), :d, 0.5);
INSERT INTO ev_heapmap VALUES (clock_timestamp(), :d, 0.5);
COMMIT;
SQL
N0="$(val "SELECT count(*) FROM ev_heapmap")"
START="$(val "SELECT clock_timestamp()")"
for k in $KINDS; do BASE[$k]="$(val "SELECT pg_relation_size('ev_${k}_ts')")"; done
grown() { val "SELECT '+' || pg_size_pretty(pg_relation_size('ev_$1_ts') - ${BASE[$1]})"; }
# commits are not flushed one by one here, so that the sessions spend their time inserting
PGOPTIONS="-c synchronous_commit=off" "${PGBENCH[@]}" -n -c "$CLIENTS" -j "$(( CLIENTS < 4 ? CLIENTS : 4 ))" -T "$SECS" \
  -f "$WORK/insert.sql" > "$WORK/pgbench.out" 2>&1 &
BENCH=$!
printf '   %6s %14s %14s %14s %14s\n' "second" "rows added" "B-tree" "BRIN" "heapmap"
for s in $(seq 2 2 "$SECS"); do
  sleep 2
  printf '   %6s %14s %14s %14s %14s\n' "$s" "$(val "SELECT count(*) FROM ev_heapmap WHERE ts >= '$START'")" \
    "$(grown btree)" "$(grown brin)" "$(grown heapmap)"
done
wait "$BENCH" || { cat "$WORK/pgbench.out"; exit 1; }
N1="$(val "SELECT count(*) FROM ev_heapmap")"
note "$((N1 - N0)) rows added to each table, $(grep -o 'tps = [0-9.]*' "$WORK/pgbench.out" | head -1 | cut -d' ' -f3 | cut -d. -f1) transactions per second."
for k in $KINDS; do SIZE2[$k]="$(size_of "ev_${k}_ts")"; done
note ""
note "Sessions inserting at the same time each fill a table page of their own, so"
note "neighbouring timestamps end up several pages apart. heapmap answers by reading"
note "a wider window for those timestamps, and stores exactly the few rows left over:"
show "SELECT segments, max_window AS widest_window, tail_pages, exception_entries,
       round(100.0 * exception_entries / $N1, 2) AS percent_of_rows, pg_size_pretty(index_bytes) AS size
FROM heapmap_stats('ev_heapmap_ts');"
pause

# ---------------------------------------------------------------------------
step "9. Is every answer right?"
note "40 random questions, answered once through the index and once by reading the"
note "whole table. The two must return exactly the same rows."
"${PSQL[@]}" <<'SQL'
DO $$
DECLARE t timestamptz; a text; b text; pred text; n int := 0; span interval;
BEGIN
    FOR t IN SELECT ts FROM (SELECT ts FROM ev_heapmap TABLESAMPLE SYSTEM (0.05)) s ORDER BY random() LIMIT 40 LOOP
        span := CASE n % 4 WHEN 0 THEN interval '0' WHEN 1 THEN interval '10 seconds'
                           WHEN 2 THEN interval '1 hour' ELSE interval '2 days' END;
        pred := format('ts >= %L AND ts <= %L', t, t + span);
        SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = on;
        EXECUTE 'SELECT count(*) || '':'' || coalesce(md5(string_agg(ctid::text, '','' ORDER BY ctid)), '''') FROM ev_heapmap WHERE ' || pred INTO a;
        SET LOCAL enable_seqscan = on; SET LOCAL enable_bitmapscan = off;
        EXECUTE 'SELECT count(*) || '':'' || coalesce(md5(string_agg(ctid::text, '','' ORDER BY ctid)), '''') FROM ev_heapmap WHERE ' || pred INTO b;
        IF a IS DISTINCT FROM b THEN
            RAISE EXCEPTION 'different answers for %: index % table %', pred, a, b;
        END IF;
        n := n + 1;
    END LOOP;
    RAISE NOTICE '% of % questions answered identically', n, n;
END $$;
SQL
pause

# ---------------------------------------------------------------------------
step "Summary"
header
row "index size at the start" "${SIZE0[btree]}" "${SIZE0[brin]}" "${SIZE0[heapmap]}"
row "index size at the end" "${SIZE2[btree]}" "${SIZE2[brin]}" "${SIZE2[heapmap]}"
row "build time (s)" "${BUILD[btree]}" "${BUILD[brin]}" "${BUILD[heapmap]}"
row "pages touched per lookup" "${PAGES[btree]}" "${PAGES[brin]}" "${PAGES[heapmap]}"
row "time per lookup, cached (µs)" "${MICROS[btree]}" "${MICROS[brin]}" "${MICROS[heapmap]}"
row "one-hour query, cached (ms)" "${RANGE[btree]}" "${RANGE[brin]}" "${RANGE[heapmap]}"
row "loading 20% more rows (s)" "${LOAD[btree]}" "${LOAD[brin]}" "${LOAD[heapmap]}"
row "write-ahead log for that load" "${WAL[btree]}" "${WAL[brin]}" "${WAL[heapmap]}"
echo
note "The tables stay in place (ev_btree, ev_brin, ev_heapmap), so you can go on"
note "querying them from psql or pgAdmin. To remove them:"
note "  DROP TABLE ev_src, ev_btree, ev_brin, ev_heapmap, ev_more, ev_keys;"
