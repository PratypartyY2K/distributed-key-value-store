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
  } else {
    std::cerr << "unknown op\n";
    return 1;
  }
}
