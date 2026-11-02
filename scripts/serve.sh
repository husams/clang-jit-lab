#!/usr/bin/env bash
# Build the HTML edition and serve site/ in the background.
#
# Usage: scripts/serve.sh [start] HOST:PORT   rebuild site/ and serve it on HOST:PORT
#        scripts/serve.sh stop HOST:PORT      stop the server on HOST:PORT
#        scripts/serve.sh status HOST:PORT    report whether it is running
#
# HOST is a host name or an IP address; IPv6 addresses go in brackets:
#   scripts/serve.sh 127.0.0.1:8000
#   scripts/serve.sh localhost:8000
#   scripts/serve.sh 0.0.0.0:8000          (all IPv4 interfaces)
#   scripts/serve.sh [::1]:8000
#
# The server is `python3 -m http.server` (stdlib only); its pid and log are in
# out/serve-<host>-<port>.{pid,log}.
set -euo pipefail

cd "$(dirname "$0")/.."

usage() {
  echo "usage: $0 [start|stop|status] HOST:PORT   (e.g. 127.0.0.1:8000, localhost:8000, [::1]:8000)" >&2
  exit 2
}

case "${1:-}" in
  start | stop | status) cmd=$1; shift ;;
  *) cmd=start ;;
esac
[[ $# -eq 1 ]] || usage
addr=$1

# Split HOST:PORT; an IPv6 host is written [addr]:port.
if [[ "$addr" =~ ^\[([0-9A-Fa-f:.]+)\]:([0-9]+)$ ]]; then
  host=${BASH_REMATCH[1]}
  port=${BASH_REMATCH[2]}
  url_host="[$host]"
elif [[ "$addr" =~ ^([A-Za-z0-9._-]+):([0-9]+)$ ]]; then
  host=${BASH_REMATCH[1]}
  port=${BASH_REMATCH[2]}
  url_host=$host
else
  usage
fi
(( 10#$port > 0 && 10#$port < 65536 )) || usage
port=$((10#$port))
shown="$url_host:$port"

# The URL to print: a wildcard bind is reachable through localhost.
case "$host" in
  0.0.0.0) url="http://localhost:$port/ (all IPv4 interfaces)" ;;
  ::) url="http://localhost:$port/ (all interfaces)" ;;
  *) url="http://$url_host:$port/" ;;
esac

mkdir -p out
tag=$(printf '%s-%s' "$host" "$port" | tr ':/' '__')
pidfile="out/serve-$tag.pid"
logfile="out/serve-$tag.log"

running() { [[ -f "$pidfile" ]] && kill -0 "$(cat "$pidfile")" 2>/dev/null; }
listening() { lsof -nP -a -p "$(cat "$pidfile")" -iTCP:"$port" -sTCP:LISTEN >/dev/null 2>&1; }

case "$cmd" in
  start)
    if running; then
      echo "already running: $url (pid $(cat "$pidfile"))"
      exit 0
    fi
    rm -f "$pidfile"
    if lsof -nP -iTCP:"$port" -sTCP:LISTEN >/dev/null 2>&1; then
      echo "port $port is already in use:" >&2
      lsof -nP -iTCP:"$port" -sTCP:LISTEN >&2 || true
      if lsof -nP -iTCP:"$port" -sTCP:LISTEN 2>/dev/null | grep -q '^ControlCe'; then
        echo "(that is macOS AirPlay Receiver, which owns ports 5000 and 7000; pick another port, e.g. 8000)" >&2
      fi
      exit 1
    fi
    python3 scripts/build_site.py
    nohup python3 -m http.server "$port" --bind "$host" --directory site \
      >"$logfile" 2>&1 </dev/null &
    echo $! >"$pidfile"
    # Wait up to 5 s for the socket; a bad host name or address makes the server exit.
    for _ in $(seq 1 50); do
      if ! running; then
        rm -f "$pidfile"
        echo "server failed to start on $shown, see $logfile:" >&2
        tail -3 "$logfile" >&2
        exit 1
      fi
      listening && break
      sleep 0.1
    done
    if ! listening; then
      kill "$(cat "$pidfile")" 2>/dev/null || true
      rm -f "$pidfile"
      echo "server did not start listening on $shown, see $logfile" >&2
      exit 1
    fi
    echo "serving $url (pid $(cat "$pidfile"), log $logfile)"
    ;;
  stop)
    if running; then
      kill "$(cat "$pidfile")"
      echo "stopped server on $shown"
    else
      echo "no server running on $shown"
    fi
    rm -f "$pidfile"
    ;;
  status)
    if running; then
      echo "running: $url (pid $(cat "$pidfile"), log $logfile)"
    else
      rm -f "$pidfile"
      echo "no server running on $shown"
      exit 1
    fi
    ;;
esac
