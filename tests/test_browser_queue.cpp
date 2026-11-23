#include <cstdio>
#include <deque>
#include <string>
#include "../src/core/browser_queue.hpp"

int main() {
    std::deque<std::pair<std::string, std::string>> queue;
    std::atomic<bool> dropped{false};
    for (int i = 0; i < 32; ++i)
        if (!omni::browser_queue_push(queue, "refresh", "1", 32, dropped)) return 1;
    if (omni::browser_queue_push(queue, "refresh", "1", 32, dropped)) return 2;
    if (!dropped.load() || queue.size() != 32) return 3;
    queue.pop_front();
    if (!omni::browser_queue_push(queue, "refresh", "1", 32, dropped)) return 4;
    const bool ok = queue.size() == 32 && !dropped.load();
    std::printf(ok ? "PASSED\n" : "FAILED\n");
    return ok ? 0 : 5;
}
