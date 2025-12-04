import os
import csv
import re

LOG_DIR = "../raw_logs/" # Directory containing log files
OUT_CSV = "../results/distributed_results.csv"

pattern = re.compile(
    r"out_(?P<client>[0-9.]+)_N(?P<N>[0-9]+)_(?P<threads>[0-9]+)_(?P<ratio>[0-9.]+)_(?P<mode>[a-z]+)\.txt"
)

def parse_latency(line):
    # Extract p50, p95, p99
    nums = re.findall(r"p\d+=([0-9]+)", line)
    if len(nums) == 3:
        return nums
    return ["", "", ""]

rows = []

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

    with open(os.path.join(LOG_DIR, fname)) as f:
        for line in f:
            if "Total ops" in line:
                ops = int(re.findall(r"(\d+)", line)[0])
            elif "Throughput" in line:
                throughput = float(re.findall(r"([0-9.]+)", line)[0])
            elif "GET Latency" in line:
                p50_get, p95_get, p99_get = parse_latency(line)
            elif "PUT Latency" in line:
                p50_put, p95_put, p99_put = parse_latency(line)

    rows.append([
        client, mode, N, threads, ratio,
        ops, throughput,
        p50_get, p95_get, p99_get,
        p50_put, p95_put, p99_put
    ])

# Write CSV
with open(OUT_CSV, "w", newline="") as f:
    writer = csv.writer(f)
    writer.writerow([
        "client_ip", "mode", "N", "threads", "ratio",
        "ops", "throughput",
        "p50_get", "p95_get", "p99_get",
        "p50_put", "p95_put", "p99_put"
    ])
    writer.writerows(rows)

print("Saved:", OUT_CSV)
# This script parses distributed key-value store log files and compiles the results into a CSV file.