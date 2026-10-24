#!/usr/bin/env bash
# ==============================================================================
# bridge_vms.sh — Manage socat serial bridge between Parallels VMs
# ==============================================================================

set -euo pipefail

KD_SOCK="/tmp/kd.sock"
DBG_SOCK="/tmp/debugger.sock"
LOG_FILE="/tmp/socat.log"
SUDO_PASS="${SUDO_PASS:-9090}"

# Resolve socat binary path
if command -v socat >/dev/null 2>&1; then
    SOCAT_BIN="$(command -v socat)"
elif [ -x "/opt/homebrew/bin/socat" ]; then
    SOCAT_BIN="/opt/homebrew/bin/socat"
elif [ -x "/usr/local/bin/socat" ]; then
    SOCAT_BIN="/usr/local/bin/socat"
else
    echo "[-] Error: 'socat' not found. Install it with: brew install socat" >&2
    exit 1
fi

run_sudo() {
    echo "$SUDO_PASS" | sudo -S "$@"
}

get_socat_pid() {
    run_sudo ps aux 2>/dev/null | grep -E "socat.*UNIX-CONNECT.*sock" | grep -v grep | awk '{print $2}' || true
}

check_vms() {
    if command -v prlctl >/dev/null 2>&1; then
        echo "[*] Parallels VMs status:"
        prlctl list --all | grep -E "(Target|Debugger)" || true
    fi
}

start_bridge() {
    local pid
    pid="$(get_socat_pid)"
    if [ -n "$pid" ]; then
        echo "[!] socat is already running (PID: $pid)."
        show_status
        return 0
    fi

    # Check sockets
    if [ ! -S "$KD_SOCK" ] || [ ! -S "$DBG_SOCK" ]; then
        echo "[-] Sockets missing:"
        [ -S "$KD_SOCK" ] || echo "    Missing: $KD_SOCK"
        [ -S "$DBG_SOCK" ] || echo "    Missing: $DBG_SOCK"
        echo "[!] Make sure Target and Debugger VMs are running in Parallels."
        check_vms
        exit 1
    fi

    echo "[*] Starting socat bridge..."
    run_sudo sh -c "nohup '$SOCAT_BIN' -d -d UNIX-CONNECT:'$KD_SOCK' UNIX-CONNECT:'$DBG_SOCK' > '$LOG_FILE' 2>&1 &"
    sleep 1

    pid="$(get_socat_pid)"
    if [ -n "$pid" ]; then
        echo "[+] Bridge successfully started (PID: $pid)."
        echo "[*] Connected: $KD_SOCK <---> $DBG_SOCK"
        echo "[*] Logs: $LOG_FILE"
    else
        echo "[-] Failed to start socat. Check log:"
        cat "$LOG_FILE" 2>/dev/null || true
        exit 1
    fi
}

stop_bridge() {
    local pid
    pid="$(get_socat_pid)"
    if [ -z "$pid" ]; then
        echo "[*] socat is not running."
        return 0
    fi

    echo "[*] Stopping socat bridge (PID: $pid)..."
    run_sudo pkill -9 -f "socat.*sock" 2>/dev/null || true
    echo "[+] Stopped."
}

show_status() {
    local pid
    pid="$(get_socat_pid)"
    echo "============================================================"
    echo " socat VM Serial Bridge Status"
    echo "============================================================"
    if [ -n "$pid" ]; then
        echo " [STATUS] RUNNING (PID: $pid)"
    else
        echo " [STATUS] STOPPED"
    fi

    echo ""
    echo " [SOCKETS]"
    ls -l "$KD_SOCK" "$DBG_SOCK" 2>/dev/null || echo " Sockets not found in /tmp"

    if [ -f "$LOG_FILE" ]; then
        echo ""
        echo " [LAST LOG ENTRIES: $LOG_FILE]"
        tail -n 6 "$LOG_FILE"
    fi
    echo "============================================================"
}

show_logs() {
    if [ -f "$LOG_FILE" ]; then
        run_sudo tail -f "$LOG_FILE"
    else
        echo "[-] No log file found at $LOG_FILE"
    fi
}

case "${1:-start}" in
    start)
        start_bridge
        ;;
    stop)
        stop_bridge
        ;;
    restart)
        stop_bridge
        sleep 1
        start_bridge
        ;;
    status)
        show_status
        ;;
    log|logs)
        show_logs
        ;;
    *)
        echo "Usage: $0 {start|stop|restart|status|logs}"
        exit 1
        ;;
esac
