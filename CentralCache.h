#pragma once
#include "Common.h"
#include <array>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

namespace memory_pool
{

// 跟踪一段连续span的分配状态
struct SpanTracker
{
    std::atomic<void *> spanAddr{nullptr}; // 这段 span 的起始地址
    std::atomic<size_t> numPages{0};       // 这段 span 包含多少页
    std::atomic<size_t> blockCount{0};     // 这段 span 被划分成了多少个内存块
};

class CentralCache
{
  public:
    static CentralCache &getInstance();

    FetchResult fetchRange(size_t index, size_t maxBatch);

    void returnRange(void *start, size_t index);

    CentralCache(const CentralCache &) = delete;
    CentralCache &operator=(const CentralCache &) = delete;

  private:
    CentralCache();
    ~CentralCache();

    FetchResult fetchFromPageCache(size_t index);

    SpanTracker *getSpanTracker(void *block, size_t index);
    void updateSpanFreeCount(SpanTracker *tracker, size_t freeCount, size_t index);

    bool shouldDelayReturn(size_t index, size_t currentCnt, std::chrono::steady_clock::time_point now) const;

    void performDelayedReturn(size_t index);

  private:
    static constexpr size_t SPAN_PAGES = 8;                          // 最少申请的页数
    static constexpr size_t MAX_DELAY_COUNT = 48;                    // 延迟归还的最大块数
    static constexpr std::chrono::milliseconds DELAY_INTERVAL{1000}; // 延迟归还的时间间隔

    // 将变量对齐到 64 字节边界, 避免多线程下伪共享造成的性能下降
    struct alignas(64) FreeListHead{
        std::atomic<void *> head{nullptr};
    };
    struct alignas(64) SpinLock{
        std::atomic_flag flag = ATOMIC_FLAG_INIT;
    };
    struct alignas(64) DelayCounter{
        std::atomic<size_t> cnt{0};
    };

    std::array<FreeListHead, NUM_CLASSES> centralFree_;
    std::array<SpinLock, NUM_CLASSES> locks_;
    std::array<DelayCounter, NUM_CLASSES> delay_;
    std::array<std::chrono::steady_clock::time_point, NUM_CLASSES> lastReturn_;

    std::array<std::unordered_map<void*, SpanTracker*>, NUM_CLASSES> spanPageMap_{};//记录页对应的SpanTracker(加锁对index执行，所以有index项)
};

} // namespace memory_pool
