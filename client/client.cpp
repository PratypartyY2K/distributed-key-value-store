#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

#include <grpcpp/grpcpp.h>
#include "kv.grpc.pb.h"
#include <grpcpp/create_channel.h>

using namespace std::chrono_literals;

struct Peer {
  std::string addr;
  std::unique_ptr<kv::ReplicaService::Stub> stub;
};

static std::vector<Peer> peers;
static int N = 0;
static int R = 0;
static int W = 0;
static std::string CLIENT_ID = "clientA"; // change per process if you run many clients

// --- Helpers ---
std::vector<std::string> load_replicas(const std::string& path) {
  std::vector<std::string> addrs;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) if(!line.empty()) addrs.push_back(line);
  return addrs;
}

void init_stubs(const std::vector<std::string>& addrs) {
  peers.clear();
  for (auto& a : addrs) {
    auto ch = grpc::CreateChannel(a, grpc::InsecureChannelCredentials());
    peers.push_back({a, kv::ReplicaService::NewStub(ch)});
  }
}

// --- RPC wrappers (sync, simple timeouts) ---
bool rpc_lock(Peer& p) {
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + 200ms);
  kv::LockRequest req; req.set_client_id(CLIENT_ID);
  kv::LockReply rep;
  auto s = p.stub->Lock(&ctx, req, &rep);
  return s.ok() && rep.granted();
}

bool rpc_unlock(Peer& p) {
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + 200ms);
  kv::UnlockRequest req; req.set_client_id(CLIENT_ID);
  kv::UnlockReply rep;
  auto s = p.stub->Unlock(&ctx, req, &rep);
  return s.ok() && rep.success();
}

bool rpc_read(Peer& p, std::string& value, int64_t& ts, std::string& writer) {
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + 200ms);
  kv::ReadRequest req; kv::ReadReply rep;
  auto s = p.stub->Read(&ctx, req, &rep);
  if (!s.ok()) return false;
  value = rep.value(); ts = rep.timestamp(); writer = rep.writer_id();
  return true;
}

bool rpc_write(Peer& p, const std::string& value, int64_t ts, const std::string& writer) {
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + 200ms);
  kv::WriteRequest req; req.set_value(value); req.set_timestamp(ts); req.set_writer_id(writer);
  kv::WriteReply rep;
  auto s = p.stub->Write(&ctx, req, &rep);
  return s.ok() && rep.success();
}

// --- Blocking protocol (hint-based): Acquire R locks, read, then unlock ---
struct ReadResult { bool ok=false; int64_t ts=0; std::string val; std::string writer; };

ReadResult blocking_get() {
  // 1) Try to acquire locks from N and stop once we have first R granted
  std::vector<int> locked_idxs;
  locked_idxs.reserve(R);

  std::vector<std::thread> ths;
  std::mutex m;
  std::atomic<bool> done{false};

  for (int i = 0; i < N; ++i) {
    ths.emplace_back([&, i]{
      if (done.load()) return;
      if (rpc_lock(peers[i])) {
        std::lock_guard<std::mutex> g(m);
        if ((int)locked_idxs.size() < R) {
          locked_idxs.push_back(i);
          if ((int)locked_idxs.size() == R) done.store(true);
        } else {
          // Extra lock not needed; release it immediately
          rpc_unlock(peers[i]);
        }
      }
    });
  }
  for (auto& t : ths) t.join();

  if ((int)locked_idxs.size() < R) {
    // Failed to acquire quorum; best-effort unlock those we got
    for (int idx : locked_idxs) rpc_unlock(peers[idx]);
    return {};
  }

  // 2) Read from those R servers
  int64_t best_ts = -1;
  std::string best_val, best_writer;
  int success_reads = 0;
  for (int idx : locked_idxs) {
    std::string v,w; int64_t ts;
    if (rpc_read(peers[idx], v, ts, w)) {
      success_reads++;
      if (ts > best_ts) { best_ts = ts; best_val = v; best_writer = w; }
    }
  }

  // 3) Unlock everyone we locked
  for (int idx : locked_idxs) rpc_unlock(peers[idx]);

  ReadResult r;
  r.ok = (success_reads >= R);
  r.ts = best_ts; r.val = best_val; r.writer = best_writer;
  return r;
}

// Writes in the blocking design can be leader-serialized or locked similarly.
// For simplicity: write to W servers (no lock), since servers serialize writes internally;
// linearizability comes from the read path here + servers applying last-writer-wins on ts.
bool blocking_put(const std::string& value, int64_t new_ts, const std::string& writer_id) {
  // Write to all; count first W acks
  std::atomic<int> acks{0};
  std::vector<std::thread> ths;
  for (int i = 0; i < N; ++i) {
    ths.emplace_back([&, i]{
      if (rpc_write(peers[i], value, new_ts, writer_id)) acks.fetch_add(1);
    });
  }
  for (auto& t : ths) t.join();
  return acks.load() >= W;
}

// ---------- ABD (Non-blocking) IMPLEMENTATION ----------

// Read phase helper: query all N, collect max (no write-back here)
struct ABDMax {
  bool ok=false; int64_t ts=-1; std::string val; std::string writer;
};

// Read all N (in parallel). Success if >= R replies.
ABDMax abd_read_phase_collect_max() {
  ABDMax out;
  std::atomic<int> ok_cnt{0};
  std::mutex m;
  int64_t best_ts = -1; std::string best_val, best_writer;

  std::vector<std::thread> ths;
  ths.reserve(N);
  for (int i = 0; i < N; ++i) {
    ths.emplace_back([&, i]{
      std::string v,w; int64_t ts;
      if (rpc_read(peers[i], v, ts, w)) {
        ok_cnt.fetch_add(1);
        std::lock_guard<std::mutex> g(m);
        if (ts > best_ts) { best_ts = ts; best_val = v; best_writer = w; }
      }
    });
  }
  for (auto& t : ths) t.join();

  if (ok_cnt.load() >= R) {
    out.ok = true; out.ts = best_ts; out.val = best_val; out.writer = best_writer;
  }
  return out;
}

// ABD GET = read-phase (collect max) + write-back (to W)
struct ABDReadResult { bool ok=false; int64_t ts=0; std::string val; std::string writer; };

ABDReadResult abd_get() {
  ABDReadResult res;
  ABDMax mx = abd_read_phase_collect_max();
  if (!mx.ok) return res;

  // Write-back chosen (ts,val) to all N; succeed if >= W acks
  std::atomic<int> acks{0};
  std::vector<std::thread> ths;
  ths.reserve(N);
  for (int i = 0; i < N; ++i) {
    ths.emplace_back([&, i]{
      if (rpc_write(peers[i], mx.val, mx.ts, mx.writer)) acks.fetch_add(1);
    });
  }
  for (auto& t : ths) t.join();

  res.ok = (acks.load() >= W);
  res.ts = mx.ts; res.val = mx.val; res.writer = mx.writer;
  return res;
}

// ABD PUT(value) = read-phase (timestamps only) + write to W with (max_ts+1, value)
bool abd_put(const std::string& value, const std::string& writer_id) {
  ABDMax mx = abd_read_phase_collect_max();           // may return ts=-1 on empty store
  if (!mx.ok) return false;
  const int64_t new_ts = mx.ts + 1;

  std::atomic<int> acks{0};
  std::vector<std::thread> ths;
  ths.reserve(N);
  for (int i = 0; i < N; ++i) {
    ths.emplace_back([&, i]{
      if (rpc_write(peers[i], value, new_ts, writer_id)) acks.fetch_add(1);
    });
  }
  for (auto& t : ths) t.join();
  return acks.load() >= W;
}

// --- Load test framework ---
struct ThreadStats {
    std::vector<long long> get_lat_us;
    std::vector<long long> put_lat_us;
    long long ops = 0;
};

void run_load (
  int num_threads,
  double get_ratio,
  int duration_sec,
  bool use_abd // if false: use blocking get/put; if true: use ABD get/put
) {
  std::atomic<bool> stop{false};
  
   // Per-thread latency + ops counters
  std::vector<std::unique_ptr<ThreadStats>> all_stats;
  all_stats.reserve(num_threads);

  // Create per-thread stat objects
  for (int i = 0; i < num_threads; ++i) {
    all_stats.push_back(std::make_unique<ThreadStats>());
  }

  // Launch threads
  auto worker = [&](int tid) {
    ThreadStats& stats = *all_stats[tid];
    std::mt19937_64 rng(tid + std::random_device{}());
    std::uniform_real_distribution<double> dist(0.0, 1.0);

    while (!stop.load()) {
      double op_choice = dist(rng);
      auto start = std::chrono::high_resolution_clock::now();
      if (op_choice < get_ratio) {
        // GET operation
        if (use_abd) {
          abd_get();
        } else {
          blocking_get();
        }
        auto end = std::chrono::high_resolution_clock::now();
        long long lat_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        stats.get_lat_us.push_back(lat_us);
      } else {
        // PUT operation
        std::string value = "val_from_thread_" + std::to_string(rng() & 0xFFFF);
        if (use_abd) {
          abd_put(value, CLIENT_ID);
        } else {
          // For blocking_put, we need a ts; use a simple counter for demo purposes
          auto res = blocking_get();
          int64_t base_ts = res.ok ? res.ts + 1 : 1;
          blocking_put(value, base_ts, CLIENT_ID);
        }
        auto end = std::chrono::high_resolution_clock::now();
        long long lat_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        stats.put_lat_us.push_back(lat_us);
      }
      stats.ops++;
    }
  };

  // Start threads
  std::vector<std::thread> threads;
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back(worker, i);
}

  // Join threads
  for (auto& t : threads) {
    t.join();
  }

  // Merge stats
  long long total_ops = 0;
  std::vector<long long> all_get_lats;
  std::vector<long long> all_put_lats;

  for (int i = 0; i < num_threads;++i) {
    ThreadStats& stats = *all_stats[i];
    total_ops += stats.ops;
    all_get_lats.insert(all_get_lats.end(), stats.get_lat_us.begin(), stats.get_lat_us.end());
    all_put_lats.insert(all_put_lats.end(), stats.put_lat_us.begin(), stats.put_lat_us.end());
  }

  auto percentile = [&] (std::vector<long long>& data, double p) {
    if (data.empty()) return 0LL;
    std::sort(data.begin(), data.end());
    size_t idx = static_cast<size_t>(p * (data.size() - 1));
    return data[idx];
  };

  double ops_sec = static_cast<double>(total_ops) / duration_sec;
  std::cout << "Load Test Results:\n";
  std::cout << "  Total Ops: " << total_ops << "\n";
  std::cout << "  Ops/sec: " << ops_sec << "\n";
  std::cout << " Total threads: " << num_threads << "\n";
  std::cout << "  Duration (s): " << duration_sec << "\n";
  std::cout << "  GET Ratio: " << get_ratio << "\n";
  std::cout << "  Use ABD: " << (use_abd ? "yes" : "no") << "\n";
  std::cout << "  Client ID: " << CLIENT_ID << "\n";
  std::cout << "  N=" << N << " R=" << R << " W=" << W << "\n";
  std::cout << "  Total GETs: " << all_get_lats.size() << "\n";
  std::cout << "  Total PUTs: " << all_put_lats.size() << "\n";
  std::cout << "  Latency Percentiles:\n";
  if (!all_get_lats.empty()) {
    std::cout << "  GET Latencies (us): p50=" << percentile(all_get_lats, 0.50)
              << " p90=" << percentile(all_get_lats, 0.90)
              << " p99=" << percentile(all_get_lats, 0.99) << "\n";
  }
  if (!all_put_lats.empty()) {
    std::cout << "  PUT Latencies (us): p50=" << percentile(all_put_lats, 0.50)
              << " p90=" << percentile(all_put_lats, 0.90)
              << " p99=" << percentile(all_put_lats, 0.99) << "\n";
  }
}

// Simple CLI: client get|put <value> (put auto-increments ts using local counter)
int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Usage:\n"
              << "  ./client <replicas.txt> get\n"
              << "  ./client <replicas.txt> put <value>\n";
    return 1;
  }
  auto addrs = load_replicas(argv[1]);
  if (addrs.empty()) { std::cerr << "No replicas.\n"; return 1; }
  init_stubs(addrs);
  N = (int)peers.size();
  // Majority quorums:
  R = (N/2) + 1;
  W = (N/2) + 1;

  static std::atomic<int64_t> local_ts{0};

  std::string op = argv[2];
  if (op == "get") {
    auto r = blocking_get();
    if (!r.ok) { std::cout << "GET failed (no quorum)\n"; return 2; }
    std::cout << "GET -> value='" << r.val << "' ts=" << r.ts << " writer=" << r.writer << "\n";
    return 0;
  } else if (op == "put") {
    if (argc < 4) { std::cerr << "put needs <value>\n"; return 1; }
    // Get max ts first (so writes are monotonic). We can reuse blocking_get or do a light parallel read.
    auto r = blocking_get();
    int64_t ts = std::max<int64_t>(r.ts + 1, local_ts.fetch_add(1) + 1);
    bool ok = blocking_put(argv[3], ts, CLIENT_ID);
    std::cout << (ok ? "PUT ok" : "PUT failed") << " ts=" << ts << "\n";
    return ok ? 0 : 3;
  } else if (op == "abd_get") {
    auto r = abd_get();
    if (!r.ok) { std::cout << "ABD GET failed (no quorum)\n"; return 2; }
    std::cout << "ABD GET -> value='" << r.val << "' ts=" << r.ts << " writer=" << r.writer << "\n";
    return 0;
  } else if (op == "abd_put") {
    if (argc < 4) { std::cerr << "abd_put needs <value>\n"; return 1; }
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
