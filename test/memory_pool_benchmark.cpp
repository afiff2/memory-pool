// memory_pool_benchmark.cpp (Fixed Workload Version)
// 基准测试：自定义内存池 (MemoryPool::allocate / deallocate) vs. 标准 malloc/free
// 此版本使用 固定且可复现 的工作负载：
//  1. 使用相同随机种子预生成每个线程的操作序列 (alloc/free) 与大小。
//  2. 两次基准 (自定义内存池 & malloc/free) 共享同一份序列，保证绝对公平。
// 编译 / 运行示例：
//   g++ -std=c++17 -O2 -pthread memory_pool_benchmark.cpp -o benchmark
//   ./benchmark              # 默认 12 线程 × 200 000 操作，种子=42
//   ./benchmark 16 500000 7  # 16 线程 × 500 000 操作，种子=7
//
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../MemoryPool.h"

struct Op { bool is_alloc; size_t size; };
using Workload = std::vector<std::vector<Op>>; // workload[tid][op_idx]

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

    std::vector<double> alloc_lat;
    std::vector<double> free_lat;

    uint64_t current_bytes = 0;
    uint64_t peak_bytes    = 0;
};

using Clock = std::chrono::high_resolution_clock;

// 生成固定工作负载：alloc/free 操作比例约 1:1，分配大小在 [8, 1024] 字节
Workload generate_workload(uint32_t threads, uint32_t ops_per_thread, uint64_t seed) {
    Workload wl(threads);
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<size_t> size_dist(8, 1024);

    for (uint32_t tid = 0; tid < threads; ++tid) {
        auto& vec = wl[tid];
        vec.reserve(ops_per_thread);
        size_t outstanding = 0;

        for (uint32_t i = 0; i < ops_per_thread; ++i) {
            bool do_alloc;
            if (outstanding == 0) {
                do_alloc = true;               // 必须先 alloc
            } else if (outstanding >= 128) {
                do_alloc = false;              // 控制未释放块数量
            } else {
                do_alloc = (rng() & 1);        // 随机选择 alloc/free
            }

            if (do_alloc) {
                size_t sz = size_dist(rng);
                vec.push_back({true, sz});
                ++outstanding;
            } else {
                vec.push_back({false, 0});     // size 由 runtime 决定
                --outstanding;
            }
        }
    }
    return wl;
}

// Templated benchmark runner that consumes **pre‑generated** workload
template <typename AllocateFn, typename DeallocateFn>
BenchmarkResult run_benchmark(const std::string& title,
                              AllocateFn&& allocateFn,
                              DeallocateFn&& deallocateFn,
                              const Workload& workload) {
    const uint32_t num_threads    = static_cast<uint32_t>(workload.size());
    const uint32_t ops_per_thread = static_cast<uint32_t>(workload[0].size());

    BenchmarkResult result{};
    result.title          = title;
    result.threads        = num_threads;
    result.ops_per_thread = ops_per_thread;

    std::vector<std::thread> workers;
    std::vector<ThreadStats> thread_stats(num_threads);

    auto bench_start = Clock::now();

    for (uint32_t tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            ThreadStats& stats = thread_stats[tid];
            stats.alloc_lat.reserve(ops_per_thread / 2);
            stats.free_lat.reserve(ops_per_thread / 2);

            std::vector<std::pair<void*, size_t>> local_blocks;
            local_blocks.reserve(128);

            const auto& ops = workload[tid];
            for (const auto& op : ops) {
                if (op.is_alloc) {
                    size_t sz = op.size;
                    auto t0 = Clock::now();
                    void* p  = allocateFn(sz);
                    auto t1 = Clock::now();
                    double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                    stats.alloc_lat.push_back(us);
                    ++stats.attempted;
                    if (p) {
                        ++stats.alloc_success;
                        stats.current_bytes += sz;
                        stats.peak_bytes = std::max(stats.peak_bytes, stats.current_bytes);
                        local_blocks.emplace_back(p, sz);
                    } else {
                        ++stats.alloc_fail;
                    }
                } else { // free
                    if (local_blocks.empty()) { ++stats.free_fail; ++stats.attempted; continue; }
                    auto blk = local_blocks.back();
                    local_blocks.pop_back();
                    auto t0 = Clock::now();
                    deallocateFn(blk.first, blk.second);
                    auto t1 = Clock::now();
                    double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                    stats.free_lat.push_back(us);
                    ++stats.attempted;
                    ++stats.free_success;
                    stats.current_bytes -= blk.second;
                }
            }

            // Safety net: free leftovers
            for (auto& blk : local_blocks) {
                deallocateFn(blk.first, blk.second);
            }
        });
    }

    for (auto& th : workers) th.join();

    auto bench_end = Clock::now();
    result.total_time_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(bench_end - bench_start).count());

    // Aggregate
    std::vector<double> all_alloc_lat, all_free_lat;
    for (auto& ts : thread_stats) {
        result.alloc_success += ts.alloc_success;
        result.alloc_fail    += ts.alloc_fail;
        result.free_success  += ts.free_success;
        result.free_fail     += ts.free_fail;
        result.attempted_ops += ts.attempted;
        result.peak_mem_mb   += static_cast<double>(ts.peak_bytes) / (1024.0 * 1024.0);
        all_alloc_lat.insert(all_alloc_lat.end(), ts.alloc_lat.begin(), ts.alloc_lat.end());
        all_free_lat .insert(all_free_lat .end(), ts.free_lat .begin(), ts.free_lat .end());
    }
    result.succeeded_ops = result.alloc_success + result.free_success;

    auto calc = [](std::vector<double>& v, double& avg, double& p99) {
        if (v.empty()) { avg = p99 = 0; return; }
        avg = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
        std::nth_element(v.begin(), v.begin() + static_cast<size_t>(v.size() * 0.99), v.end());
        p99 = v[static_cast<size_t>(v.size() * 0.99)];
    };
    calc(all_alloc_lat, result.avg_alloc_us, result.p99_alloc_us);
    calc(all_free_lat , result.avg_free_us , result.p99_free_us );

    return result;
}

void print_result(const BenchmarkResult& r) {
    auto fmt_int = [](uint64_t v) { return std::to_string(v); };
    auto fmt_fp  = [](double v) { std::ostringstream oss; oss << std::fixed << std::setprecision(2) << v; return oss.str(); };

    std::cout << "--- 开始运行基准测试: " << r.title << " ---\n";
    std::cout << "线程数: " << r.threads << ", 每个线程的操作数: " << r.ops_per_thread << "\n";
    std::cout << std::left << std::setw(24) << "总耗时:" << std::right << std::setw(15) << r.total_time_ms << " ms\n";
    std::cout << std::left << std::setw(24) << "总尝试操作数:" << std::right << std::setw(15) << r.attempted_ops << "\n";
    std::cout << std::left << std::setw(24) << "总成功操作数:" << std::right << std::setw(15) << r.succeeded_ops << "\n";
    double ops_sec = r.attempted_ops / (static_cast<double>(r.total_time_ms) / 1000.0);
    std::cout << std::left << std::setw(24) << "每秒操作数 (Ops/Sec):" << std::right << std::setw(15) << fmt_fp(ops_sec) << "\n";
    std::cout << std::left << std::setw(24) << "成功分配次数:" << std::right << std::setw(15) << fmt_int(r.alloc_success) << "\n";
    std::cout << std::left << std::setw(24) << "失败分配次数:" << std::right << std::setw(15) << fmt_int(r.alloc_fail) << "\n";
    std::cout << std::left << std::setw(24) << "成功释放次数:" << std::right << std::setw(15) << fmt_int(r.free_success) << "\n";
    std::cout << std::left << std::setw(24) << "平均分配延迟:" << std::right << std::setw(15) << fmt_fp(r.avg_alloc_us) << " us\n";
    std::cout << std::left << std::setw(24) << "P99 分配延迟:" << std::right << std::setw(15) << fmt_fp(r.p99_alloc_us) << " us\n";
    std::cout << std::left << std::setw(24) << "平均释放延迟:" << std::right << std::setw(15) << fmt_fp(r.avg_free_us) << " us\n";
    std::cout << std::left << std::setw(24) << "P99 释放延迟:" << std::right << std::setw(15) << fmt_fp(r.p99_free_us) << " us\n";
    std::cout << std::left << std::setw(24) << "峰值内存 (线程峰值和):" << std::right << std::setw(15) << fmt_fp(r.peak_mem_mb) << " MB\n";
    std::cout << "--- 基准测试结束: " << r.title << " ---\n\n";
}

void print_comparison(const BenchmarkResult& a, const BenchmarkResult& b) {
    auto fmt_fp = [](double v) { std::ostringstream oss; oss << std::fixed << std::setprecision(2) << v; return oss.str(); };
    auto ops_sec = [](const BenchmarkResult& r) { return r.attempted_ops / (static_cast<double>(r.total_time_ms) / 1000.0); };

    constexpr int w1 = 34, w2 = 20;
    std::cout << "--- 基准测试结果对比 ---\n";
    std::cout << std::left << std::setw(w1) << "指标" << "| "
              << std::setw(w2) << a.title << "| " << std::setw(w2) << b.title << "|\n";
    std::cout << std::string(w1 + 2 + w2 + 2 + w2 + 1, '-') << "\n";

    auto line = [&](const std::string& name, const std::string& va, const std::string& vb) {
        std::cout << std::left << std::setw(w1) << name << "| "
                  << std::setw(w2) << va << "| " << std::setw(w2) << vb << "|\n";
    };

    line("每秒操作数 (Ops/Sec,越高越好)", fmt_fp(ops_sec(a)), fmt_fp(ops_sec(b)));
    line("平均分配延迟 (us, 越低越好)", fmt_fp(a.avg_alloc_us), fmt_fp(b.avg_alloc_us));
    line("P99 分配延迟 (us, 越低越好)", fmt_fp(a.p99_alloc_us), fmt_fp(b.p99_alloc_us));
    line("平均释放延迟 (us, 越低越好)", fmt_fp(a.avg_free_us), fmt_fp(b.avg_free_us));
    line("P99 释放延迟 (us, 越低越好)", fmt_fp(a.p99_free_us), fmt_fp(b.p99_free_us));
    line("峰值内存 (MB, 线程峰值和)", fmt_fp(a.peak_mem_mb), fmt_fp(b.peak_mem_mb));
    line("成功分配次数", std::to_string(a.alloc_success), std::to_string(b.alloc_success));
    line("失败分配次数", std::to_string(a.alloc_fail), std::to_string(b.alloc_fail));
    line("成功释放次数", std::to_string(a.free_success), std::to_string(b.free_success));
    std::cout << std::string(w1 + 2 + w2 + 2 + w2 + 1, '-') << "\n";
}

int main(int argc, char* argv[]) {
    uint32_t num_threads    = argc > 1 ? static_cast<uint32_t>(std::stoi(argv[1])) : 12;
    uint32_t ops_per_thread = argc > 2 ? static_cast<uint32_t>(std::stoi(argv[2])) : 200000;
    uint64_t seed           = argc > 3 ? static_cast<uint64_t>(std::stoull(argv[3])) : 42ULL;

    // 1. 生成固定工作负载
    auto workload = generate_workload(num_threads, ops_per_thread, seed);

    // 2. 自定义内存池
    auto custom_result = run_benchmark(
        "自定义内存池", [](size_t sz) { return memory_pool::MemoryPool::allocate(sz); },
        [](void* p, size_t sz) { memory_pool::MemoryPool::deallocate(p, sz); }, workload);
    print_result(custom_result);

    // 3. 标准 malloc/free
    auto malloc_result = run_benchmark(
        "malloc/free", [](size_t sz) { return std::malloc(sz); },
        [](void* p, size_t) { std::free(p); }, workload);
    print_result(malloc_result);

    // 4. 对比
    print_comparison(custom_result, malloc_result);

    return 0;
}
