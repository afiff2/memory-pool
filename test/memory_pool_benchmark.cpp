// memory_pool_benchmark.cpp (High-Intensity, Fixed Workload)
// --------------------------------------------------------------------
// • 覆盖 16 B → 64 KB 四个 size-class：Small / Medium / Large
// • 测试次数比例：Small : Medium : Large = 8 : 4 : 1
// • 同一随机种子一次生成每线程完整 alloc/free 序列，供两轮基准共享
// 编译示例：
//   ./benchmark             # 默认 12 线程 × 200 k 操作, seed=42
//   ./benchmark 16 1000000 9 # 16 线程 × 1 M 操作, seed=9
// --------------------------------------------------------------------

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

#include "../MemoryPool.h"   // 根据实际路径调整

// --------------------------------------------------
// Size-class definitions
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
constexpr uint32_t RATIO_TOTAL = RATIO_SM + RATIO_MD + RATIO_LG;
// --------------------------------------------------

const size_t OUTSTANDING_LIMIT = 512;  // higher than previous 128 for more pressure

struct Op { bool is_alloc; size_t size; };          // size==0 for free
using Workload = std::vector<std::vector<Op>>;      // workload[tid]

// --------------------------------------------------
// Fixed-workload generator matching requested distribution
Workload generate_workload(uint32_t threads, uint32_t ops_per_thread, uint64_t seed) {
    Workload wl(threads);

    // 预先生成每个线程要用的“尺寸池”
    std::vector<std::vector<size_t>> sizes_pool(threads);
    {
        // 先算好每个 size-class 的数量
        uint32_t base    = ops_per_thread / RATIO_TOTAL;
        uint32_t cnt_lg  = base * RATIO_LG;
        uint32_t cnt_md  = base * RATIO_MD;
        uint32_t cnt_sm  = ops_per_thread - (cnt_md + cnt_lg); // 吸收余数

        std::uniform_int_distribution<size_t> dist_sm (16, MAX_SMALL_SZ);
        std::uniform_int_distribution<size_t> dist_md(MAX_SMALL_SZ+1, MAX_MEDIUM_SZ);
        std::uniform_int_distribution<size_t> dist_lg(MAX_MEDIUM_SZ+1, MAX_LARGE_SZ);

        for (uint32_t tid = 0; tid < threads; ++tid) {
            std::mt19937_64 rng(seed + tid);
            auto& pool = sizes_pool[tid];
            pool.reserve(ops_per_thread);

            auto push_rand = [&](auto& dist, uint32_t cnt, size_t step) {
                for (uint32_t i = 0; i < cnt; ++i) {
                    size_t raw = dist(rng);
                    pool.push_back((raw + step - 1) & ~(step - 1));
                }
            };
            push_rand(dist_sm, cnt_sm, STEP_SMALL);
            push_rand(dist_md, cnt_md, STEP_MEDIUM);
            push_rand(dist_lg, cnt_lg, STEP_LARGE);
            std::shuffle(pool.begin(), pool.end(), rng);
        }
    }

    // 生成 alloc/free 操作序列
    for (uint32_t tid = 0; tid < threads; ++tid) {
        wl[tid].reserve(ops_per_thread);
        auto& ops = wl[tid];
        auto& sizes = sizes_pool[tid];
        size_t size_idx    = 0;
        size_t outstanding = 0;
        std::mt19937_64 rng(seed + tid + 0x9e3779b97f4a7c15ULL);

        for (uint32_t i = 0; i < ops_per_thread; ++i) {
            bool do_alloc;
            if      (outstanding == 0)            do_alloc = true;
            else if (outstanding >= OUTSTANDING_LIMIT) do_alloc = false;
            else                                  do_alloc = (rng() & 1);

            if (do_alloc && size_idx < sizes.size()) {
                ops.push_back({true, sizes[size_idx++]});
                ++outstanding;
            } else {
                ops.push_back({false, 0});
                if (outstanding) --outstanding;
            }
        }
        // **不再追加剩余 sizes**，保证 ops.size() == ops_per_thread
    }

    return wl;
}

// --------------------------------------------------
// 全局内存计数（字节）
static std::atomic<uint64_t> g_curr_bytes{0}, g_peak_bytes{0};

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
};

struct ThreadStats {
    uint64_t attempted = 0;
    uint64_t alloc_success = 0, alloc_fail = 0;
    uint64_t free_success = 0, free_fail = 0;
    std::vector<double> alloc_lat, free_lat;
};

using Clock = std::chrono::high_resolution_clock;

template <typename AllocFn, typename FreeFn>
BenchmarkResult run_benchmark(const std::string& title,
                              AllocFn&& allocFn, FreeFn&& freeFn,
                              const Workload& workload,
                              uint32_t ops_per_thread) {
    uint32_t threads = static_cast<uint32_t>(workload.size());
    BenchmarkResult r{title, threads, ops_per_thread};

    // 重置全局计数
    g_curr_bytes.store(0);
    g_peak_bytes.store(0);

    std::vector<std::thread> workers;
    std::vector<ThreadStats> stats(threads);

    std::cout << "--- 开始单元测试: " << r.title << " ---\n";
    auto t_begin = Clock::now();
    for (uint32_t tid = 0; tid < threads; ++tid) {
        workers.emplace_back([&, tid]() {
            ThreadStats& s = stats[tid];
            s.alloc_lat.reserve(ops_per_thread/2);
            s.free_lat .reserve(ops_per_thread/2);
            std::vector<std::pair<void*, size_t>> blocks;
            blocks.reserve(OUTSTANDING_LIMIT);

            for (auto& op : workload[tid]) {
                if (op.is_alloc) {
                    auto t0 = Clock::now();
                    void* p = allocFn(op.size);
                    auto t1 = Clock::now();
                    double us = std::chrono::duration<double,std::micro>(t1-t0).count();
                    s.alloc_lat.push_back(us);
                    ++s.attempted;

                    if (p) {
                        ++s.alloc_success;
                        blocks.emplace_back(p, op.size);
                        // 全局内存 + 更新峰值
                        uint64_t new_bytes = g_curr_bytes.fetch_add(op.size) + op.size;
                        uint64_t prev_peak;
                        while (new_bytes > (prev_peak = g_peak_bytes.load()) &&
                               !g_peak_bytes.compare_exchange_weak(prev_peak, new_bytes)) {}
                    } else {
                        ++s.alloc_fail;
                    }
                } else {
                    ++s.attempted;
                    if (blocks.empty()) {
                        ++s.free_fail;
                    } else {
                        auto [ptr, sz] = blocks.back();
                        blocks.pop_back();
                        auto t0 = Clock::now();
                        freeFn(ptr, sz);
                        auto t1 = Clock::now();
                        double us = std::chrono::duration<double,std::micro>(t1-t0).count();
                        s.free_lat.push_back(us);
                        ++s.free_success;
                        g_curr_bytes.fetch_sub(sz);
                    }
                }
            }
            // 结束时释放尚未 free 的块（不统计）
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

    // 归总各线程统计
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
    r.succeeded_ops = r.alloc_success + r.free_success;

    auto calc = [](std::vector<double>& v, double& avg, double& p99) {
        if (v.empty()) { avg = p99 = 0; return; }
        avg = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
        auto idx = static_cast<size_t>(v.size() * 0.99);
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
              << std::left<<std::setw(24)<<"成功操作数:"  << std::right<<std::setw(10)<<r.succeeded_ops<<"\n"
              << std::left<<std::setw(24)<<"Ops/Sec:"     << std::right<<std::setw(10)
                   << fmt_f(r.attempted_ops/(r.total_time_ms/1000.0))<<"\n"
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
    auto opsps = [](const BenchmarkResult& r){ return r.attempted_ops/(r.total_time_ms/1000.0); };

    constexpr int w1=34, w2=20;
    std::cout<<"--- 对比: "<<a.title<<" vs "<<b.title<<" ---\n"
             <<std::left<<std::setw(w1)<<"指标"<<"| "<<std::setw(w2)<<a.title<<"| "<<std::setw(w2)<<b.title<<"|\n"
             <<std::string(w1+2+w2+2+w2+1,'-')<<"\n";

    auto line = [&](const char* name, const std::string& va, const std::string& vb){
        std::cout<<std::left<<std::setw(w1)<<name<<"| "<<std::setw(w2)<<va<<"| "<<std::setw(w2)<<vb<<"|\n";
    };
    line("Ops/Sec (越高越好)", fmt_f(opsps(a)), fmt_f(opsps(b)));
    line("Avg alloc (us)",     fmt_f(a.avg_alloc_us), fmt_f(b.avg_alloc_us));
    line("P99 alloc (us)",     fmt_f(a.p99_alloc_us), fmt_f(b.p99_alloc_us));
    line("Avg free  (us)",     fmt_f(a.avg_free_us), fmt_f(b.avg_free_us));
    line("P99 free  (us)",     fmt_f(a.p99_free_us), fmt_f(b.p99_free_us));
    std::cout<<std::string(w1+2+w2+2+w2+1,'-')<<"\n";
}

int main(int argc, char* argv[]) {
    uint32_t num_threads    = argc>1 ? std::stoul(argv[1]) : 12;
    uint32_t ops_per_thread = argc>2 ? std::stoul(argv[2]) : 200000;
    uint64_t seed           = argc>3 ? std::stoull(argv[3]) : 42ULL;

    auto workload = generate_workload(num_threads, ops_per_thread, seed);

    auto result_pool = run_benchmark("自定义内存池",
        [](size_t sz){ return memory_pool::MemoryPool::allocate(sz); },
        [](void* p, size_t sz){ memory_pool::MemoryPool::deallocate(p, sz); },
        workload, ops_per_thread);
    print_result(result_pool);

    auto result_malloc = run_benchmark("malloc/free",
        [](size_t sz){ return std::malloc(sz); },
        [](void* p, size_t){ std::free(p); },
        workload, ops_per_thread);
    print_result(result_malloc);

    print_comparison(result_pool, result_malloc);
    return 0;
}
