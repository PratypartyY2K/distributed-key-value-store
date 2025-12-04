#pragma once
#include <string>
#include <mutex>

// ReplicaState stores all per-replica mutable data for a single register.
// Clients read/write the (value, timestamp, writer_id) triple under
// state_mutex, while the blocking protocol additionally serializes writers
// using the lock_* fields guarded by lock_mutex.
struct ReplicaState {
    std::string value = "";
    int64_t timestamp = 0;
    std::string writer_id = "";
    std::mutex state_mutex;

    // Lock bookkeeping required only when running the blocking algorithm.
    bool lock_held = false;
    std::string lock_owner = "";
    std::mutex lock_mutex;
};
