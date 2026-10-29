#!/bin/bash
# =============================================================================
# ready-lib.sh — readiness handshake between the core node and the cell node
#
# The core publishes small marker files over plain HTTP on the experiment LAN
# (cn-ready, ric-ready, failed). The cell polls them instead of guessing, from
# elapsed time, how long the core build will take. Source this file; it defines
# functions only.
# =============================================================================

# ready_server_start <dir> <bind_ip> <port> <log>
# Serve <dir> read-only on <bind_ip>:<port>. Replaces a previous instance.
ready_server_start() {
    local dir="$1" ip="$2" port="$3" log="$4"
    mkdir -p "$dir"
    pkill -f "http.server ${port} --bind ${ip}" 2>/dev/null
    nohup python3 -m http.server "$port" --bind "$ip" --directory "$dir" \
        > "$log" 2>&1 &
    sleep 1
    pgrep -f "http.server ${port} --bind ${ip}" > /dev/null
}

# ready_marker_present <url>
ready_marker_present() {
    curl -fs --max-time 3 -o /dev/null "$1" 2>/dev/null
}

# wait_for_ready <ready_url> <failed_url> <timeout_s> [interval_s]
# Returns 0 once ready_url exists, 2 if failed_url appears first, 1 on timeout.
wait_for_ready() {
    local ready_url="$1" failed_url="$2" timeout="$3" interval="${4:-10}"
    local elapsed=0
    while true; do
        if ready_marker_present "$ready_url"; then
            return 0
        fi
        if ready_marker_present "$failed_url"; then
            return 2
        fi
        if [ "$elapsed" -ge "$timeout" ]; then
            return 1
        fi
        echo "waiting for ${ready_url} (${elapsed}s)"
        sleep "$interval"
        elapsed=$((elapsed + interval))
    done
}
