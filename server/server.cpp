#include <iostream>
#include <grpcpp/grpcpp.h>
#include "kv.grpc.pb.h"
#include "replica_state.hpp"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;

class ReplicaServiceImpl final : public kv::ReplicaService::Service {
public:
    ReplicaServiceImpl(ReplicaState* state) : state_(state) {}

    Status Read(ServerContext*, const kv::ReadRequest*, kv::ReadReply* reply) override {
        std::lock_guard<std::mutex> lock(state_->state_mutex);
        reply->set_value(state_->value);
        reply->set_timestamp(state_->timestamp);
        reply->set_writer_id(state_->writer_id);
        return Status::OK;
    }

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

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: ./replica <ip:port>\n";
        return 1;
    }
    RunServer(argv[1]);
    return 0;
}

