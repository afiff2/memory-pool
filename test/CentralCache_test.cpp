// test_central_cache.cpp
#include "CentralCache.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <random>

using namespace memory_pool;

// 单线程测试：每次只拿 1 个块
void testSingleThread() {
    std::cout << "[SingleThread] Start\n";
    constexpr size_t MAX_INDEX = NUM_CLASSES;  // 类别总数
    constexpr int LOOPS = 1000;                // 每个 index 循环次数

    for (size_t idx = 0; idx < MAX_INDEX; ++idx) {
        for (int i = 0; i < LOOPS; ++i) {
            // 尝试 fetch 1 个块
            auto res = CentralCache::getInstance().fetchRange(idx, 1);
            assert(res.head != nullptr && res.count == 1 && "fetchRange 返回不符合预期");
            // 使用完毕立刻归还
            CentralCache::getInstance().returnRange(res.head, idx);
        }
        std::cout << "[SingleThread] index=" << idx << " OK\n";
    }
    std::cout << "[SingleThread] All OK\n";
}

// 多线程测试：并发随机申请/归还，每次也只拿 1 个块
void testMultiThread() {
    std::cout << "[MultiThread] Start\n";
    constexpr size_t MAX_INDEX = NUM_CLASSES;
    constexpr int THREADS = 8;
    constexpr int OPS_PER_THREAD = 500;

    auto worker = [&](int tid) {
        std::mt19937_64 rng(std::random_device{}());
        std::uniform_int_distribution<size_t> dist_idx(0, MAX_INDEX - 1);

        for (int op = 0; op < OPS_PER_THREAD; ++op) {
            size_t idx = dist_idx(rng);
            auto res = CentralCache::getInstance().fetchRange(idx, 1);
            if (!res.head) {
                std::cerr << "[Thread " << tid
                          << "] fetchRange idx=" << idx << " 返回 nullptr\n";
                continue;
            }
            // 模拟一些工作
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            CentralCache::getInstance().returnRange(res.head, idx);
        }
        std::cout << "[Thread " << tid << "] Done\n";
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t)
        threads.emplace_back(worker, t);
    for (auto &th : threads)
        th.join();

    std::cout << "[MultiThread] All threads completed\n";
}

int main() {
    testSingleThread();
    testMultiThread();
    std::cout << "=== All tests passed ===\n";
    return 0;
}
