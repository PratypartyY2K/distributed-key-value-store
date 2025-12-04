"""Parse distributed load-test logs into a consolidated CSV report.

The log file naming scheme conveys all metadata (client IP, replica set
size, thread count, GET ratio, and protocol). This script aggregates the
per-client numbers exported by `distributed_load_runner.sh`, producing a
single CSV that can be fed directly into notebooks, spreadsheets, or plot
scripts. Only minimal assumptions are made about the log body, so it is safe
to re-run the parser as new experiments are added.

Usage:
    cd scripts
    python3 distributed_parser.py

Outputs:
    ../results/distributed_results.csv
"""

import csv
import os
import re
from typing import Iterable, List, Tuple

LOG_DIR = "../raw_logs/"  # Directory containing log files
OUT_CSV = "../results/distributed_results.csv"

pattern = re.compile(
    r"out_(?P<client>[0-9.]+)_N(?P<N>[0-9]+)_(?P<threads>[0-9]+)_(?P<ratio>[0-9.]+)_(?P<mode>[a-z]+)\.txt"
)


def parse_latency(line: str) -> Tuple[str, str, str]:
    """Return (p50, p95, p99) latency tokens extracted from a log line."""
    values = re.findall(r"p\d+=([0-9]+)", line)
    if len(values) == 3:
        return tuple(values)
    return ("", "", "")


def extract_metrics() -> List[List]:
    """Walk all log files and collect the metrics emitted by the client."""
    rows: List[List] = []
    for fname in os.listdir(LOG_DIR):
        match = pattern.match(fname)
        if not match:
            continue

        client = match.group("client")
        N = int(match.group("N"))
        threads = int(match.group("threads"))
        ratio = float(match.group("ratio"))
        mode = match.group("mode")

        ops = throughput = None
        p50_get = p95_get = p99_get = None
        p50_put = p95_put = p99_put = None

        with open(os.path.join(LOG_DIR, fname), encoding="utf-8") as logfile:
            for line in logfile:
                if "Total ops" in line:
                    ops = int(re.findall(r"(\d+)", line)[0])
                elif "Throughput" in line:
                    throughput = float(re.findall(r"([0-9.]+)", line)[0])
                elif "GET Latency" in line:
                    p50_get, p95_get, p99_get = parse_latency(line)
                elif "PUT Latency" in line:
                    p50_put, p95_put, p99_put = parse_latency(line)

        rows.append([
            client,
            mode,
            N,
            threads,
            ratio,
            ops,
            throughput,
            p50_get,
            p95_get,
            p99_get,
            p50_put,
            p95_put,
            p99_put,
        ])
    return rows


def write_report(rows: Iterable[List]) -> None:
    """Persist the consolidated metrics into OUT_CSV."""
    os.makedirs(os.path.dirname(OUT_CSV), exist_ok=True)
    with open(OUT_CSV, "w", newline="", encoding="utf-8") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow([
            "client_ip",
            "mode",
            "N",
            "threads",
            "ratio",
            "ops",
            "throughput",
            "p50_get",
            "p95_get",
            "p99_get",
            "p50_put",
            "p95_put",
            "p99_put",
        ])
        writer.writerows(rows)


def main() -> None:
    """Drive the parser end-to-end."""
    # Gather metrics from each raw log and generate the report.
    rows = extract_metrics()
    if not rows:
        print(f"[WARN] No logs found in {LOG_DIR}. Did you run distributed_load_runner?")
        return

    write_report(rows)
    print("Saved:", OUT_CSV)


if __name__ == "__main__":
    main()
