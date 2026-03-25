#!/bin/bash
# ============================================================
# WinDbg Kernel Debug Session Starter
# Two Parallels ARM64 VMs connected via socat Unix socket relay
# ============================================================

set -e

echo "[1/4] Starting Target VM..."
prlctl start "Windows 11 Pro (Target)"
sleep 5

echo "[2/4] Starting Debugger VM..."
prlctl start "Windows 11 Pro (Debugger)"
sleep 5

echo "[3/4] Waiting for Parallels to create serial sockets..."
for i in $(seq 1 15); do
    if [ -S /tmp/kd.sock ] && [ -S /tmp/debugger.sock ]; then
        echo "  Both sockets ready."
        break
    fi
    echo "  waiting... ($i)"
    sleep 1
done

if [ ! -S /tmp/kd.sock ] || [ ! -S /tmp/debugger.sock ]; then
    echo "ERROR: Sockets not created. Check Parallels serial config."
    exit 1
fi

echo "[4/4] Starting socat relay (Target <-> Debugger)..."
sudo socat UNIX-CLIENT:/tmp/kd.sock UNIX-CLIENT:/tmp/debugger.sock &
SOCAT_PID=$!
echo "  socat PID=$SOCAT_PID"

echo ""
echo "============================================================"
echo " Session ready!"
echo ""
echo " On DEBUGGER VM run (as Admin):"
echo "   \"C:\\Program Files (x86)\\Windows Kits\\10\\Debuggers\\arm64\\windbg.exe\" -k com:port=com1,baud=115200,reconnect"
echo ""
echo " Then reboot TARGET VM:"
echo "   prlctl exec 'Windows 11 Pro (Target)' cmd /c 'shutdown /r /t 0'"
echo ""
echo " To stop relay: kill $SOCAT_PID"
echo "============================================================"
