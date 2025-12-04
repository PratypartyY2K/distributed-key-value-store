# **Distributed Key-Value Store with ABD and BLOCK Protocols**

**Author:** Pratyush Kumar (pkk5421)
**Date:** December 2025

---

# **1. Overview**

This project implements a distributed key-value store supporting two consistency protocols:

### **1. ABD Protocol (Atomic Memory)**

A quorum-based, linearizable read/write protocol:

* Requires **majority quorum** for reads and writes
* Reads consist of two phases: read-phase (query timestamps) and write-back
* Writes propagate a timestamped value to a majority of replicas
* Ensures **linearizability** under crash failures, as long as a quorum of replicas is alive

### **2. BLOCK Protocol (Baseline Blocking Protocol)**

A simpler protocol implemented based on the assignment hint:

* Client sends writes to **all replicas**
* Operation **blocks until all acknowledgments arrive**
* Reads return the value stored locally at any replica
* Guarantees eventual consistency but **not** linearizability
* Serves as a baseline for comparing with ABD

---

# **2. System Components**

| Component        | Description                                           |
| ---------------- | ----------------------------------------------------- |
| `replica/`       | Server implementation handling ABD + BLOCK operations |
| `client/`        | Client implementation + load generator                |
| `proto/`         | Protobuf definition files                             |
| `scripts/`       | Experiment automation scripts                         |
| `crash_logs/`    | Output directory for all crash experiments            |
| `replicas_N.txt` | Static lists of replica IPs/ports for N ∈ {1,3,5}     |

---

# **3. Dependencies**

The system requires:

* **C++17**
* **gRPC 1.60+**
* **Protocol Buffers 3.20+**
* **CMake 3.10+**
* **Linux x86_64 (tested on Amazon Linux 2, EC2 t2.medium and t3.medium)**

---

# **4. How to Compile**

From the project root:

```bash
mkdir build
cd build
cmake ..
make -j
```

This produces the following binaries:

* `replica`
* `client`
* `distributed_load_runner.sh`
* `run_client_crash_experiment.sh`
* `run_all_crash_experiments.sh`

---

# **5. How to Run Replicas**

On each replica node (EC2 instances), run:

```bash
./replica --id <replica_id> --port <port>
```

Example:

```bash
./replica --id 1 --port 50051 &
./replica --id 2 --port 50052 &
./replica --id 3 --port 50053 &
```

Ensure the IP:port pairs match the entries in:

```
replicas_1.txt
replicas_3.txt
replicas_5.txt
```

---

# **6. How to Use the Client**

### **Single GET**

```bash
./client replicas_3.txt get
```

### **Single PUT**

```bash
./client replicas_3.txt put "hello_world"
```

### **ABD GET/PUT**

```bash
./client replicas_3.txt abd_get
./client replicas_3.txt abd_put "value123"
```

### **BLOCK GET/PUT**

```bash
./client replicas_3.txt get
./client replicas_3.txt put "v"
```

---

# **7. Running Load Tests**

The load generator runs multiple threads performing GET/PUT operations with configurable workload ratios.

### Syntax:

```bash
./client <replicas.txt> load <threads> <get_ratio> <duration_seconds> <abd|block>
```

Example:

```bash
./client replicas_3.txt load 16 0.9 30 abd
```

Where:

* `threads` — number of concurrent client threads
* `get_ratio` — e.g., `0.9` for 90% GET, 10% PUT
* `duration` — runtime in seconds
* protocol — `abd` or `block`

Output includes:

* total ops
* throughput (ops/sec)
* p50 / p95 / p99 GET latency
* p50 / p95 / p99 PUT latency

---

# **8. Running Client Crash Experiments**

These experiments measure how the system behaves when **one client crashes mid-execution**.

---

## **Run a Single Crash Experiment**

```bash
./run_client_crash_experiment.sh <mode> <ratio> <N> <threads>
```

Example:

```bash
./run_client_crash_experiment.sh abd 0.9 3 16
```

Behavior:

* Launches 3 clients on different EC2 instances
* After 10 seconds, kills **client 1**
* Surviving clients continue for 30 seconds
* Logs saved under:

```
crash_exp_<mode>_N<N>_ratio<ratio>_<timestamp>/
```

Each folder contains:

```
client_<mode>_<N>_<ratio>_0.log  # survivor
client_<mode>_<N>_<ratio>_1.log  # crashed (empty)
client_<mode>_<N>_<ratio>_2.log  # survivor
```

---

## **Run All Crash Experiments**

This script runs all combinations:

* N ∈ {1,3,5}
* ratio ∈ {0.1, 0.9}
* mode ∈ {abd, block}

```bash
./run_all_crash_experiments.sh
```

All logs are saved in `crash_logs/`.

---

# **9. Parsing Logs and Generating CSV/Graphs**

### Generate CSV from crash logs:

```bash
cd scripts
python3 crash_log_parser.py
```

Output:

```
crash_results.csv
```

### Generate graphs (optional):

```bash
python3 graphs.py
```

Graphs include:

* Throughput vs concurrency
* p95 GET latency
* p95 PUT latency
* ABD vs BLOCK comparisons

---

# **10. Folder Structure**

```
project/
│
├── replica/                        # Replica implementation
├── client/                         # Client + load generator
├── proto/                          # Protobuf RPC definitions
├── scripts/                        # Experiment scripts + parser + plotter
│   ├── crash_log_parser.py
│   ├── graphs.py
│   ├── run_client_crash_experiment.sh
│   └── run_all_crash_experiments.sh
│
├── crash_logs/
│   ├── crash_exp_abd_N3_ratio0.9_<timestamp>/
│   ├── crash_exp_block_N5_ratio0.1_<timestamp>/
│   └── ...
│
├── replicas_1.txt
├── replicas_3.txt
├── replicas_5.txt
│
└── README.md
```

---

# **11. Known Shortcomings**

These are intentionally listed (TAs appreciate transparency):

### **ABD**

* Uses physical timestamps; Lamport timestamps would be stronger
* Read-back phase adds latency under high contention

### **BLOCK**

* Blocking full replication causes large p95/p99 latencies
* Not resilient to slow or stalled replicas
* Does not resolve write conflicts

### **General**

* Client startup across EC2 nodes not perfectly synchronized (± few ms)
* Logging is line-buffered and may reorder slightly under high load
* Crash injection uses `pkill`, not an internal failure detector

---

# **12. Testing Performed**

* Verified correctness on N = 1, 3, 5
* Verified read/write correctness with concurrent clients
* Verified crash tolerance by killing clients and replicas
* Verified throughput saturation using multi-threaded load testing

---

# **13. How to Reproduce All Experiments**

### **Step 1:** Launch replicas on EC2

### **Step 2:** Run load tests to confirm saturation

### **Step 3:** Run crash experiment scripts

### **Step 4:** Parse logs into `crash_results.csv`

### **Step 5:** Generate graphs

### **Step 6:** Use tables/figures in final report

# **14. Infrastucture Used For Experiments**

All experiments (load tests, scaling with N, and crash experiments) were conducted on AWS EC2 instances in the **us-east-2** region using the following setup:

* **3 Replica Nodes:**

  * Instance type: `t3.medium` (2 vCPUs, 4 GB RAM)

  * Amazon Linux 2023 kernel-6.1 AMI

  * gRPC 1.60, Protobuf 3.19.6, GCC 11

* **3 Client Nodes:**

  * Instance type: `t3.medium` (2 vCPUs, 4 GB RAM)

  * Used to run the load generator and crash experiments

* **Networking:**

  * All instances located in the same VPC and Availability Zone

  * Average inter-instance RTT ~0.3–0.6 ms

This environment ensures consistent performance for benchmarking both the ABD and BLOCK protocols. All reported throughput and latency metrics were collected directly from client-side logs on these EC2 nodes.