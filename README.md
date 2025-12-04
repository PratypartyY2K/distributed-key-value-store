# Distributed Key-Value Store

This project implements a single-key linearizable register replicated across a
set of servers. Two quorum-based consistency algorithms are supported:

- **Blocking protocol** – behaves like a primary/backup system by requiring
  per-replica locks before performing quorum reads and writes. Locks guarantee
  that only one writer updates a replica at a time.
- **ABD protocol** – an implementation of the Attiya–Bar-Noy–Dolev algorithm
  that provides lock-free reads/writes using two-phase quorum exchanges.

Both modes share the same gRPC service (`proto/kv.proto`) and data layout
(`server/replica_state.hpp`). Clients can switch between algorithms at runtime.

## Requirements

- CMake ≥ 3.15
- A C++17 toolchain
- gRPC + Protocol Buffers (CMake targets `gRPC::grpc++` and
  `protobuf::libprotobuf` must be discoverable)

macOS with Homebrew or Linux package managers can provide those dependencies.

## Building

```bash
cmake -S . -B build
cmake --build build --target replica client -j
```

The first command configures the project and generates protobuf bindings. The
second command produces two executables inside `build/`: `replica` and `client`.

## Running Replicas

Each replica hosts the register at a unique address (`ip:port`). Start as many
replicas as desired (e.g., 3 or 5) and list their addresses in a text file that
the client will read.

```bash
# Terminal 1
./build/replica 0.0.0.0:5001

# Terminal 2
./build/replica 0.0.0.0:5002

# Terminal 3
./build/replica 0.0.0.0:5003

cat > replicas.txt <<'EOF'
localhost:5001
localhost:5002
localhost:5003
EOF
```

Quorum sizes default to `R = W = floor(N/2) + 1`, so running 3 replicas yields
`R = W = 2`.

## Client Commands

All client invocations follow `./build/client <replicas.txt> <subcommand> ...`.
`<replicas.txt>` is the file created earlier.

### Blocking protocol

```bash
# Majority read guarded by per-replica locks
./build/client replicas.txt get

# Majority write; value is replicated using the timestamp returned by GET
./build/client replicas.txt put myValue
```

The blocking PUT first performs a GET to determine a safe timestamp, then
writes to every replica and returns success once a write quorum acknowledges.

### ABD protocol

```bash
./build/client replicas.txt abd_get
./build/client replicas.txt abd_put anotherValue
```

`abd_put` reads the highest timestamp, increments it, and writes back to a
majority; `abd_get` performs a read phase followed by a write-back to ensure
future readers see a consistent value.

### Load generator

The client can also stress test the cluster:

```bash
# 8 threads, 80% reads, run for 60s in ABD mode
./build/client replicas.txt load 8 0.8 60 abd

# Same workload but using the blocking protocol
./build/client replicas.txt load 8 0.8 60 block
```

Metrics such as throughput and p50/p95/p99 latencies are printed to stdout.
You can redirect runs into CSV/JSON for additional analysis; the repository
contains `results/distributed_results.csv` as a placeholder for aggregated
numbers and `raw_logs/` for raw experiment captures.

## Repository Layout

```
client/   – CLI client implementing both protocols and the load generator
server/   – gRPC replica service and shared replica state definition
proto/    – Protobuf service definition plus CMake rules for codegen
results/  – Empty CSV for summarizing benchmark runs (user-generated)
raw_logs/ – Example log outputs captured from distributed experiments
```

## Notes

- `kv.proto` defines the full RPC surface (Read, Write, Lock, Unlock). Locks
  are ignored by ABD, but required by the blocking protocol.
- Clients generate unique IDs (hostname + pid + tid + randomness) to tag lock
  ownership and writer IDs.
- The system focuses on correctness and observability; no persistent storage is
  implemented, so replicas keep state in-memory.
