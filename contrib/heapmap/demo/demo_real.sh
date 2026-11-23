#!/usr/bin/env bash
# Second act of the demo: one day of real GitHub events (GH Archive), indexed
# on created_at. Needs the gh_btree, gh_brin and gh_heapmap tables that
# eval/gharchive.sh creates (they stay in the database).
#   demo/demo_real.sh          AUTO=1 to skip the pauses
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
eval "export $("$HERE/../cluster.sh" env)"
PSQL=("$BIN/psql" -X -q -v ON_ERROR_STOP=1 --pset=pager=off)
bold() { printf '\033[1m%s\033[0m\n' "$*"; }
step() { echo; bold "== $*"; }
pause() { [ -n "${AUTO:-}" ] || { printf '   [Enter] '; read -r _; }; }
show() { printf '   | %s\n' "$@"; "${PSQL[@]}" -c "$*"; }

"${PSQL[@]}" -tA -c "SELECT 1 FROM pg_class WHERE relname = 'gh_heapmap'" | grep -q 1 \
  || { echo "no gh_* tables; run eval/gharchive.sh first" >&2; exit 1; }

bold "One day of real GitHub events (GH Archive, 1 January 2024)"
echo "   Every public event, in the order it was recorded, indexed on created_at."
show "SELECT count(*) AS events, min(created_at) AS first, max(created_at) AS last, pg_size_pretty(pg_relation_size('gh_heapmap')) AS table_size FROM gh_heapmap;"
pause

step "1. How out of order is a real stream?"
echo "   Events whose timestamp is behind the event stored just before them:"
show "SELECT count(*) FILTER (WHERE created_at < prev) AS behind, round(100.0 * count(*) FILTER (WHERE created_at < prev) / count(*), 3) AS percent, percentile_cont(0.5) WITHIN GROUP (ORDER BY prev - created_at) FILTER (WHERE created_at < prev) AS median_lag FROM (SELECT created_at, lag(created_at) OVER (ORDER BY ctid) prev FROM gh_heapmap) s;"
pause

step "2. Index sizes"
show "SELECT 'btree' AS index, pg_size_pretty(pg_relation_size('gh_btree_idx')) AS size UNION ALL SELECT 'brin', pg_size_pretty(pg_relation_size('gh_brin_idx')) UNION ALL SELECT 'heapmap', pg_size_pretty(pg_relation_size('gh_heapmap_idx'));"
pause

step "3. Everything that happened in one second"
TS=$("${PSQL[@]}" -tA -c "SELECT created_at FROM gh_heapmap TABLESAMPLE SYSTEM (0.01) LIMIT 1")
for t in btree brin heapmap; do
  show "EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT count(*) FROM gh_$t WHERE created_at = '$TS';"
done
pause

step "4. Five minutes of events, by type"
for t in btree heapmap; do
  show "EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, SUMMARY OFF) SELECT type, count(*) FROM gh_$t WHERE created_at >= '$TS' AND created_at < timestamptz '$TS' + interval '5 minutes' GROUP BY type ORDER BY 2 DESC LIMIT 5;"
done
pause

step "5. What the heapmap index holds for this table"
show "SELECT segments, round(avg_window::numeric, 1) AS pages_per_lookup, tail_pages, exception_entries, pg_size_pretty(index_bytes) AS size FROM heapmap_stats('gh_heapmap_idx');"
echo "   The exception entries are the out-of-order events from step 1, stored exactly."
echo "   Everything else is answered by the model: a few hundred line segments."
