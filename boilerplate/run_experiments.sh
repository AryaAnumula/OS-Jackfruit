#!/bin/bash
# Task 5: Scheduler Experiments
# Experiment 1: Two CPU-bound containers at different nice priorities
# Experiment 2: CPU-bound vs I/O-bound at same priority

SOCKET=/tmp/mini_runtime.sock
ENGINE=./engine
LOGS=./logs

wait_for_exit() {
    local id=$1
    while sudo $ENGINE ps | grep -q "^$id.*running"; do
        sleep 0.5
    done
}

echo "============================================"
echo "EXPERIMENT 1: CPU-bound with different nice"
echo "============================================"

# Ensure fresh rootfs copies
cp -a rootfs-base rootfs-exp1a 2>/dev/null || true
cp -a rootfs-base rootfs-exp1b 2>/dev/null || true
cp cpu_hog rootfs-exp1a/
cp cpu_hog rootfs-exp1b/

# Start supervisor if not running
if ! [ -S $SOCKET ]; then
    echo "ERROR: Start the supervisor first with: sudo ./engine supervisor ./rootfs-base"
    exit 1
fi

echo "[Exp1] Starting high-priority container (nice -5)..."
T1_START=$(date +%s%N)
sudo $ENGINE start exp1a ./rootfs-exp1a "/cpu_hog 10" --nice -5

echo "[Exp1] Starting low-priority container (nice 10)..."
sudo $ENGINE start exp1b ./rootfs-exp1b "/cpu_hog 10" --nice 10

T2_START=$(date +%s%N)

echo "[Exp1] Waiting for both to finish..."
wait_for_exit exp1a
T1_END=$(date +%s%N)
wait_for_exit exp1b
T2_END=$(date +%s%N)

T1_MS=$(( (T1_END - T1_START) / 1000000 ))
T2_MS=$(( (T2_END - T2_START) / 1000000 ))

echo ""
echo "Results - Experiment 1:"
echo "  exp1a (nice -5): completed in ${T1_MS}ms"
echo "  exp1b (nice 10): completed in ${T2_MS}ms"
echo ""

echo "============================================"
echo "EXPERIMENT 2: CPU-bound vs I/O-bound"
echo "============================================"

cp -a rootfs-base rootfs-exp2a 2>/dev/null || true
cp -a rootfs-base rootfs-exp2b 2>/dev/null || true
cp cpu_hog rootfs-exp2a/
cp io_pulse rootfs-exp2b/

echo "[Exp2] Starting CPU-bound container..."
CPU_START=$(date +%s%N)
sudo $ENGINE start exp2a ./rootfs-exp2a "/cpu_hog 10"

echo "[Exp2] Starting I/O-bound container..."
IO_START=$(date +%s%N)
sudo $ENGINE start exp2b ./rootfs-exp2b "/io_pulse 10"

wait_for_exit exp2a
CPU_END=$(date +%s%N)
wait_for_exit exp2b
IO_END=$(date +%s%N)

CPU_MS=$(( (CPU_END - CPU_START) / 1000000 ))
IO_MS=$(( (IO_END - IO_START) / 1000000 ))

echo ""
echo "Results - Experiment 2:"
echo "  exp2a (cpu_hog): completed in ${CPU_MS}ms"
echo "  exp2b (io_pulse): completed in ${IO_MS}ms"
echo ""
echo "============================================"
echo "Done. Check logs/ for per-container output."
echo "============================================"