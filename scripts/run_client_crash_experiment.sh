#!/bin/bash
set -e

# ================= CONFIGURATION =================
MODE=${1:-"abd"}        # Default to abd if not set
RATIO=${2:-"0.1"}       # Default ratio
N=${3:-"3"}             # Default replicas
THREADS=${4:-"1"}       # Default threads
DURATION=30             # Total experiment duration in seconds
CRASH_DELAY=10          # How long to wait before crashing a node

# Calculated buffer to ensure processes finish before copying logs
BUFFER_TIME=5           

REPLICAS_FILE="replicas_${N}.txt"

# Define client IPs
CLIENTS=(
    "172.31.34.114"   # Client 0
    "172.31.36.111"   # Client 1
    "172.31.47.43"    # Client 2
)

# Target to crash (Index 1 in the array above)
CRASH_IP="172.31.36.111"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_DIR="crash_exp_${MODE}_N${N}_ratio${RATIO}_${TIMESTAMP}"
mkdir -p "$LOG_DIR"

echo "============================================================"
echo "EXP: MODE=$MODE | N=$N | RATIO=$RATIO | THREADS=$THREADS"
echo "TIME: Duration=${DURATION}s | Crash at=${CRASH_DELAY}s"
echo "LOGS: $LOG_DIR/"
echo "============================================================"

##############################################
# 1. Start the clients normally
##############################################
echo "[1] Starting clients..."

i=0
for IP in "${CLIENTS[@]}"; do
    echo "    → Starting client $i on $IP"
    
    # Run SSH in background
    ssh ec2-user@$IP \
        "cd ~/distributed-key-value-store/build && \
         pkill -x client || true; \
         nohup ./client $REPLICAS_FILE load $THREADS $RATIO $DURATION $MODE \
         > client_${MODE}_${N}_${RATIO}_$i.log 2>&1 &" &
    
    # FIX: Use standard assignment to avoid set -e failure on 0
    i=$((i+1))
done

# Wait for the SSH background jobs to launch
wait

##############################################
# 2. Wait for Warmup, then Crash
##############################################
echo "[2] Experiment running. Waiting ${CRASH_DELAY}s before crash..."
sleep $CRASH_DELAY

echo "[3] !!! INJECTING CRASH ON $CRASH_IP !!!"
# Use -x to match exact binary name 'client' so we don't kill this script
ssh ec2-user@$CRASH_IP "pkill -x client || true"
echo "    → Client on $CRASH_IP killed."

##############################################
# 3. Wait for remaining duration
##############################################
# Calculate remaining time: Total Duration - Time already elapsed + Buffer
REMAINING_TIME=$((DURATION - CRASH_DELAY + BUFFER_TIME))

echo "[4] Waiting ${REMAINING_TIME}s for surviving clients to finish..."
sleep $REMAINING_TIME

##############################################
# 4. Force Cleanup & Collect logs
##############################################
echo "[5] Ensuring all clients are stopped..."
for IP in "${CLIENTS[@]}"; do
    # Just in case processes hung, kill them before copying
    ssh -o ConnectTimeout=2 ec2-user@$IP "pkill -x client || true" &
done
wait

echo "[6] Collecting logs into $LOG_DIR/ ..."
i=0
for IP in "${CLIENTS[@]}"; do
    echo "    → Copying logs from $IP"
    scp -o ConnectTimeout=5 ec2-user@$IP:~/distributed-key-value-store/build/client_${MODE}_${N}_${RATIO}_$i.log "$LOG_DIR/" \
        || echo "WARNING: Failed to fetch log for client $i"
    i=$((i+1))
done

echo "============================================================"
echo "DONE. Logs stored in: $LOG_DIR/"
