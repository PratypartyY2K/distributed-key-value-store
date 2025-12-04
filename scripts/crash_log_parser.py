import os
import re
import csv

# Strict patterns to parse GET and PUT latency lines correctly
pattern = {
    "ops": re.compile(r"Total ops:\s+(\d+)"),
    "throughput": re.compile(r"Throughput:\s+([0-9.]+)"),

    "p50_get": re.compile(r"GET Latency \(us\): p50=(\d+)"),
    "p95_get": re.compile(r"GET Latency \(us\): p50=\d+\s+p95=(\d+)"),
    "p99_get": re.compile(r"GET Latency \(us\): .*p99=(\d+)"),

    "p50_put": re.compile(r"PUT Latency \(us\): p50=(\d+)"),
    "p95_put": re.compile(r"PUT Latency \(us\): p50=\d+\s+p95=(\d+)"),
    "p99_put": re.compile(r"PUT Latency \(us\): .*p99=(\d+)")
}

def extract(path):
    """Extract metrics from a single log file"""
    with open(path, "r") as f:
        text = f.read()

    def m(key):
        match = pattern[key].search(text)
        return match.group(1) if match else ""

    return {
        "ops": m("ops"),
        "throughput": m("throughput"),
        "p50_get": m("p50_get"),
        "p95_get": m("p95_get"),
        "p99_get": m("p99_get"),
        "p50_put": m("p50_put"),
        "p95_put": m("p95_put"),
        "p99_put": m("p99_put"),
    }

def main():
    base_dir = os.path.join(os.getcwd(), "crash_logs")
    output_csv = os.path.join(os.getcwd(), "crash_results.csv")

    rows = []

    for folder in os.listdir(base_dir):
        if not folder.startswith("crash_exp_"):
            continue
        
        fullpath = os.path.join(base_dir, folder)

        # crash_exp_abd_N3_ratio0.9_20251204_193739
        parts = folder.split("_")

        mode = parts[2]            # abd/block
        N = parts[3][1:]           # N1,N3,N5 → 1,3,5
        ratio = parts[4][5:]       # ratio0.9 → 0.9

        for fname in os.listdir(fullpath):
            if not fname.endswith(".log"):
                continue

            client_id = fname.split("_")[-1].split(".")[0]   # 0,1,2
            crashed = "yes" if client_id == "1" else "no"

            path = os.path.join(fullpath, fname)
            metrics = extract(path)

            rows.append({
                "mode": mode,
                "N": N,
                "ratio": ratio,
                "client": client_id,
                "crashed_client": crashed,
                **metrics
            })

    # Write CSV
    with open(output_csv, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow([
            "mode", "N", "ratio", "client", "crashed_client",
            "ops", "throughput",
            "p50_get", "p95_get", "p99_get",
            "p50_put", "p95_put", "p99_put"
        ])

        for r in rows:
            writer.writerow([
                r["mode"], r["N"], r["ratio"], r["client"], r["crashed_client"],
                r["ops"], r["throughput"],
                r["p50_get"], r["p95_get"], r["p99_get"],
                r["p50_put"], r["p95_put"], r["p99_put"]
            ])

    print("CSV generated:", output_csv)

if __name__ == "__main__":
    main()
