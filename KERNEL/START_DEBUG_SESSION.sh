#!/bin/bash
# ============================================================
# WinDbg Kernel Debug Session Starter
# Two Parallels ARM64 VMs connected via socat Unix socket relay
# Fully automated — WinDbg launched before Target reboot
# so the boot break lands on first connect, no manual steps.
#
# Run as normal user (NOT sudo) — prlctl is per-user.
# socat is invoked with sudo internally where needed.
# ============================================================

set -euo pipefail

PRLCTL=/usr/local/bin/prlctl
SOCAT=/opt/homebrew/bin/socat

TARGET="Windows 11 Pro (Target)"
DEBUGGER="Windows 11 Pro (Debugger)"
KD_SOCK="/tmp/kd.sock"
DBG_SOCK="/tmp/debugger.sock"
WINDBG='C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\windbg.exe'
WINDBG_ARGS="-k com:port=com1,baud=115200,reconnect"

# ----------------------------------------------------------
# helpers
# ----------------------------------------------------------
die() { echo "ERROR: $*" >&2; exit 1; }

[ "$EUID" -eq 0 ] && die "Do NOT run as root/sudo. Run as your normal user: bash $0"

wait_socket() {
    local path="$1" label="$2"
    echo "      Waiting for $label socket..."
    for i in $(seq 1 30); do
        [ -S "$path" ] && { echo "      $label ready."; return 0; }
        sleep 2
    done
    die "Socket $path did not appear after 60 s. Check Parallels serial config."
}

socat_start() {
    echo "9090" | sudo -S $SOCAT UNIX-CONNECT:"$KD_SOCK" UNIX-CONNECT:"$DBG_SOCK" &
    SOCAT_PID=$!
    sleep 2
    kill -0 $SOCAT_PID 2>/dev/null || die "socat died immediately. Check socket permissions."
    echo "      socat PID=$SOCAT_PID"
}

# ----------------------------------------------------------
# 0. Configure serial ports (idempotent — safe to re-run)
# ----------------------------------------------------------
echo "[0/6] Configuring serial ports..."
$PRLCTL set "$TARGET"   --device-set serial0 --socket "$KD_SOCK"  --socket-mode server 2>/dev/null || true
$PRLCTL set "$DEBUGGER" --device-set serial0 --socket "$DBG_SOCK" --socket-mode server 2>/dev/null || true

# ----------------------------------------------------------
# 1. Kill any stale socat and clean old sockets
# ----------------------------------------------------------
echo "9090" | sudo -S pkill -9 -f "socat.*kd.sock" 2>/dev/null || true
echo "9090" | sudo -S pkill -9 -f "socat.*debugger.sock" 2>/dev/null || true
rm -f "$KD_SOCK" "$DBG_SOCK" 2>/dev/null || true

# ----------------------------------------------------------
# 2. Start VMs
# ----------------------------------------------------------
echo "[2/6] Starting Target VM..."
$PRLCTL start "$TARGET" 2>/dev/null || true

echo "      Starting Debugger VM..."
$PRLCTL start "$DEBUGGER" 2>/dev/null || true

# ----------------------------------------------------------
# 3. Wait for both sockets
# ----------------------------------------------------------
echo "[3/6] Waiting for serial sockets..."
wait_socket "$KD_SOCK"  "Target (kd)"
wait_socket "$DBG_SOCK" "Debugger"

# ----------------------------------------------------------
# 4. Start socat relay
# ----------------------------------------------------------
echo "[4/6] Starting socat relay..."
socat_start

# ----------------------------------------------------------
# 5. Launch WinDbg on Debugger VM (as Administrator)
#    It will sit at "Waiting to reconnect..." until Target reboots.
# ----------------------------------------------------------
echo "[5/6] Launching WinDbg on Debugger VM..."
$PRLCTL exec "$DEBUGGER" -- powershell -Command \
    "Start-Process -Verb RunAs -FilePath '$WINDBG' -ArgumentList '$WINDBG_ARGS'" \
    2>/dev/null || true

echo "      WinDbg started — waiting 5 s for it to open COM1..."
sleep 5

# ----------------------------------------------------------
# 6. Reboot Target — restart socat first (Parallels recreates
#    the socket on reboot, old fd goes dead), then reboot.
# ----------------------------------------------------------
echo "[6/6] Rebooting Target VM..."

echo "9090" | sudo -S pkill -9 -f "socat.*kd.sock" 2>/dev/null || true
sleep 1
socat_start

$PRLCTL exec "$TARGET" --current-user cmd /c "shutdown /r /t 0" 2>/dev/null || true

echo ""
echo "============================================================"
echo " All done."
echo ""
echo " WinDbg is running on the Debugger VM and watching COM1."
echo " Target is rebooting — boot break should land automatically."
echo ""
echo " If WinDbg shows 'Waiting to reconnect...' for > 30 s:"
echo "   1. Check socat is alive:  pgrep -af socat"
echo "   2. Check sockets exist:   ls -l $KD_SOCK $DBG_SOCK"
echo "   3. Manually force break:  Ctrl+Break in WinDbg"
echo ""
echo " To stop relay: sudo kill $SOCAT_PID"
echo "============================================================"
