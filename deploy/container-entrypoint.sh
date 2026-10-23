#!/bin/sh

set -u

child_pid=""

stop_child() {
    signal="${1:-TERM}"
    if [ -n "$child_pid" ] && kill -0 "$child_pid" 2>/dev/null; then
        kill "-$signal" "$child_pid" 2>/dev/null || true
    fi
}

forward_stop() {
    stop_child TERM
}

trap forward_stop TERM INT

node src/server.mjs &
child_pid=$!

# Let SQLite migrations and the HTTP listener finish before health probing.
sleep 10
failures=0

while kill -0 "$child_pid" 2>/dev/null; do
    if wget -q -T 5 -O /dev/null http://127.0.0.1:8787/healthz; then
        failures=0
    else
        failures=$((failures + 1))
        if [ "$failures" -ge 3 ]; then
            echo "cloud health check failed three times; restarting process" >&2
            stop_child TERM
            waited=0
            while kill -0 "$child_pid" 2>/dev/null && [ "$waited" -lt 5 ]; do
                sleep 1
                waited=$((waited + 1))
            done
            stop_child KILL
            wait "$child_pid" 2>/dev/null || true
            exit 1
        fi
    fi
    sleep 15
done

wait "$child_pid"
exit $?
