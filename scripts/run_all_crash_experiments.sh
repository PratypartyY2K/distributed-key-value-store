#!/bin/bash
set -e

MODES=("abd" "block")
RATIOS=("0.9" "0.1")
NS=("1" "3" "5")
THREADS=16

# Ensure the experiment script is executable
chmod +x run_client_crash_experiment.sh

echo "Starting full test suite with ${THREADS} threads..."

for mode in "${MODES[@]}"; do
  for ratio in "${RATIOS[@]}"; do
    for n in "${NS[@]}"; do
      echo "########################################################"
      echo "RUNNING: mode=$mode ratio=$ratio N=$n"
      echo "########################################################"
      
      ./run_client_crash_experiment.sh "$mode" "$ratio" "$n" "$THREADS"
      
      echo "-------------------------------------------"
      echo "Run complete. Cooling down for 5s..."
      sleep 5
    done
  done
done

echo "All experiments completed."
