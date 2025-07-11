// memory_pool_benchmark.cpp
// 基准测试：自定义内存池（MemoryPool::allocate / deallocate） vs. 标准 malloc/free
// 用法：
//   ./benchmark [线程数 num_threads] [每线程操作数 ops_per_thread]
//

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../MemoryPool.h"

struct BenchmarkResult {
    std::string title;
    uint32_t threads{};
    uint32_t ops_per_thread{};

    uint64_t total_time_ms{};
    uint64_t attempted_ops{};
    uint64_t succeeded_ops{};

    uint64_t alloc_success{};
    uint64_t alloc_fail{};
    uint64_t free_success{};
    uint64_t free_fail{};

    double avg_alloc_us{};
    double p99_alloc_us{};
    double avg_free_us{};
    double p99_free_us{};

    double peak_mem_mb{}; // sum of thread peaks
};

struct ThreadStats {
    uint64_t alloc_success = 0;
    uint64_t alloc_fail   = 0;
    uint64_t free_success = 0;
    uint64_t free_fail    = 0;
    uint64_t attempted    = 0;

    std::vector<double> alloc_latencies;
    std::vector<double> free_latencies;

    uint64_t current_bytes = 0;
    uint64_t peak_bytes    = 0;
};

using Clock = std::chrono::high_resolution_clock;

template <typename AllocateFn, typename DeallocateFn>
BenchmarkResult run_benchmark(const std::string& title,
                              AllocateFn&& allocateFn,
                              DeallocateFn&& deallocateFn,
                              uint32_t num_threads,
                              uint32_t ops_per_thread) {
    BenchmarkResult result;
    result.title           = title;
    result.threads         = num_threads;
    result.ops_per_thread  = ops_per_thread;

    std::vector<std::thread> workers;
    std::vector<ThreadStats> thread_stats(num_threads);

    auto bench_start = Clock::now();

    for (uint32_t tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            ThreadStats& stats = thread_stats[tid];
            stats.alloc_latencies.reserve(ops_per_thread); // 粗略预估
            stats.free_latencies.reserve(ops_per_thread);

            std::mt19937_64 rng(Clock::now().time_since_epoch().count() + tid);
            std::uniform_int_distribution<int> op_dist(0, 1);           // 0 = alloc, 1 = free
            std::uniform_int_distribution<size_t> size_dist(8, 1024);    // 分配大小 [8,1024] 字节

            struct Block { void* ptr; size_t sz; };
            std::vector<Block> local_blocks;
            local_blocks.reserve(ops_per_thread / 2);

            for (uint32_t i = 0; i < ops_per_thread; ++i) {
                bool do_alloc = local_blocks.empty() || op_dist(rng) == 0;
                if (do_alloc) {
                    size_t sz = size_dist(rng);
                    auto t0 = Clock::now();
                    void* p  = allocateFn(sz);
                    auto t1 = Clock::now();

                    double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                    stats.alloc_latencies.push_back(us);
                    ++stats.attempted;

                    if (p) {
                        ++stats.alloc_success;
                        stats.current_bytes += sz;
                        stats.peak_bytes = std::max(stats.peak_bytes, stats.current_bytes);
                        local_blocks.push_back({p, sz});
                    } else {
                        ++stats.alloc_fail;
                    }
                } else { // free
                    Block blk = local_blocks.back();
                    local_blocks.pop_back();
                    auto t0 = Clock::now();
                    deallocateFn(blk.ptr, blk.sz);
                    auto t1 = Clock::now();
                    double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                    stats.free_latencies.push_back(us);
                    ++stats.attempted;
                    ++stats.free_success;
                    stats.current_bytes -= blk.sz;
                }
            }

            // 出于严谨性，释放剩余 block
            for (auto& blk : local_blocks) {
                deallocateFn(blk.ptr, blk.sz);
            }
        });
    }

    for (auto& th : workers) {
        th.join();
    }

    auto bench_end = Clock::now();
    result.total_time_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(bench_end - bench_start).count());

    // 汇总线程数据
    std::vector<double> all_alloc_lat, all_free_lat;
    for (auto& ts : thread_stats) {
        result.alloc_success += ts.alloc_success;
        result.alloc_fail    += ts.alloc_fail;
        result.free_success  += ts.free_success;
        result.free_fail     += ts.free_fail;
        result.attempted_ops += ts.attempted;
        result.peak_mem_mb   += static_cast<double>(ts.peak_bytes) / (1024.0 * 1024.0);
        all_alloc_lat.insert(all_alloc_lat.end(), ts.alloc_latencies.begin(), ts.alloc_latencies.end());
        all_free_lat.insert(all_free_lat.end(), ts.free_latencies.begin(), ts.free_latencies.end());
    }
    result.succeeded_ops = result.alloc_success + result.free_success;

    auto calc_stats = [](std::vector<double>& vec, double& avg, double& p99) {
        if (vec.empty()) { avg = p99 = 0.0; return; }
        double sum = std::accumulate(vec.begin(), vec.end(), 0.0);
        avg = sum / vec.size();
        std::nth_element(vec.begin(), vec.begin() + static_cast<size_t>(vec.size() * 0.99), vec.end());
        p99 = vec[static_cast<size_t>(vec.size() * 0.99)];
    };

    calc_stats(all_alloc_lat, result.avg_alloc_us, result.p99_alloc_us);
    calc_stats(all_free_lat, result.avg_free_us, result.p99_free_us);

    return result;
}

void print_result(const BenchmarkResult& r) {
    std::cout << "--- 开始运行基准测试: " << r.title << " ---\n";
    std::cout << "线程数: " << r.threads << ", 每个线程的操作数: " << r.ops_per_thread << "\n";
    std::cout << std::left << std::setw(24) << "总耗时:" << std::right << std::setw(15) << r.total_time_ms << " ms\n";
    std::cout << std::left << std::setw(24) << "总尝试操作数:" << std::right << std::setw(15) << r.attempted_ops << "\n";
    std::cout << std::left << std::setw(24) << "总成功操作数:" << std::right << std::setw(15) << r.succeeded_ops << "\n";
    double ops_per_sec = r.attempted_ops / (static_cast<double>(r.total_time_ms) / 1000.0);
    std::cout << std::left << std::setw(24) << "每秒操作数 (Ops/Sec):" << std::right << std::setw(15) << std::fixed << std::setprecision(2) << ops_per_sec << "\n";
    std::cout << std::left << std::setw(24) << "成功分配次数:" << std::right << std::setw(15) << r.alloc_success << "\n";
    std::cout << std::left << std::setw(24) << "失败分配次数:" << std::right << std::setw(15) << r.alloc_fail << "\n";
    std::cout << std::left << std::setw(24) << "成功释放次数:" << std::right << std::setw(15) << r.free_success << "\n";
    std::cout << std::left << std::setw(24) << "平均分配延迟:" << std::right << std::setw(15) << std::fixed << std::setprecision(2) << r.avg_alloc_us << " us\n";
    std::cout << std::left << std::setw(24) << "P99 分配延迟:" << std::right << std::setw(15) << r.p99_alloc_us << " us\n";
    std::cout << std::left << std::setw(24) << "平均释放延迟:" << std::right << std::setw(15) << r.avg_free_us << " us\n";
    std::cout << std::left << std::setw(24) << "P99 释放延迟:" << std::right << std::setw(15) << r.p99_free_us << " us\n";
    std::cout << std::left << std::setw(24) << "峰值内存 (线程峰值和):" << std::right << std::setw(15) << std::fixed << std::setprecision(2) << r.peak_mem_mb << " MB\n";
    std::cout << "--- 基准测试结束: " << r.title << " ---\n\n";
}

int main(int argc, char* argv[]) {
    uint32_t num_threads     = argc > 1 ? static_cast<uint32_t>(std::stoi(argv[1])) : 12;
    uint32_t ops_per_thread  = argc > 2 ? static_cast<uint32_t>(std::stoi(argv[2])) : 200000;

    // 自定义内存池基准
    auto custom_result = run_benchmark(
        "自定义内存池 (Custom Memory Pool)",
        [](size_t sz) { return memory_pool::MemoryPool::allocate(sz); },
        [](void* p, size_t sz) { memory_pool::MemoryPool::deallocate(p, sz); },
        num_threads,
        ops_per_thread);

    print_result(custom_result);

    // 标准 malloc/free 基准
    auto malloc_result = run_benchmark(
        "标准库 malloc/free",
        [](size_t sz) { return std::malloc(sz); },
        [](void* p, size_t) { std::free(p); },
        num_threads,
        ops_per_thread);

    print_result(malloc_result);

    // 简易对比表格
    std::cout << "--- 基准测试结果对比 ---\n";
    std::cout << std::left << std::setw(34) << "指标" << "| "
              << std::setw(20) << "自定义内存池" << "| "
              << std::setw(20) << "malloc/free" << "|\n";
    std::cout << std::string(34 + 2 + 20 + 2 + 20 + 1, '-') << "\n";

    auto fmt = [](double v) {
        std::ostringstream oss; oss << std::fixed << std::setprecision(2) << v; return oss.str(); };

    std::cout << std::left << std::setw(34) << "每秒操作数 (Ops/Sec,越高越好)" << "| "
              << std::setw(20) << fmt(custom_result.attempted_ops / (custom_result.total_time_ms / 1000.0)) << "| "
              << std::setw(20) << fmt(malloc_result.attempted_ops / (malloc_result.total_time_ms / 1000.0)) << "|\n";

    std::cout << std::left << std::setw(34) << "平均分配延迟 (us, 越低越好)" << "| "
              << std::setw(20) << fmt(custom_result.avg_alloc_us) << "| "
              << std::setw(20) << fmt(malloc_result.avg_alloc_us) << "|\n";

    std::cout << std::left << std::setw(34) << "P99 分配延迟 (us, 越低越好)" << "| "
              << std::setw(20) << fmt(custom_result.p99_alloc_us) << "| "
              << std::setw(20) << fmt(malloc_result.p99_alloc_us) << "|\n";

    std::cout << std::left << std::setw(34) << "平均释放延迟 (us, 越低越好)" << "| "
              << std::setw(20) << fmt(custom_result.avg_free_us) << "| "
              << std::setw(20) << fmt(malloc_result.avg_free_us) << "|\n";

    std::cout << std::left << std::setw(34) << "P99 释放延迟 (us, 越低越好)" << "| "
              << std::setw(20) << fmt(custom_result.p99_free_us) << "| "
              << std::setw(20) << fmt(malloc_result.p99_free_us) << "|\n";

    std::cout << std::left << std::setw(34) << "峰值内存 (MB, 线程峰值和)" << "| "
              << std::setw(20) << fmt(custom_result.peak_mem_mb) << "| "
              << std::setw(20) << fmt(malloc_result.peak_mem_mb) << "|\n";

    std::cout << std::left << std::setw(34) << "成功分配次数" << "| "
              << std::setw(20) << custom_result.alloc_success << "| "
              << std::setw(20) << malloc_result.alloc_success << "|\n";

    std::cout << std::left << std::setw(34) << "失败分配次数" << "| "
              << std::setw(20) << custom_result.alloc_fail << "| "
              << std::setw(20) << malloc_result.alloc_fail << "|\n";

    std::cout << std::left << std::setw(34) << "成功释放次数" << "| "
              << std::setw(20) << custom_result.free_success << "| "
              << std::setw(20) << malloc_result.free_success << "|\n";

    std::cout << std::string(34 + 2 + 20 + 2 + 20 + 1, '-') << "\n";

    return 0;
}
