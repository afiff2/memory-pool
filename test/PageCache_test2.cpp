#include "PageCache.h"
#include <vector>
#include <thread>
#include <random>
#include <chrono>
#include <cassert>
#include <iostream>
#include <mutex>

static std::mutex io_mtx;

void worker(int id) {
    using namespace memory_pool;
    std::mt19937_64 rng(id);
    std::uniform_int_distribution<std::size_t> sizeDist(0, 16); // 包含 0 和较大值
    std::vector<std::pair<void*, std::size_t>> allocs;

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < 10000; ++i) {
        bool doFree = (rng() & 1) && !allocs.empty();
        if (doFree) {
            // 随机释放
            int idx = rng() % allocs.size();
            void* ptr = allocs[idx].first;
            PageCache::getInstance().deallocateSpan(ptr);
            allocs.erase(allocs.begin() + idx);
        } else {
            // 随机申请
            std::size_t np = sizeDist(rng);
            void* p = PageCache::getInstance().allocateSpan(np);
            // 如果 np==0，我们期望返回 nullptr；否则必须非空
            if (np == 0) {
                assert(p == nullptr);
                continue;
            }
            assert(p != nullptr);

            // 检查新分配与现有分配是否重叠
            std::uintptr_t a0 = reinterpret_cast<std::uintptr_t>(p);
            std::uintptr_t a1 = a0 + np * PageCache::PAGE_SIZE;
            for (auto &pr : allocs) {
                std::uintptr_t b0 = reinterpret_cast<std::uintptr_t>(pr.first);
                std::uintptr_t b1 = b0 + pr.second * PageCache::PAGE_SIZE;
                bool overlap = !(a1 <= b0 || a0 >= b1);
                assert(!overlap && "Detected overlapping spans!");
            }

            allocs.emplace_back(p, np);
        }
    }

    // 最后释放剩余
    for (auto &pr : allocs)
        PageCache::getInstance().deallocateSpan(pr.first);

    auto end = std::chrono::steady_clock::now();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::lock_guard<std::mutex> lk(io_mtx);
    std::cout << "Thread #" << id
              << " done in " << ms << " ms, "
              << "max concurrent allocs = " << allocs.capacity()
              << std::endl;
}

int main() {
    const int N = 8;
    std::vector<std::thread> threads;
    threads.reserve(N);
    for (int i = 0; i < N; ++i)
        threads.emplace_back(worker, i + 1);
    for (auto& t : threads) t.join();
    std::cout << "All threads finished.\n";
    return 0;
}
