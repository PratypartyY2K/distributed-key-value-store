#!/bin/bash

###############################################################################
# Distributed Load Runner
# -----------------------------------------------------------------------------
# This orchestration script fans out client load tests across a fleet of EC2
# hosts. Each remote invocation runs the `client` binary in load-test mode and
# streams its stdout back to the controller node so we retain a copy of every
# experiment. Adjust the configuration section below to target your own hosts,
# replica list, and workload matrix.
#
# Usage:
#   ./scripts/distributed_load_runner.sh
#
# Requirements:
#   * Password-less SSH access from the controller to each CLIENTS entry.
#   * The `client` binary and replica files present on every remote host.
#   * Local write access to capture log files under OUTPUT_DIR.
###############################################################################

# ---- CONFIGURATION ----

CLIENTS=(
  "172.31.34.114"   # Client1 (controller)
  "172.31.36.111"   # Client2
  "172.31.47.43"    # Client3
)

REPLICAS_FILE="replicas_3.txt"              # Passed to the client binary
BASE_DIR="~/distributed-key-value-store/build"  # Remote directory containing binaries
DURATION=20                                  # Seconds each load run should execute

THREADS_LIST=(1 2 4 8 16)                    # Thread counts to sweep
RATIO_LIST=(0.9 0.1)                         # GET ratios to sweep
MODES=("abd" "block")                        # Protocols to evaluate

OUTPUT_DIR="results_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUTPUT_DIR"

# ================================
# Derive N by counting replicas
# ================================
# Ensure the file exists locally to count lines
if [ -f "$REPLICAS_FILE" ]; then
    N=$(wc -l < "$REPLICAS_FILE")
    # Trim whitespace just in case
    N=$(echo "$N" | xargs)
    echo "[INFO] Detected N=$N replicas from $REPLICAS_FILE"
else
    echo "[ERROR] $REPLICAS_FILE not found locally. Cannot derive N."
    exit 1
fi
echo ""


# ---- RUN COMMAND ON REMOTE CLIENT ----
# Wraps the SSH call and records stdout per client/mode into OUTPUT_DIR.
run_remote_load() {
    CLIENT_IP=$1
    THREADS=$2
    RATIO=$3
    MODE=$4

    REMOTE_CMD="cd $BASE_DIR && ./client $REPLICAS_FILE load $THREADS $RATIO $DURATION $MODE"
    
    echo "[INFO] Running load on $CLIENT_IP (N=$N, T=$THREADS, ratio=$RATIO, mode=$MODE)"
    
    # Updated output filename to include N${N} for better record keeping
    ssh ec2-user@$CLIENT_IP "$REMOTE_CMD" > "$OUTPUT_DIR/out_${CLIENT_IP}_N${N}_${THREADS}_${RATIO}_${MODE}.txt" &
}

# ---- MASTER LOOP ----
# Sweep all requested combinations and fan out across the CLIENTS array.
for MODE in "${MODES[@]}"; do
    for R in "${RATIO_LIST[@]}"; do
        for T in "${THREADS_LIST[@]}"; do

            echo "====================================================="
            echo "  MODE=$MODE | N=$N | GET_RATIO=$R | THREADS=$T"
            echo "====================================================="

            # Run on all clients in parallel
            for C in "${CLIENTS[@]}"; do
               run_remote_load "$C" "$T" "$R" "$MODE"
            done

            # Wait for all clients to finish
            wait

            echo "[DONE] Completed batch: mode=$MODE ratio=$R threads=$T"
            echo ""
        done
    done
done

echo "=============================================="
echo " All distributed tests complete!"
echo " Results saved in: $OUTPUT_DIR/"
echo "=============================================="
