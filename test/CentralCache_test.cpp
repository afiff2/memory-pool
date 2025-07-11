// test_central_cache.cpp
#include "CentralCache.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <random>

using namespace memory_pool;

// 单线程测试：申请并归还若干次
void testSingleThread() {
    std::cout << "[SingleThread] Start\n";
    constexpr size_t MAX_INDEX = 10;  // 测试 1..10 共 10 种大小
    constexpr int LOOPS = 1000;       // 每个大小循环次数

    for (size_t idx = 0; idx < MAX_INDEX; ++idx) {
        // 连续 LOOPS 次 fetch 和 return
        for (int i = 0; i < LOOPS; ++i) {
            void* p = CentralCache::getInstance().fetchRange(idx);
            assert(p != nullptr && "fetchRange 返回 nullptr");
            // 可以在这里填充数据测试可写性
            CentralCache::getInstance().returnRange(p, idx);
        }
        std::cout << "[SingleThread] index=" << idx << " OK\n";
    }
    std::cout << "[SingleThread] All OK\n";
}

// 多线程测试：N 个线程同时跑
void testMultiThread() {
    std::cout << "[MultiThread] Start\n";
    constexpr size_t MAX_INDEX = 10;
    constexpr int THREADS = 8;
    constexpr int OPS_PER_THREAD = 500;

    auto worker = [&](int tid){
        std::mt19937_64 rng(std::random_device{}());
        std::uniform_int_distribution<size_t> dist_idx(0, MAX_INDEX-1);
        for (int op = 0; op < OPS_PER_THREAD; ++op) {
            size_t idx = dist_idx(rng);
            void* p = CentralCache::getInstance().fetchRange(idx);
            if (!p) {
                std::cerr << "[Thread " << tid << "] fetchRange idx="<<idx<<" 返回 nullptr\n";
                continue;
            }
            // 模拟做点工作
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            CentralCache::getInstance().returnRange(p, idx);
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
