#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
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
constexpr size_t STEP_SMALL     = ALIGNMENT;  // typically 8 or 16
constexpr size_t STEP_MEDIUM    = 64;
constexpr size_t STEP_LARGE     = 512;

// Ratio Small:Medium:Large = 8:4:1
constexpr uint32_t RATIO_SM = 8;
constexpr uint32_t RATIO_MD = 4;
constexpr uint32_t RATIO_LG = 1;

// Max outstanding allocations per thread
constexpr size_t OUTSTANDING_LIMIT = 512;

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
    std::vector<double> alloc_lat, free_lat;
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

    std::cout << "--- 开始单元测试: " << r.title << " ---\n";
    auto t_begin = Clock::now();
    for (uint32_t tid = 0; tid < threads; ++tid) {
        workers.emplace_back([&, tid]() {
            ThreadStats& s = stats[tid];
            s.alloc_lat.reserve(ops_per_thread / 2);
            s.free_lat .reserve(ops_per_thread / 2);

            std::mt19937_64 rng(seed + tid);
            std::discrete_distribution<int> class_dist{RATIO_SM, RATIO_MD, RATIO_LG};
            std::uniform_int_distribution<size_t> dist_sm (16, MAX_SMALL_SZ);
            std::uniform_int_distribution<size_t> dist_md(MAX_SMALL_SZ + 1, MAX_MEDIUM_SZ);
            std::uniform_int_distribution<size_t> dist_lg(MAX_MEDIUM_SZ + 1, MAX_LARGE_SZ);
            std::mt19937_64 coin(seed + tid ^ 0x9e3779b97f4a7c15ULL);

            std::vector<std::pair<void*, size_t>> blocks;
            blocks.reserve(OUTSTANDING_LIMIT);
            size_t outstanding = 0;

            for (uint32_t i = 0; i < ops_per_thread; ++i) {
                bool do_alloc;
                if      (outstanding == 0)            do_alloc = true;
                else if (outstanding >= OUTSTANDING_LIMIT) do_alloc = false;
                else                                  do_alloc = (coin() & 1);

                ++s.attempted;
                if (do_alloc) {
                    int cls = class_dist(rng);
                    size_t raw = (cls == 0 ? dist_sm(rng)
                                      : cls == 1 ? dist_md(rng)
                                                 : dist_lg(rng));
                    size_t step = (cls == 0 ? STEP_SMALL
                                      : cls == 1 ? STEP_MEDIUM
                                                 : STEP_LARGE);
                    size_t sz = (raw + step - 1) & ~(step - 1);

                    auto t0 = Clock::now();
                    void* p = allocFn(sz);
                    auto t1 = Clock::now();
                    s.alloc_lat.push_back(
                        std::chrono::duration<double, std::micro>(t1 - t0).count());

                    if (p) {
                        ++s.alloc_success;
                        blocks.emplace_back(p, sz);
                        uint64_t nb = g_curr_bytes.fetch_add(sz) + sz;
                        uint64_t prev;
                        while (nb > (prev = g_peak_bytes.load()) &&
                               !g_peak_bytes.compare_exchange_weak(prev, nb));
                        ++outstanding;
                    } else {
                        ++s.alloc_fail;
                    }
                } else {
                    if (blocks.empty()) {
                        ++s.free_fail;
                    } else {
                        auto [ptr, sz] = blocks.back();
                        blocks.pop_back();
                        auto t0 = Clock::now();
                        freeFn(ptr, sz);
                        auto t1 = Clock::now();
                        s.free_lat.push_back(
                            std::chrono::duration<double, std::micro>(t1 - t0).count());
                        ++s.free_success;
                        g_curr_bytes.fetch_sub(sz);
                        --outstanding;
                    }
                }
            }
            // 回收剩余块
            for (auto& blk : blocks) {
                freeFn(blk.first, blk.second);
                g_curr_bytes.fetch_sub(blk.second);
            }
        });
    }
    for (auto& th : workers) th.join();
    auto t_end = Clock::now();
    std::cout << "--- 结束单元测试: " << r.title << " ---\n";

    r.total_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_begin).count();

    // 汇总
    std::vector<double> all_alloc, all_free;
    for (auto& s : stats) {
        r.attempted_ops += s.attempted;
        r.alloc_success += s.alloc_success;
        r.alloc_fail    += s.alloc_fail;
        r.free_success  += s.free_success;
        r.free_fail     += s.free_fail;
        all_alloc.insert(all_alloc.end(), s.alloc_lat.begin(), s.alloc_lat.end());
        all_free .insert(all_free .end(), s.free_lat.begin(), s.free_lat.end());
    }

    auto calc = [](std::vector<double>& v, double& avg, double& p99) {
        if (v.empty()) { avg = p99 = 0; return; }
        avg = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
        size_t idx = static_cast<size_t>(v.size() * 0.99);
        std::nth_element(v.begin(), v.begin() + idx, v.end());
        p99 = v[idx];
    };
    calc(all_alloc, r.avg_alloc_us, r.p99_alloc_us);
    calc(all_free , r.avg_free_us , r.p99_free_us );

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

int main(int argc, char* argv[]) {
    uint32_t threads    = argc > 1 ? std::stoul(argv[1]) : 12;
    uint32_t ops        = argc > 2 ? std::stoul(argv[2]) : 200000;
    uint64_t seed       = argc > 3 ? std::stoull(argv[3]) : 42ULL;

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

    return 0;
}
