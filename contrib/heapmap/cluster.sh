#!/usr/bin/env bash
# A private PostgreSQL 18 cluster for the heapmap index. It lives outside the
# repo, needs no sudo, and leaves any system cluster alone.
#
#   ./cluster.sh init      create the cluster and start it
#   ./cluster.sh install   build the extension and install it for this cluster
#   ./cluster.sh start | stop | restart | crash | psql [args] | env
#
# Uses the system PostgreSQL 18 by default. To use another build:
#   HM_PG_BIN=/path/to/pg18/bin ./cluster.sh init
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${HM_CLUSTER_ROOT:-$HERE/../../../pg_heapmap_cluster}"
mkdir -p "$ROOT"
ROOT="$(cd "$ROOT" && pwd)"
DATA="$ROOT/data"
SOCK="$ROOT/sock"
EXT="$ROOT/ext"
LOG="$ROOT/server.log"
PORT="${HM_PORT:-5545}"
BIN="${HM_PG_BIN:-/usr/lib/postgresql/18/bin}"
PG_CONFIG="$BIN/pg_config"

[ -x "$BIN/postgres" ] || { echo "no PostgreSQL at $BIN (set HM_PG_BIN)" >&2; exit 1; }
case "$("$BIN/postgres" --version)" in
  *" 18."*) ;;
  *) echo "$BIN is not PostgreSQL 18" >&2; exit 1 ;;
esac

run_pg_ctl() { "$BIN/pg_ctl" -D "$DATA" -l "$LOG" -w "$@"; }

case "${1:-}" in
  init)
    mkdir -p "$SOCK"
    "$BIN/initdb" -D "$DATA" -U postgres -A trust --no-sync >/dev/null
    # The extension is installed under $EXT, so no write access to the
    # PostgreSQL installation is needed.
    cat >> "$DATA/postgresql.conf" <<CONF
port = $PORT
listen_addresses = ''
unix_socket_directories = '$SOCK'
shared_buffers = 1GB
max_wal_size = 8GB
checkpoint_timeout = 30min
dynamic_library_path = '$EXT$("$PG_CONFIG" --pkglibdir):\$libdir'
extension_control_path = '$EXT$("$PG_CONFIG" --sharedir):\$system'
CONF
    run_pg_ctl start >/dev/null
    "$BIN/createdb" -h "$SOCK" -p "$PORT" -U postgres hm_test
    echo "cluster ready at $DATA, port $PORT"
    ;;
  install)
    make -C "$HERE" -s USE_PGXS=1 PG_CONFIG="$PG_CONFIG"
    make -C "$HERE" -s USE_PGXS=1 PG_CONFIG="$PG_CONFIG" install DESTDIR="$EXT"
    ;;
  start) run_pg_ctl start >/dev/null ;;
  stop) run_pg_ctl stop -m fast >/dev/null ;;
  restart) run_pg_ctl restart -m fast >/dev/null ;;
  crash) run_pg_ctl stop -m immediate >/dev/null ;;
  psql) shift; exec "$BIN/psql" -h "$SOCK" -p "$PORT" -U postgres -X -d "${HM_DB:-hm_test}" "$@" ;;
  env) echo "PGHOST=$SOCK PGPORT=$PORT PGUSER=postgres PGDATABASE=${HM_DB:-hm_test} DATA=$DATA BIN=$BIN" ;;
  *) echo "usage: $0 init|install|start|stop|restart|crash|psql|env" >&2; exit 2 ;;
esac
