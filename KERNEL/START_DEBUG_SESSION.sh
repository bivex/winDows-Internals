#!/bin/bash
# ============================================================
# WinDbg Kernel Debug Session Starter
# Two Parallels ARM64 VMs connected via socat Unix socket relay
# ============================================================

set -e

TARGET="Windows 11 Pro (Target)"
DEBUGGER="Windows 11 Pro (Debugger)"
KD_SOCK="/tmp/kd.sock"
DBG_SOCK="/tmp/debugger.sock"

# ----------------------------------------------------------
# 1. Ensure serial ports are in server mode (creates sockets)
# ----------------------------------------------------------
echo "[1/5] Setting serial ports to server mode..."
prlctl set "$TARGET" --device-set serial0 --socket "$KD_SOCK" --socket-mode server 2>/dev/null
prlctl set "$DEBUGGER" --device-set serial0 --socket "$DBG_SOCK" --socket-mode server 2>/dev/null

# ----------------------------------------------------------
# 2. Kill stale processes and clean old sockets
# ----------------------------------------------------------
echo "[2/5] Cleaning up old state..."
pkill -9 socat 2>/dev/null || true
rm -f "$KD_SOCK" "$DBG_SOCK" 2>/dev/null || true

# ----------------------------------------------------------
# 3. Start VMs (Parallels creates sockets in server mode)
# ----------------------------------------------------------
echo "[3/5] Starting Target VM..."
prlctl start "$TARGET"
sleep 5

echo "      Starting Debugger VM..."
prlctl start "$DEBUGGER"
sleep 5

# ----------------------------------------------------------
# 4. Wait for sockets
# ----------------------------------------------------------
echo "[4/5] Waiting for serial sockets..."
for i in $(seq 1 20); do
    if [ -S "$KD_SOCK" ] && [ -S "$DBG_SOCK" ]; then
        echo "      Both sockets ready."
        break
    fi
    sleep 2
done

if [ ! -S "$KD_SOCK" ] || [ ! -S "$DBG_SOCK" ]; then
    echo "ERROR: Sockets not created. Check Parallels serial config."
    exit 1
fi

# ----------------------------------------------------------
# 5. Start socat relay (needs sudo — sockets owned by root)
# ----------------------------------------------------------
echo "[5/5] Starting socat relay (Target <-> Debugger)..."
sudo socat UNIX-CONNECT:"$KD_SOCK" UNIX-CONNECT:"$DBG_SOCK" &
SOCAT_PID=$!
sleep 2

if ! kill -0 $SOCAT_PID 2>/dev/null; then
    echo "ERROR: socat died immediately. Check socket permissions."
    exit 1
fi

echo "      socat PID=$SOCAT_PID"

echo ""
echo "============================================================"
echo " Session ready!"
echo ""
echo " On DEBUGGER VM run (as Admin):"
echo "   \"C:\\Program Files (x86)\\Windows Kits\\10\\Debuggers\\arm64\\windbg.exe\" -k com:port=com1,baud=115200,reconnect"
echo ""
echo " Then reboot TARGET VM:"
echo "   prlctl exec '$TARGET' cmd /c 'shutdown /r /t 0'"
echo ""
echo " To stop relay: sudo kill $SOCAT_PID"
echo "============================================================"
