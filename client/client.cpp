#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include <memory>
#include <unistd.h>

#include <grpcpp/grpcpp.h>
#include "kv.grpc.pb.h"

using namespace std::chrono_literals;

// -------------------- Global state --------------------

// Each Peer holds the network address of a replica participating in the quorum.
struct Peer {
  std::string addr;
};

static std::vector<Peer> peers;
static int N = 0;
static int R = 0;
static int W = 0;
static thread_local std::string CLIENT_ID = "";



// thread-local stubs: one stub per (thread, replica)
thread_local std::vector<std::unique_ptr<kv::ReplicaService::Stub>> tls_stubs;

// For blocking protocol timestamps
static std::atomic<int64_t> local_ts{0};

// -------------------- Helpers --------------------

/*
 * generate_unique_client_id
 * Returns a best-effort globally unique identifier constructed from
 * hostname, process id, thread id, and random bits. The ID is attached to
 * lock requests so replicas can verify ownership when unlocking.
 */
std::string generate_unique_client_id() {
    // Use: hostname + process id + thread id + random bits
    std::stringstream ss;

    // Hostname
    char hostname[256];
    gethostname(hostname, sizeof(hostname));

    // Random number for extra safety
    std::mt19937_64 rng(std::random_device{}());
    uint64_t rand_bits = rng();

    ss << hostname << "_"
       << "pid" << getpid() << "_"
       << "tid" << std::this_thread::get_id() << "_"
       << "r" << rand_bits;

    return ss.str();
}

/*
 * load_replicas
 * Input: path to a plaintext file that lists replicas line-by-line in
 * "host:port" form. Output: vector of addresses used during bootstrap.
 * Minimal validation is performed since the script controls the contents.
 */
std::vector<std::string> load_replicas(const std::string& path) {
  std::vector<std::string> addrs;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) addrs.push_back(line);
  }
  return addrs;
}

/*
 * init_peers
 * Stores the provided replica addresses, derives N/R/W quorum sizes, and
 * clears any previous state. R and W default to majority quorums.
 */
void init_peers(const std::vector<std::string>& addrs) {
  peers.clear();
  for (auto& a : addrs) {
    peers.push_back(Peer{a});
  }
  N = (int)peers.size();
  R = (N / 2) + 1;
  W = (N / 2) + 1;
}

/*
 * init_thread_local_stubs
 * Each worker thread keeps its own vector of gRPC stubs to avoid sharing
 * channels. This helper recreates per-thread stubs whenever the peer list
 * changes.
 */
void init_thread_local_stubs() {
  tls_stubs.clear();
  tls_stubs.reserve(N);
  for (auto& p : peers) {
    auto ch = grpc::CreateChannel(p.addr, grpc::InsecureChannelCredentials());
    tls_stubs.push_back(kv::ReplicaService::NewStub(ch));
  }
}

/*
 * ensure_stubs
 * Lazily initializes thread-local stubs on demand. Call before issuing any
 * RPC to guarantee tls_stubs matches the current replica count.
 */
void ensure_stubs() {
  if ((int)tls_stubs.size() != N) {
    init_thread_local_stubs();
  }
}

// -------------------- RPC wrappers --------------------

/*
 * rpc_lock / rpc_unlock / rpc_read / rpc_write
 * Each helper issues a single RPC with a short deadline and, when
 * applicable, copies reply fields into output parameters. Callers operate
 * on replica indices to avoid additional address bookkeeping.
 */
bool rpc_lock(int idx) {
  ensure_stubs();
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + 200ms);
  kv::LockRequest req;
  req.set_client_id(CLIENT_ID);
  kv::LockReply rep;
  auto s = tls_stubs[idx]->Lock(&ctx, req, &rep);
  return s.ok() && rep.granted();
}

bool rpc_unlock(int idx) {
  ensure_stubs();
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + 200ms);
  kv::UnlockRequest req;
  req.set_client_id(CLIENT_ID);
  kv::UnlockReply rep;
  auto s = tls_stubs[idx]->Unlock(&ctx, req, &rep);
  return s.ok() && rep.success();
}

bool rpc_read(int idx, std::string& value, int64_t& ts, std::string& writer) {
  ensure_stubs();
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + 200ms);
  kv::ReadRequest req;
  kv::ReadReply rep;
  auto s = tls_stubs[idx]->Read(&ctx, req, &rep);
  if (!s.ok()) return false;
  value = rep.value();
  ts = rep.timestamp();
  writer = rep.writer_id();
  return true;
}

bool rpc_write(int idx, const std::string& value, int64_t ts, const std::string& writer) {
  ensure_stubs();
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + 200ms);
  kv::WriteRequest req;
  req.set_value(value);
  req.set_timestamp(ts);
  req.set_writer_id(writer);
  kv::WriteReply rep;
  auto s = tls_stubs[idx]->Write(&ctx, req, &rep);
  return s.ok() && rep.success();
}

// -------------------- Blocking protocol --------------------

// blocking_get returns this summary of the quorum read attempt.
struct ReadResult {
  bool ok = false;
  int64_t ts = 0;
  std::string val;
  std::string writer;
};

/*
 * blocking_get
 * Implements the two-phase majority read for the blocking protocol.
 * 1) Acquire R locks, 2) read and select the highest timestamp, 3) unlock.
 * Returns {ok=false} if the quorum could not be reached.
 */
ReadResult blocking_get() {
  // 1) Acquire locks from replicas until R granted
  std::vector<int> locked_idxs;
  locked_idxs.reserve(R);

  std::vector<std::thread> ths;
  std::mutex m;
  std::atomic<bool> done{false};

  for (int i = 0; i < N; ++i) {
    ths.emplace_back([&, i]{
      if (done.load()) return;
      if (rpc_lock(i)) {
        std::lock_guard<std::mutex> g(m);
        if ((int)locked_idxs.size() < R) {
          locked_idxs.push_back(i);
          if ((int)locked_idxs.size() == R) {
            done.store(true);
          }
        } else {
          // extra lock; release immediately
          rpc_unlock(i);
        }
      }
    });
  }
  for (auto& t : ths) t.join();

  if ((int)locked_idxs.size() < R) {
    // Failed to acquire quorum; best-effort unlock
    for (int idx : locked_idxs) rpc_unlock(idx);
    return {};
  }

  // 2) Read from those R
  int64_t best_ts = -1;
  std::string best_val, best_writer;
  int success_reads = 0;
  for (int idx : locked_idxs) {
    std::string v, w;
    int64_t ts;
    if (rpc_read(idx, v, ts, w)) {
      success_reads++;
      if (ts > best_ts) {
        best_ts = ts;
        best_val = v;
        best_writer = w;
      }
    }
  }

  // 3) Unlock
  for (int idx : locked_idxs) rpc_unlock(idx);

  ReadResult r;
  r.ok = (success_reads >= R);
  r.ts = best_ts;
  r.val = best_val;
  r.writer = best_writer;
  return r;
}

/*
 * blocking_put
 * Replicates the supplied (value, timestamp, writer_id) to every replica
 * after a successful blocking_get. The call counts acknowledgments and
 * returns true only when a write quorum W responds.
 */
bool blocking_put(const std::string& value, int64_t new_ts, const std::string& writer_id) {
  std::atomic<int> acks{0};
  std::vector<std::thread> ths;
  ths.reserve(N);

  for (int i = 0; i < N; ++i) {
    ths.emplace_back([&, i]{
      if (rpc_write(i, value, new_ts, writer_id)) {
        acks.fetch_add(1);
      }
    });
  }
  for (auto& t : ths) t.join();
  return acks.load() >= W;
}

// -------------------- ABD (non-blocking) protocol --------------------

// Holds the max timestamp/value observed during the ABD read phase.
struct ABDMax {
  bool ok = false;
  int64_t ts = -1;
  std::string val;
  std::string writer;
};

// Returned to callers when ABD read completes successfully.
struct ABDReadResult {
  bool ok = false;
  int64_t ts = 0;
  std::string val;
  std::string writer;
};

/*
 * abd_read_phase_collect_max
 * Performs the first ABD phase: read from every replica, track the highest
 * timestamp, and require that at least R reads succeed. The result seeds GET
 * read/write-back as well as PUT timestamp selection.
 */
ABDMax abd_read_phase_collect_max() {
  ABDMax out;
  std::atomic<int> ok_cnt{0};
  std::mutex m;
  int64_t best_ts = -1;
  std::string best_val, best_writer;

  std::vector<std::thread> ths;
  ths.reserve(N);

  for (int i = 0; i < N; ++i) {
    ths.emplace_back([&, i]{
      std::string v, w;
      int64_t ts;
      if (rpc_read(i, v, ts, w)) {
        ok_cnt.fetch_add(1);
        std::lock_guard<std::mutex> g(m);
        if (ts > best_ts) {
          best_ts = ts;
          best_val = v;
          best_writer = w;
        }
      }
    });
  }
  for (auto& t : ths) t.join();

  if (ok_cnt.load() >= R) {
    out.ok = true;
    out.ts = best_ts;
    out.val = best_val;
    out.writer = best_writer;
  }
  return out;
}

/*
 * abd_get
 * Executes the full ABD GET by invoking the read phase and then writing the
 * winning value back to every replica. Returns ok=false when either phase
 * fails to reach a quorum.
 */
ABDReadResult abd_get() {
  ABDReadResult res;
  ABDMax mx = abd_read_phase_collect_max();
  if (!mx.ok) return res;

  std::atomic<int> acks{0};
  std::vector<std::thread> ths;
  ths.reserve(N);

  for (int i = 0; i < N; ++i) {
    ths.emplace_back([&, i]{
      if (rpc_write(i, mx.val, mx.ts, mx.writer)) {
        acks.fetch_add(1);
      }
    });
  }
  for (auto& t : ths) t.join();

  res.ok = (acks.load() >= W);
  res.ts = mx.ts;
  res.val = mx.val;
  res.writer = mx.writer;
  return res;
}

/*
 * abd_put
 * Writes a caller-provided value by first learning the highest timestamp,
 * then writing value with ts+1 until a write quorum acknowledges.
 */
bool abd_put(const std::string& value, const std::string& writer_id) {
  ABDMax mx = abd_read_phase_collect_max();
  if (!mx.ok) return false;
  const int64_t new_ts = mx.ts + 1;

  std::atomic<int> acks{0};
  std::vector<std::thread> ths;
  ths.reserve(N);

  for (int i = 0; i < N; ++i) {
    ths.emplace_back([&, i]{
      if (rpc_write(i, value, new_ts, writer_id)) {
        acks.fetch_add(1);
      }
    });
  }
  for (auto& t : ths) t.join();
  return acks.load() >= W;
}

// -------------------- Load generator --------------------

// Per-thread metrics container used by the load generator.
struct ThreadStats {
  std::vector<long long> get_lat_us;
  std::vector<long long> put_lat_us;
  long long ops = 0;
};

/*
 * run_load
 * Spawns num_threads workers that repeatedly issue GET or PUT requests for
 * duration_sec seconds. Aggregates latency samples, prints throughput stats,
 * and can target either ABD or blocking mode based on use_abd.
 */
void run_load(
    int num_threads,
    double get_ratio,
    int duration_sec,
    bool use_abd
) {
  std::atomic<bool> stop{false};

  std::vector<std::unique_ptr<ThreadStats>> all_stats;
  all_stats.reserve(num_threads);
  for (int i = 0; i < num_threads; ++i) {
    all_stats.push_back(std::make_unique<ThreadStats>());
  }

  auto worker = [&](int tid) {
    CLIENT_ID = generate_unique_client_id();
    ensure_stubs(); // init stubs for this thread

    ThreadStats& stats = *all_stats[tid];
    std::mt19937_64 rng(std::random_device{}());
    std::uniform_real_distribution<> dist(0.0, 1.0);

    while (!stop.load()) {
      double r = dist(rng);
      auto start = std::chrono::steady_clock::now();

      if (r < get_ratio) {
        if (use_abd) {
          auto res = abd_get();
          (void)res;
        } else {
          auto res = blocking_get();
          (void)res;
        }

        auto end = std::chrono::steady_clock::now();
        long long us =
          std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        stats.get_lat_us.push_back(us);
      } else {
        std::string value = "v" + std::to_string(rng() & 0xFFFF);

        if (use_abd) {
          abd_put(value, CLIENT_ID);
        } else {
          auto rres = blocking_get();
          int64_t ts = rres.ok ? rres.ts + 1 : 1;
          blocking_put(value, ts, CLIENT_ID);
        }

        auto end = std::chrono::steady_clock::now();
        long long us =
          std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        stats.put_lat_us.push_back(us);
      }

      stats.ops++;
    }
  };

  // Start threads
  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back(worker, i);
  }

  std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
  stop.store(true);

  for (auto& t : threads) t.join();

  // Merge stats
  long long total_ops = 0;
  std::vector<long long> all_gets, all_puts;

  for (int i = 0; i < num_threads; ++i) {
    ThreadStats& s = *all_stats[i];
    total_ops += s.ops;
    all_gets.insert(all_gets.end(), s.get_lat_us.begin(), s.get_lat_us.end());
    all_puts.insert(all_puts.end(), s.put_lat_us.begin(), s.put_lat_us.end());
  }

  auto percentile = [&](std::vector<long long>& v, double p) {
    if (v.empty()) return -1LL;
    std::sort(v.begin(), v.end());
    size_t idx = (size_t)(p * v.size());
    if (idx >= v.size()) idx = v.size() - 1;
    return v[idx];
  };

  double ops_sec = total_ops / (double)duration_sec;

  std::cout << "---- Load Test Results ----\n";
  std::cout << "Mode: " << (use_abd ? "ABD" : "Blocking") << "\n";
  std::cout << "Replicas: " << N << " (R=" << R << ", W=" << W << ")\n";
  std::cout << "Duration: " << duration_sec << " seconds\n";
  std::cout << "Clients: " << CLIENT_ID << "\n";
  std::cout << "Threads: " << num_threads << "\n";
  std::cout << "Get ratio: " << get_ratio << "\n";
  std::cout << "Total ops: " << total_ops << " in " << duration_sec << " seconds\n";
  std::cout << "Throughput: " << ops_sec << " ops/sec\n";
  std::cout << "GET Latency (us): p50=" << percentile(all_gets, 0.50)
            << " p95=" << percentile(all_gets, 0.95)
            << " p99=" << percentile(all_gets, 0.99) << "\n";
  std::cout << "PUT Latency (us): p50=" << percentile(all_puts, 0.50)
            << " p95=" << percentile(all_puts, 0.95)
            << " p99=" << percentile(all_puts, 0.99) << "\n";
}

// -------------------- main() --------------------

/*
 * main
 * Parses CLI flags, initializes the client runtime, and dispatches the
 * requested command (single get/put, ABD variants, or the load generator).
 */
int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Usage:\n"
              << "  ./client <replicas.txt> get\n"
              << "  ./client <replicas.txt> put <value>\n"
              << "  ./client <replicas.txt> abd_get\n"
              << "  ./client <replicas.txt> abd_put <value>\n"
              << "  ./client <replicas.txt> load <threads> <get_ratio> <seconds> <abd|block>\n";
    return 1;
  }

  auto addrs = load_replicas(argv[1]);
  if (addrs.empty()) {
    std::cerr << "No replicas.\n";
    return 1;
  }

  init_peers(addrs);
  CLIENT_ID = generate_unique_client_id();
  ensure_stubs(); // main thread's stubs

  std::string op = argv[2];

  if (op == "get") {
    auto r = blocking_get();
    if (!r.ok) {
      std::cout << "GET failed (no quorum)\n";
      return 2;
    }
    std::cout << "GET -> value='" << r.val << "' ts=" << r.ts
              << " writer=" << r.writer << "\n";
    return 0;

  } else if (op == "put") {
    if (argc < 4) {
      std::cerr << "put needs <value>\n";
      return 1;
    }
    auto r = blocking_get();
    int64_t ts = std::max<int64_t>(r.ts + 1, local_ts.fetch_add(1) + 1);
    bool ok = blocking_put(argv[3], ts, CLIENT_ID);
    std::cout << (ok ? "PUT ok" : "PUT failed") << " ts=" << ts << "\n";
    return ok ? 0 : 3;

  } else if (op == "abd_get") {
    auto r = abd_get();
    if (!r.ok) {
      std::cout << "ABD GET failed (no quorum)\n";
      return 2;
    }
    std::cout << "ABD GET -> value='" << r.val << "' ts=" << r.ts
              << " writer=" << r.writer << "\n";
    return 0;

  } else if (op == "abd_put") {
    if (argc < 4) {
      std::cerr << "abd_put needs <value>\n";
      return 1;
    }
    bool ok = abd_put(argv[3], CLIENT_ID);
    std::cout << (ok ? "ABD PUT ok" : "ABD PUT failed") << "\n";
    return ok ? 0 : 3;

  } else if (op == "load") {
    if (argc < 7) {
      std::cerr << "Usage: ./client <replicas.txt> load <threads> <get_ratio> <seconds> <abd|block>\n";
      return 1;
    }
    int num_threads = std::stoi(argv[3]);
    double get_ratio = std::stod(argv[4]);
    int duration_sec = std::stoi(argv[5]);
    std::string mode = argv[6];
    bool use_abd = (mode == "abd");
    run_load(num_threads, get_ratio, duration_sec, use_abd);
    return 0;
  } else {
    std::cerr << "unknown op\n";
    return 1;
  }
}
