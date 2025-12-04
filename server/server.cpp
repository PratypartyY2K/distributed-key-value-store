#include <iostream>
#include <grpcpp/grpcpp.h>
#include "kv.grpc.pb.h"
#include "replica_state.hpp"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;

// ReplicaServiceImpl owns the concrete implementation of the gRPC service and
// simply proxies each RPC into the thread-safe ReplicaState structure.
class ReplicaServiceImpl final : public kv::ReplicaService::Service {
public:
    ReplicaServiceImpl(ReplicaState* state) : state_(state) {}

    // Read returns the last committed value+metadata under the state mutex to
    // guarantee linearizable reads when serving ABD read phases.
    Status Read(ServerContext*, const kv::ReadRequest*, kv::ReadReply* reply) override {
        std::lock_guard<std::mutex> lock(state_->state_mutex);
        reply->set_value(state_->value);
        reply->set_timestamp(state_->timestamp);
        reply->set_writer_id(state_->writer_id);
        return Status::OK;
    }

    // Write installs a newer timestamp/value pair when the caller's timestamp
    // is >= the replica's, thereby acting as a last-writer-wins register.
    Status Write(ServerContext*, const kv::WriteRequest* req, kv::WriteReply* reply) override {
        std::lock_guard<std::mutex> lock(state_->state_mutex);
        if (req->timestamp() >= state_->timestamp) {
            state_->value = req->value();
            state_->timestamp = req->timestamp();
            state_->writer_id = req->writer_id();
        }
        reply->set_success(true);
        return Status::OK;
    }

    // Lock/Unlock expose per-replica mutual exclusion for the blocking
    // algorithm. Only one client may hold the lock at a time.
    Status Lock(ServerContext*, const kv::LockRequest* req, kv::LockReply* reply) override {
        std::lock_guard<std::mutex> lock(state_->lock_mutex);
        if (!state_->lock_held) {
            state_->lock_held = true;
            state_->lock_owner = req->client_id();
            reply->set_granted(true);
        } else {
            reply->set_granted(false);
        }
        return Status::OK;
    }

    // Unlock succeeds only if the caller currently owns the lock. This guards
    // against clients accidentally releasing another client's lock.
    Status Unlock(ServerContext*, const kv::UnlockRequest* req, kv::UnlockReply* reply) override {
        std::lock_guard<std::mutex> lock(state_->lock_mutex);
        if (state_->lock_owner == req->client_id()) {
            state_->lock_held = false;
            state_->lock_owner = "";
            reply->set_success(true);
        } else {
            reply->set_success(false);
        }
        return Status::OK;
    }

private:
    ReplicaState* state_;
};

// RunServer boots a single-replica process that listens on the provided
// address until termination. Each replica owns its own ReplicaState instance.
void RunServer(std::string address) {
    ReplicaState state;
    ReplicaServiceImpl service(&state);

    ServerBuilder builder;
    builder.AddListeningPort(address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Replica running at " << address << std::endl;
    server->Wait();
}

// Entry point: validates args and starts the server.
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: ./replica <ip:port>\n";
        return 1;
    }
    RunServer(argv[1]);
    return 0;
}
