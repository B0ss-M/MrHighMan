#pragma once
#include <atomic>
#include <deque>
#include <string>
#include <utility>

namespace omni {

inline bool browser_queue_push(std::deque<std::pair<std::string, std::string>> &queue,
                               const std::string &key, const std::string &value, size_t limit,
                               std::atomic<bool> &dropped) {
    if (queue.size() >= limit) {
        dropped.store(true);
        return false;
    }
    dropped.store(false);
    queue.emplace_back(key, value);
    return true;
}

}  // namespace omni
