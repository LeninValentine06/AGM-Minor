#!/bin/sh
# =============================================================================
# run_agm.sh  —  AGM System Startup Script
#
# Starts all 5 AGM processes in the correct dependency order:
#
#   1. watchdog_process   — must be first (others connect to it on startup)
#   2. compute_process    — creates the shared memory + compute channel
#   3. safety_process     — maps shared memory, connects to watchdog
#   4. logger_process     — opens logger channel
#   5. sensor_process     — LAST: begins sending data once all receivers ready
#
# Each process output is redirected to its own log file AND shown on stdout
# via 'tee'.  Use Ctrl+C to stop all processes.
#
# DEMO SCENARIO (automated, driven by sensor_process fault schedule):
#   t =  0–30 s  Normal operation  — observe RR, EtCO2, FiO2, pressures
#   t = 30–60 s  APNEA injected   — ALARM_APNEA fires after 10 s (t~40 s)
#   t = 60–90 s  SENSOR FREEZE    — watchdog fault after ~200 ms
#   t = 90+ s    Recovery/Normal  — alarms clear, watchdog OK
# =============================================================================

BINDIR="./build/x86_64-debug"
LOGDIR="./logs"
mkdir -p "$LOGDIR"

# Verify binaries exist
for BIN in watchdog_process compute_process safety_process logger_process sensor_process; do
    if [ ! -x "$BINDIR/$BIN" ]; then
        echo "ERROR: $BINDIR/$BIN not found. Run 'make' first."
        exit 1
    fi
done

echo "=============================================="
echo " AGM Real-Time Monitor — Starting System"
echo "=============================================="
echo ""

# Cleanup any leftover shared memory from a previous run
rm -f /dev/shmem/agm_params 2>/dev/null

# --- 1. Watchdog (highest priority, first to start) ----------------------
echo "[startup] Starting watchdog_process..."
"$BINDIR/watchdog_process" 2>&1 | tee "$LOGDIR/watchdog.log" &
WD_PID=$!
sleep 0.3

# --- 2. Compute (creates shared memory) ----------------------------------
echo "[startup] Starting compute_process..."
"$BINDIR/compute_process" 2>&1 | tee "$LOGDIR/compute.log" &
CP_PID=$!
sleep 0.3

# --- 3. Safety (maps shared memory read-only) ----------------------------
echo "[startup] Starting safety_process..."
"$BINDIR/safety_process" 2>&1 | tee "$LOGDIR/safety.log" &
SP_PID=$!
sleep 0.2

# --- 4. Logger (low priority background task) ----------------------------
echo "[startup] Starting logger_process..."
"$BINDIR/logger_process" 2>&1 | tee "$LOGDIR/logger.log" &
LP_PID=$!
sleep 0.2

# --- 5. Sensor (last — begins sending data) ------------------------------
echo "[startup] Starting sensor_process..."
"$BINDIR/sensor_process" 2>&1 | tee "$LOGDIR/sensor.log" &
SN_PID=$!

echo ""
echo "=============================================="
echo " All processes started:"
printf "   watchdog_process  PID %d\n" $WD_PID
printf "   compute_process   PID %d\n" $CP_PID
printf "   safety_process    PID %d\n" $SP_PID
printf "   logger_process    PID %d\n" $LP_PID
printf "   sensor_process    PID %d\n" $SN_PID
echo ""
echo " Logs: $LOGDIR/"
echo " Press Ctrl+C to stop all processes."
echo "=============================================="
echo ""
echo " DEMO TIMELINE:"
echo "   t =  0–30 s  Normal operation"
echo "   t = 30–60 s  APNEA injected → watch for ALARM_APNEA at t~40 s"
echo "   t = 60–90 s  Sensor freeze  → watch for WATCHDOG FAULT"
echo "   t = 90+ s    Recovery"
echo "=============================================="

# --- Trap Ctrl+C and kill all children -----------------------------------
cleanup() {
    echo ""
    echo "[shutdown] Stopping all AGM processes..."
    kill $SN_PID $LP_PID $SP_PID $CP_PID $WD_PID 2>/dev/null
    wait $SN_PID $LP_PID $SP_PID $CP_PID $WD_PID 2>/dev/null
    rm -f /dev/shmem/agm_params 2>/dev/null
    echo "[shutdown] Done. Logs saved to $LOGDIR/"
    exit 0
}
trap cleanup INT TERM

# Wait for any process to exit (abnormal exit = fault scenario)
wait -n 2>/dev/null || wait
cleanup
