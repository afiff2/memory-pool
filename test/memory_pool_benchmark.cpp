// memory_pool_benchmark.cpp (High‑Intensity, Fixed Workload)
// --------------------------------------------------------------------
// 更高强度版本：
//  • 覆盖 16 B → 256 KB 四个 size‑class：Small / Medium / Large / XLarge
//  • 测试次数比例：Small : Medium : Large : XLarge = 32 : 16 : 4 : 1
//  • 同一随机种子一次生成每线程完整 alloc/free 序列，供两轮基准共享
// 编译示例：
//   ./benchmark             # 默认 12 线程 × 200 k 操作, seed=42
//   ./benchmark 16 1000000 9 # 16 线程 × 1 M 操作, seed=9
// --------------------------------------------------------------------

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

#include "../MemoryPool.h"   // 根据实际路径调整

// --------------------------------------------------
// Size‑class definitions
constexpr size_t ALIGNMENT      = alignof(std::max_align_t);
static_assert((ALIGNMENT & (ALIGNMENT - 1)) == 0, "ALIGNMENT must be power‑of‑two");
constexpr size_t MAX_SMALL_SZ   = 512;
constexpr size_t MAX_MEDIUM_SZ  = 4 * 1024;
constexpr size_t MAX_LARGE_SZ   = 64 * 1024;
constexpr size_t MAX_XLARGE_SZ  = 256 * 1024;
constexpr size_t STEP_SMALL     = ALIGNMENT;  // typically 8 or 16
constexpr size_t STEP_MEDIUM    = 64;
constexpr size_t STEP_LARGE     = 512;
constexpr size_t STEP_XLARGE    = 4096;

// Ratio Small:Medium:Large:XLarge = 32:16:4:1
constexpr uint32_t RATIO_SM = 32;
constexpr uint32_t RATIO_MD = 16;
constexpr uint32_t RATIO_LG = 4;
constexpr uint32_t RATIO_XL = 1;
constexpr uint32_t RATIO_TOTAL = RATIO_SM + RATIO_MD + RATIO_LG + RATIO_XL;
// --------------------------------------------------

const size_t OUTSTANDING_LIMIT = 512;  // higher than previous 128 for more pressure

struct Op { bool is_alloc; size_t size; };          // size==0 for free
using Workload = std::vector<std::vector<Op>>;      // workload[tid]

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
    double   avg_alloc_us{};
    double   p99_alloc_us{};
    double   avg_free_us{};
    double   p99_free_us{};
    double   peak_mem_mb{};  // sum of per‑thread peaks
};

struct ThreadStats {
    uint64_t alloc_success = 0, alloc_fail = 0;
    uint64_t free_success  = 0, free_fail  = 0;
    uint64_t attempted     = 0;
    std::vector<double> alloc_lat, free_lat;
    uint64_t curr_bytes = 0, peak_bytes = 0;
};

using Clock = std::chrono::high_resolution_clock;

// Utility: round size up to step
static inline size_t round_up(size_t n, size_t step) { return (n + step - 1) & ~(step - 1); }

// --------------------------------------------------
// Fixed‑workload generator matching requested distribution
Workload generate_workload(uint32_t threads, uint32_t ops_per_thread, uint64_t seed) {
    Workload wl(threads);
    std::mt19937_64 rng(seed);

    // Determine exact counts per size‑class per thread
    uint32_t base = ops_per_thread / RATIO_TOTAL;          // floor division
    uint32_t count_xl = base * RATIO_XL;
    uint32_t count_lg = base * RATIO_LG;
    uint32_t count_md = base * RATIO_MD;
    uint32_t count_sm = ops_per_thread - (count_md + count_lg + count_xl); // absorb remainder

    std::uniform_int_distribution<size_t> dist_small (16, MAX_SMALL_SZ);
    std::uniform_int_distribution<size_t> dist_medium(MAX_SMALL_SZ + 1, MAX_MEDIUM_SZ);
    std::uniform_int_distribution<size_t> dist_large (MAX_MEDIUM_SZ + 1, MAX_LARGE_SZ);
    std::uniform_int_distribution<size_t> dist_xlarge(MAX_LARGE_SZ  + 1, MAX_XLARGE_SZ);

    for (uint32_t tid = 0; tid < threads; ++tid) {
        auto& vec = wl[tid];
        vec.reserve(ops_per_thread);

        // 1. Create list of sizes according to ratio
        std::vector<size_t> sizes;
        sizes.reserve(count_sm + count_md + count_lg + count_xl);
        auto push_rand = [&](auto& dist, uint32_t cnt, size_t step) {
            for (uint32_t i = 0; i < cnt; ++i) {
                size_t raw = dist(rng);
                sizes.push_back(round_up(raw, step));
            }
        };
        push_rand(dist_small , count_sm, STEP_SMALL );
        push_rand(dist_medium, count_md, STEP_MEDIUM);
        push_rand(dist_large , count_lg, STEP_LARGE );
        push_rand(dist_xlarge, count_xl, STEP_XLARGE);

        // Shuffle to remove ordering bias
        std::shuffle(sizes.begin(), sizes.end(), rng);

        // 2. Build Op sequence: alloc/free mixed, maintain bound on outstanding blocks
        size_t outstanding = 0;
        size_t size_idx    = 0;

        for (uint32_t i = 0; i < ops_per_thread; ++i) {
            bool do_alloc;
            if (outstanding == 0)               do_alloc = true;
            else if (outstanding >= OUTSTANDING_LIMIT) do_alloc = false;
            else                                do_alloc = (rng() & 1);

            if (do_alloc && size_idx < sizes.size()) {
                vec.push_back({true, sizes[size_idx++]});
                ++outstanding;
            } else {
                vec.push_back({false, 0});      // size resolved at runtime
                if (outstanding) --outstanding;
            }
        }

        // In rare case some sizes unused due to free‑bias, append them
        while (size_idx < sizes.size()) {
            vec.push_back({true, sizes[size_idx++]});
            vec.push_back({false, 0});
        }
    }
    return wl;
}

// --------------------------------------------------
// Benchmark runner (consume pre‑generated workload)
template <typename AllocFn, typename FreeFn>
BenchmarkResult run_benchmark(const std::string& title,
                              AllocFn&& allocFn, FreeFn&& freeFn,
                              const Workload& workload) {
    uint32_t threads         = static_cast<uint32_t>(workload.size());
    uint32_t ops_per_thread  = static_cast<uint32_t>(workload.front().size());
    BenchmarkResult r{title, threads, ops_per_thread};

    std::vector<std::thread> workers;
    std::vector<ThreadStats> stats(threads);

    auto t_begin = Clock::now();

    for (uint32_t tid = 0; tid < threads; ++tid) {
        workers.emplace_back([&, tid]() {
            ThreadStats& s = stats[tid];
            s.alloc_lat.reserve(ops_per_thread / 2);
            s.free_lat .reserve(ops_per_thread / 2);
            std::vector<std::pair<void*, size_t>> blocks;
            blocks.reserve(OUTSTANDING_LIMIT);

            const auto& ops = workload[tid];
            for (auto& op : ops) {
                if (op.is_alloc) {
                    size_t sz = op.size;
                    auto t0 = Clock::now();
                    void* p  = allocFn(sz);
                    auto t1 = Clock::now();
                    double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                    s.alloc_lat.push_back(us);
                    ++s.attempted;
                    if (p) {
                        ++s.alloc_success;
                        s.curr_bytes += sz;
                        s.peak_bytes = std::max(s.peak_bytes, s.curr_bytes);
                        blocks.emplace_back(p, sz);
                    } else ++s.alloc_fail;
                } else {
                    if (blocks.empty()) { ++s.free_fail; ++s.attempted; continue; }
                    auto blk = blocks.back(); blocks.pop_back();
                    auto t0 = Clock::now();
                    freeFn(blk.first, blk.second);
                    auto t1 = Clock::now();
                    double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                    s.free_lat.push_back(us);
                    ++s.attempted; ++s.free_success;
                    s.curr_bytes -= blk.second;
                }
            }
            for (auto& blk : blocks) freeFn(blk.first, blk.second);
        });
    }
    for (auto& th : workers) th.join();

    auto t_end = Clock::now();
    r.total_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_begin).count();

    std::vector<double> all_alloc_lat, all_free_lat;
    for (auto& s : stats) {
        r.alloc_success += s.alloc_success;
        r.alloc_fail    += s.alloc_fail;
        r.free_success  += s.free_success;
        r.free_fail     += s.free_fail;
        r.attempted_ops += s.attempted;
        r.peak_mem_mb   += static_cast<double>(s.peak_bytes) / (1024.0 * 1024.0);
        all_alloc_lat.insert(all_alloc_lat.end(), s.alloc_lat.begin(), s.alloc_lat.end());
        all_free_lat .insert(all_free_lat .end(), s.free_lat .begin(), s.free_lat .end());
    }
    r.succeeded_ops = r.alloc_success + r.free_success;

    auto calc = [](std::vector<double>& v, double& avg, double& p99) {
        if (v.empty()) { avg = p99 = 0; return; }
        avg = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
        std::nth_element(v.begin(), v.begin() + static_cast<size_t>(v.size() * 0.99), v.end());
        p99 = v[static_cast<size_t>(v.size() * 0.99)];
    };
    calc(all_alloc_lat, r.avg_alloc_us, r.p99_alloc_us);
    calc(all_free_lat , r.avg_free_us , r.p99_free_us );

    return r;
}

void print_result(const BenchmarkResult& r) {
    auto fmt_int = [](uint64_t v) { return std::to_string(v); };
    auto fmt_fp  = [](double v) { std::ostringstream oss; oss << std::fixed << std::setprecision(2) << v; return oss.str(); };

    std::cout << "--- 开始运行基准测试: " << r.title << " ---\n";
    std::cout << "线程数: " << r.threads << ", 每个线程的操作数: " << r.ops_per_thread << "\n";
    std::cout << std::left << std::setw(28) << "总耗时:" << std::right << std::setw(18) << r.total_time_ms << " ms\n";
    std::cout << std::left << std::setw(28) << "总尝试操作数:" << std::right << std::setw(18) << r.attempted_ops << "\n";
    std::cout << std::left << std::setw(28) << "总成功操作数:" << std::right << std::setw(18) << r.succeeded_ops << "\n";
    double ops_sec = r.attempted_ops / (static_cast<double>(r.total_time_ms) / 1000.0);
    std::cout << std::left << std::setw(28) << "每秒操作数 (Ops/Sec):" << std::right << std::setw(18) << fmt_fp(ops_sec) << "\n";
    std::cout << std::left << std::setw(28) << "成功分配次数:" << std::right << std::setw(18) << fmt_int(r.alloc_success) << "\n";
    std::cout << std::left << std::setw(28) << "失败分配次数:" << std::right << std::setw(18) << fmt_int(r.alloc_fail) << "\n";
    std::cout << std::left << std::setw(28) << "成功释放次数:" << std::right << std::setw(18) << fmt_int(r.free_success) << "\n";
    std::cout << std::left << std::setw(28) << "平均分配延迟:" << std::right << std::setw(18) << fmt_fp(r.avg_alloc_us) << " us\n";
    std::cout << std::left << std::setw(28) << "P99 分配延迟:" << std::right << std::setw(18) << fmt_fp(r.p99_alloc_us) << " us\n";
    std::cout << std::left << std::setw(28) << "平均释放延迟:" << std::right << std::setw(18) << fmt_fp(r.avg_free_us) << " us\n";
    std::cout << std::left << std::setw(28) << "P99 释放延迟:" << std::right << std::setw(18) << fmt_fp(r.p99_free_us) << " us\n";
    std::cout << std::left << std::setw(28) << "峰值内存 (线程峰值和):" << std::right << std::setw(18) << fmt_fp(r.peak_mem_mb) << " MB\n";
    std::cout << "--- 基准测试结束: " << r.title << " ---\n\n";
}

void print_comparison(const BenchmarkResult& a, const BenchmarkResult& b) {
    auto fmt_fp = [](double v) { std::ostringstream oss; oss << std::fixed << std::setprecision(2) << v; return oss.str(); };
    auto ops_sec = [](const BenchmarkResult& r) { return r.attempted_ops / (static_cast<double>(r.total_time_ms) / 1000.0); };

    constexpr int w1 = 38, w2 = 24;
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

    auto workload = generate_workload(num_threads, ops_per_thread, seed);

    auto result_pool = run_benchmark("自定义内存池", [](size_t sz){return memory_pool::MemoryPool::allocate(sz);},
                                                  [](void* p,size_t sz){memory_pool::MemoryPool::deallocate(p,sz);}, workload);
    print_result(result_pool);

    auto result_malloc = run_benchmark("malloc/free", [](size_t sz){return std::malloc(sz);},
                                                     [](void* p,size_t){std::free(p);}, workload);
    print_result(result_malloc);

    print_comparison(result_pool, result_malloc);
    return 0;
}
