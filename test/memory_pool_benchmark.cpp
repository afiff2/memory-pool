#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../MemoryPool.h" // 根据实际路径调整

// --------------------------------------------------
// Size-class & distribution definitions
constexpr size_t ALIGNMENT      = alignof(std::max_align_t);
static_assert((ALIGNMENT & (ALIGNMENT - 1)) == 0, "ALIGNMENT must be power-of-two");
constexpr size_t MAX_SMALL_SZ   = 512;
constexpr size_t MAX_MEDIUM_SZ  = 4 * 1024;
constexpr size_t MAX_LARGE_SZ   = 64 * 1024;
constexpr size_t STEP_SMALL     = ALIGNMENT;
constexpr size_t STEP_MEDIUM    = 64;
constexpr size_t STEP_LARGE     = 512;

// Ratio Small:Medium:Large = 8:4:1
constexpr uint32_t RATIO_SM = 8;
constexpr uint32_t RATIO_MD = 4;
constexpr uint32_t RATIO_LG = 1;

// Max outstanding allocations per thread
constexpr size_t OUTSTANDING_LIMIT = 512;

// Reservoir sampling size per thread (for p99 approximation)
constexpr size_t RESERVOIR_SIZE = 100000;

// Global memory counters
static std::atomic<uint64_t> g_curr_bytes{0}, g_peak_bytes{0};

struct BenchmarkResult {
    std::string title;
    uint32_t threads;
    uint32_t ops_per_thread;
    uint64_t total_time_ms;
    uint64_t attempted_ops;
    uint64_t alloc_success;
    uint64_t alloc_fail;
    uint64_t free_success;
    uint64_t free_fail;
    double   avg_alloc_us;
    double   p99_alloc_us;
    double   avg_free_us;
    double   p99_free_us;
};

struct ThreadStats {
    uint64_t attempted = 0;
    uint64_t alloc_success = 0, alloc_fail = 0;
    uint64_t free_success = 0, free_fail = 0;
    double sum_alloc_us = 0, sum_free_us = 0;
    uint64_t count_alloc = 0, count_free = 0;
    double sample_alloc[RESERVOIR_SIZE];
    double sample_free[RESERVOIR_SIZE];
};

using Clock = std::chrono::high_resolution_clock;

template <typename AllocFn, typename FreeFn>
BenchmarkResult run_benchmark_inline(const std::string& title,
                                    AllocFn&& allocFn,
                                    FreeFn&& freeFn,
                                    uint32_t threads,
                                    uint32_t ops_per_thread,
                                    uint64_t seed) {
    BenchmarkResult r{title, threads, ops_per_thread};
    g_curr_bytes = 0;
    g_peak_bytes = 0;

    std::vector<std::thread> workers;
    std::vector<ThreadStats> stats(threads);

    auto t_begin = Clock::now();
    for (uint32_t tid = 0; tid < threads; ++tid) {
        workers.emplace_back([&, tid]() {
            ThreadStats& s = stats[tid];
            std::mt19937_64 rng(seed + tid);
            std::discrete_distribution<int> class_dist{RATIO_SM, RATIO_MD, RATIO_LG};
            std::uniform_int_distribution<size_t> dist_sm(16, MAX_SMALL_SZ);
            std::uniform_int_distribution<size_t> dist_md(MAX_SMALL_SZ + 1, MAX_MEDIUM_SZ);
            std::uniform_int_distribution<size_t> dist_lg(MAX_MEDIUM_SZ + 1, MAX_LARGE_SZ);
            std::mt19937_64 coin(seed + tid ^ 0x9e3779b97f4a7c15ULL);

            std::pair<void*, size_t> blocks[OUTSTANDING_LIMIT];
            size_t blocks_size = 0;
            size_t outstanding = 0;

            for (uint32_t i = 0; i < ops_per_thread; ++i) {
                bool do_alloc;
                if      (outstanding == 0)
                    do_alloc = true;
                else if (outstanding >= OUTSTANDING_LIMIT)
                    do_alloc = false;
                else
                    do_alloc = (coin() & 1);

                s.attempted++;
                if (do_alloc) {
                    int cls = class_dist(rng);
                    size_t raw = cls == 0 ? dist_sm(rng)
                                  : cls == 1 ? dist_md(rng)
                                             : dist_lg(rng);
                    size_t step = cls == 0 ? STEP_SMALL
                                   : cls == 1 ? STEP_MEDIUM
                                              : STEP_LARGE;
                    size_t sz = (raw + step - 1) & ~(step - 1);

                    auto t0 = Clock::now();
                    void* p = allocFn(sz);
                    auto t1 = Clock::now();
                    double us = std::chrono::duration<double, std::micro>(t1 - t0).count();

                    s.sum_alloc_us += us;
                    s.count_alloc++;
                    if (s.count_alloc <= RESERVOIR_SIZE) {
                        s.sample_alloc[s.count_alloc - 1] = us;
                    } else {
                        uint64_t j = rng() % s.count_alloc;
                        if (j < RESERVOIR_SIZE) s.sample_alloc[j] = us;
                    }

                    if (p) {
                        s.alloc_success++;
                        if (blocks_size < OUTSTANDING_LIMIT) {
                            blocks[blocks_size++] = {p, sz};
                        }
                        uint64_t nb = g_curr_bytes.fetch_add(sz) + sz;
                        uint64_t prev;
                        while (nb > (prev = g_peak_bytes.load()) &&
                               !g_peak_bytes.compare_exchange_weak(prev, nb));
                        outstanding++;
                    } else {
                        s.alloc_fail++;
                    }
                } else {
                    if (blocks_size == 0) {
                        s.free_fail++;
                    } else {
                        void* ptr = blocks[--blocks_size].first;
                        size_t sz = blocks[blocks_size].second;
                        auto t0  = Clock::now();
                        freeFn(ptr, sz);
                        auto t1  = Clock::now();
                        double us = std::chrono::duration<double, std::micro>(t1 - t0).count();

                        s.sum_free_us += us;
                        s.count_free++;
                        if (s.count_free <= RESERVOIR_SIZE) {
                            s.sample_free[s.count_free - 1] = us;
                        } else {
                            uint64_t j = rng() % s.count_free;
                            if (j < RESERVOIR_SIZE) s.sample_free[j] = us;
                        }

                        s.free_success++;
                        g_curr_bytes.fetch_sub(sz);
                        outstanding--;
                    }
                }
            }
            for (size_t i = 0; i < blocks_size; ++i) {
                freeFn(blocks[i].first, blocks[i].second);
                g_curr_bytes.fetch_sub(blocks[i].second);
            }
        });
    }
    for (auto& th : workers) th.join();
    auto t_end = Clock::now();

    r.total_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_begin).count();

    // 聚合结果
    std::vector<double> all_alloc_samples;
    std::vector<double> all_free_samples;
    uint64_t total_alloc_count = 0, total_free_count = 0;
    double total_alloc_sum = 0, total_free_sum = 0;

    for (auto& s : stats) {
        r.attempted_ops   += s.attempted;
        r.alloc_success   += s.alloc_success;
        r.alloc_fail      += s.alloc_fail;
        r.free_success    += s.free_success;
        r.free_fail       += s.free_fail;
        total_alloc_sum   += s.sum_alloc_us;
        total_alloc_count += s.count_alloc;
        total_free_sum    += s.sum_free_us;
        total_free_count  += s.count_free;

        size_t na = std::min<uint64_t>(s.count_alloc, RESERVOIR_SIZE);
        for (size_t i = 0; i < na; ++i) all_alloc_samples.push_back(s.sample_alloc[i]);
        size_t nf = std::min<uint64_t>(s.count_free, RESERVOIR_SIZE);
        for (size_t i = 0; i < nf; ++i) all_free_samples.push_back(s.sample_free[i]);
    }

    r.avg_alloc_us = total_alloc_count ? (total_alloc_sum / total_alloc_count) : 0;
    r.avg_free_us  = total_free_count  ? (total_free_sum  / total_free_count)  : 0;

    auto compute_p99 = [](std::vector<double>& v) {
        if (v.empty()) return 0.0;
        size_t idx = static_cast<size_t>(v.size() * 0.99);
        std::nth_element(v.begin(), v.begin() + idx, v.end());
        return v[idx];
    };
    r.p99_alloc_us = compute_p99(all_alloc_samples);
    r.p99_free_us  = compute_p99(all_free_samples);

    return r;
}

void print_result(const BenchmarkResult& r) {
    auto fmt_i = [](uint64_t v){ return std::to_string(v); };
    auto fmt_f = [](double  v){ std::ostringstream os; os<<std::fixed<<std::setprecision(2)<<v;return os.str(); };

    std::cout << "--- 基准测试: " << r.title << " ---\n";
    std::cout << "线程数: " << r.threads << "，每线程操作: " << r.ops_per_thread << "\n";
    std::cout << std::left<<std::setw(24)<<"总耗时:"      << std::right<<std::setw(10)<<r.total_time_ms<<" ms\n"
              << std::left<<std::setw(24)<<"尝试操作数:"  << std::right<<std::setw(10)<<r.attempted_ops<<"\n"
              << std::left<<std::setw(24)<<"分配成功:"    << std::right<<std::setw(10)<<r.alloc_success<<"\n"
              << std::left<<std::setw(24)<<"分配失败:"    << std::right<<std::setw(10)<<r.alloc_fail<<"\n"
              << std::left<<std::setw(24)<<"释放成功:"    << std::right<<std::setw(10)<<r.free_success<<"\n"
              << std::left<<std::setw(24)<<"释放失败:"    << std::right<<std::setw(10)<<r.free_fail<<"\n"
              << std::left<<std::setw(24)<<"Avg alloc (us):"<< std::right<<std::setw(10)<<fmt_f(r.avg_alloc_us)<<"\n"
              << std::left<<std::setw(24)<<"P99 alloc (us):"<< std::right<<std::setw(10)<<fmt_f(r.p99_alloc_us)<<"\n"
              << std::left<<std::setw(24)<<"Avg free  (us):"<< std::right<<std::setw(10)<<fmt_f(r.avg_free_us)<<"\n"
              << std::left<<std::setw(24)<<"P99 free  (us):"<< std::right<<std::setw(10)<<fmt_f(r.p99_free_us)<<"\n"
              << "---------------------------\n\n";
}

void print_comparison(const BenchmarkResult& a, const BenchmarkResult& b) {
    auto fmt_f = [](double  v){ std::ostringstream os; os<<std::fixed<<std::setprecision(2)<<v;return os.str(); };
    auto opsps = [](const BenchmarkResult& r){ return r.attempted_ops / (r.total_time_ms / 1000.0); };
    constexpr int w1 = 34, w2 = 20;

    std::cout << "--- 对比: " << a.title << " vs " << b.title << " ---\n";
    std::cout << std::left << std::setw(w1) << "指标"    << "| " << std::setw(w2) << a.title << "| " << std::setw(w2) << b.title << "|\n";
    std::cout << std::string(w1 + 2 + w2 + 2 + w2 + 1, '-') << "\n";
    auto line = [&](const char* name, const std::string& va, const std::string& vb) {
        std::cout << std::left << std::setw(w1) << name << "| " << std::setw(w2) << va << "| " << std::setw(w2) << vb << "|\n";
    };
    line("Ops/Sec (越高越好)", fmt_f(opsps(a)), fmt_f(opsps(b)));
    line("Avg alloc (us)",      fmt_f(a.avg_alloc_us), fmt_f(b.avg_alloc_us));
    line("P99 alloc (us)",      fmt_f(a.p99_alloc_us), fmt_f(b.p99_alloc_us));
    line("Avg free  (us)",      fmt_f(a.avg_free_us),   fmt_f(b.avg_free_us));
    line("P99 free  (us)",      fmt_f(a.p99_free_us),   fmt_f(b.p99_free_us));
    std::cout << std::string(w1 + 2 + w2 + 2 + w2 + 1, '-') << "\n\n";
}

int main(int argc, char* argv[]) {
    uint32_t threads  = argc > 1 ? std::stoul(argv[1]) : 12;
    uint32_t ops      = argc > 2 ? std::stoul(argv[2]) : 200000;
    uint64_t seed     = argc > 3 ? std::stoull(argv[3]) : 42ULL;

    auto res_pool = run_benchmark_inline("自定义内存池",
                                         [](size_t sz){ return memory_pool::MemoryPool::allocate(sz); },
                                         [](void* p, size_t sz){ memory_pool::MemoryPool::deallocate(p, sz); },
                                         threads, ops, seed);
    print_result(res_pool);

    auto res_malloc = run_benchmark_inline("malloc/free",
                                           [](size_t sz){ return std::malloc(sz); },
                                           [](void* p, size_t sz){ std::free(p); },
                                           threads, ops, seed);
    print_result(res_malloc);

    print_comparison(res_pool, res_malloc);
    return 0;
}
