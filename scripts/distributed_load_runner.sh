#!/bin/bash

#############################
#   Distributed Load Runner #
#############################

# ---- CONFIGURATION ----

CLIENTS=(
  "172.31.34.114"   # Client1 (controller)
  "172.31.36.111"   # Client2
  "172.31.47.43"    # Client3
)

REPLICAS_FILE="replicas_3.txt"
BASE_DIR="~/distributed-key-value-store/build"
DURATION=20

THREADS_LIST=(1 2 4 8 16)
RATIO_LIST=(0.9 0.1)
MODES=("abd" "block")

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
run_remote_load() {
    CLIENT_IP=$1
    THREADS=$2
    RATIO=$3
    MODE=$4

    # Note: We pass the filename $REPLICAS_FILE to the remote client.
    # The remote client must have this file inside $BASE_DIR.
    REMOTE_CMD="cd $BASE_DIR && ./client $REPLICAS_FILE load $THREADS $RATIO $DURATION $MODE"
    
    echo "[INFO] Running load on $CLIENT_IP (N=$N, T=$THREADS, ratio=$RATIO, mode=$MODE)"
    
    # Updated output filename to include N${N} for better record keeping
    ssh ec2-user@$CLIENT_IP "$REMOTE_CMD" > "$OUTPUT_DIR/out_${CLIENT_IP}_N${N}_${THREADS}_${RATIO}_${MODE}.txt" &
}

# ---- MASTER LOOP ----
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
