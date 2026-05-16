#!/usr/bin/env bash
# kublet.sh — start/stop/status the fleet webui on port 1666 (and clean up
# any deploy subprocess that's hanging on it).
#
# Usage:
#   ./kublet.sh start     boot the Flask server in the background
#   ./kublet.sh stop      stop the server and any pio/esptool child it spawned
#   ./kublet.sh restart   stop + start
#   ./kublet.sh status    show PID, uptime, any active deploy children, log tail
#   ./kublet.sh logs      tail -f the webui log

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WEBUI_DIR="$SCRIPT_DIR/webui"
VENV_PY="$SCRIPT_DIR/kublet-apps/kublet_env/bin/python"
PORT="${KUBLET_WEBUI_PORT:-1666}"
PID_FILE="/tmp/kublet-webui.pid"
LOG_FILE="/tmp/kublet-webui.log"

pid_listening() {
  # Most reliable way to find the server: who is bound to :PORT?
  lsof -t -i ":$PORT" -sTCP:LISTEN 2>/dev/null | head -n1 || true
}

start() {
  local existing
  existing="$(pid_listening)"
  if [ -n "$existing" ]; then
    echo "Already running on port $PORT (PID $existing)."
    return 0
  fi
  if [ ! -x "$VENV_PY" ]; then
    echo "Error: venv python missing at $VENV_PY" >&2
    echo "       Run install.sh first." >&2
    return 1
  fi
  echo "Starting webui on port $PORT..."
  ( cd "$WEBUI_DIR" && nohup "$VENV_PY" app.py >"$LOG_FILE" 2>&1 & echo $! >"$PID_FILE" )
  # Wait up to 5s for the listener to actually come up
  for _ in 1 2 3 4 5; do
    sleep 1
    local p
    p="$(pid_listening)"
    if [ -n "$p" ]; then
      echo "Started: PID $p on http://localhost:$PORT"
      echo "Log:     $LOG_FILE"
      return 0
    fi
  done
  echo "Process started but nothing listening on $PORT yet." >&2
  echo "Last few log lines:" >&2
  tail -n 20 "$LOG_FILE" 2>/dev/null >&2 || true
  return 1
}

stop() {
  local pid
  pid="$(pid_listening)"
  if [ -z "$pid" ]; then
    echo "Not running."
    rm -f "$PID_FILE"
    return 0
  fi
  echo "Stopping webui (PID $pid)..."
  # Kill any deploy subprocess first so the server doesn't reap them
  # mid-shutdown and report errors. These are children of the Flask
  # process when an OTA/USB deploy is in flight.
  pkill -TERM -P "$pid" 2>/dev/null || true
  kill -TERM "$pid" 2>/dev/null || true
  for _ in 1 2 3 4 5; do
    sleep 1
    if ! kill -0 "$pid" 2>/dev/null; then
      echo "Stopped."
      rm -f "$PID_FILE"
      # Sweep stragglers from earlier hung deploys.
      pkill -f "tools/dev deploy" 2>/dev/null || true
      pkill -f "platformio.*run"  2>/dev/null || true
      pkill -f "esptool.*write_flash" 2>/dev/null || true
      return 0
    fi
  done
  echo "Did not exit on TERM — force killing." >&2
  kill -KILL "$pid" 2>/dev/null || true
  pkill -KILL -P "$pid" 2>/dev/null || true
  rm -f "$PID_FILE"
}

status() {
  local pid children uptime
  pid="$(pid_listening)"
  if [ -z "$pid" ]; then
    echo "Status   : not running"
    [ -f "$LOG_FILE" ] && { echo "Last log :"; tail -n 5 "$LOG_FILE"; }
    return 0
  fi
  uptime="$(ps -o etime= -p "$pid" 2>/dev/null | tr -d ' ' || true)"
  echo "Status   : running"
  echo "PID      : $pid"
  echo "Port     : $PORT"
  echo "URL      : http://localhost:$PORT"
  echo "Uptime   : ${uptime:-unknown}"
  echo "Log      : $LOG_FILE"
  # pgrep exits 1 when there are no children — that's expected, so we
  # have to defuse it explicitly under `set -euo pipefail`.
  children="$(pgrep -P "$pid" 2>/dev/null | tr '\n' ' ' || true)"
  if [ -n "${children// }" ]; then
    echo
    echo "Active subprocess(es) — possibly a deploy in flight:"
    ps -p $children -o pid,etime,command 2>/dev/null || true
  fi
  echo
  echo "Last 12 log lines:"
  tail -n 12 "$LOG_FILE" 2>/dev/null || echo "(no log)"
}

case "${1:-status}" in
  start)   start ;;
  stop)    stop ;;
  restart) stop; start ;;
  status)  status ;;
  logs)    tail -f "$LOG_FILE" ;;
  *)
    echo "Usage: $0 {start|stop|restart|status|logs}" >&2
    exit 2
    ;;
esac
