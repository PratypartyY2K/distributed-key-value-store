#pragma once
#include <string>
#include <mutex>

struct ReplicaState {
    std::string value = "";
    int64_t timestamp = 0;
    std::string writer_id = "";
    std::mutex state_mutex;

    // For the blocking protocol
    bool lock_held = false;
    std::string lock_owner = "";
    std::mutex lock_mutex;
};

