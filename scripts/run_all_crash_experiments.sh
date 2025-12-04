#!/bin/bash
set -e

###############################################################################
# Run All Crash Experiments
# -----------------------------------------------------------------------------
# Convenience wrapper that iterates through every consistency protocol, GET
# ratio, and replica-set size so we can reproduce the entire crash study with
# a single command. Each iteration simply delegates to
# `run_client_crash_experiment.sh` and throttles the loop with a brief
# cool-down to avoid overwhelming the cluster with continuous restarts.
#
# Usage:
#   ./scripts/run_all_crash_experiments.sh
#
# Environment expectations:
#   * The replica/client inventory files referenced by the delegated script
#     should already exist and contain reachable hosts.
#   * Experiments write logs under crash_logs/ (see README for layout).
###############################################################################

MODES=("abd" "block")
RATIOS=("0.9" "0.1")
NS=("1" "3" "5")
THREADS=16

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
