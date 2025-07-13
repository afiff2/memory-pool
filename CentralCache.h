#pragma once
#include "Common.h"
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <unordered_set>

namespace memory_pool
{

// 跟踪一段连续span的分配状态
class SpanTracker
{
    public:
    void * spanAddr = nullptr; // 这段 span 的起始地址
    size_t numPages = 0;       // 这段 span 包含多少页
    static constexpr size_t BLOCK_COUNT = 1024; // 每个 span 固定包含 1024 个块
    size_t freeCount = BLOCK_COUNT; // 实时记录空闲块数量
    static constexpr size_t BITMAP_WORDS = BLOCK_COUNT / 32;
    uint32_t bitmap[BITMAP_WORDS] = {}; // 每位标识一个块的使用情况，共 32*32 = 1024 位
    SpanTracker *next = nullptr;
    SpanTracker *prev = nullptr;

    // 标记第 idx 块为已分配
    inline void setAllocated(size_t idx) {
        size_t w = idx >> 5, b = idx & 31;
        // 只有从 0 -> 1 时才减少 freeCount
        if ((bitmap[w] & (1u << b)) == 0) {
            bitmap[w] |= (1u << b);
            --freeCount;
        }
    }
    // 标记第 idx 块为可用
    inline void setFree(size_t idx) {
        size_t w = idx >> 5, b = idx & 31;
        // 只有从 1 -> 0 时才增加 freeCount
        if (bitmap[w] & (1u << b)) {
            bitmap[w] &= ~(1u << b);
            ++freeCount;
        }
    }
    // 检查第 idx 块是否空闲
    inline bool isFree(size_t idx) const {
        return !(bitmap[idx >> 5] & (1u << (idx & 31)));
    }
    // O(1) 判断是否全空闲
    inline bool allFree() const {
        return freeCount == BLOCK_COUNT;
    }
    // O(1) 判断是否全已分配
    inline bool allAllocated() const {
        return freeCount == 0;
    }
    // 将所有块标记为已分配（全部改成 1）
    inline void allocateAll() {
        for (size_t i = 0; i < BITMAP_WORDS; ++i) {
            bitmap[i] = 0xFFFFFFFFu;
        }
        freeCount = 0;
    }
    // 将所有块标记为可用（全部改成 0）
    inline void freeAll() {
        for (size_t i = 0; i < BITMAP_WORDS; ++i) {
            bitmap[i] = 0;
        }
        freeCount = BLOCK_COUNT;
    }
    // 一次最多分配 maxBatch 块，计算地址并返回
    FetchResult allocateBatch(size_t maxBatch, size_t blockSize);
};

class CentralCache
{
  public:
    static CentralCache &getInstance();

    // 返回至多 maxBatch 个可用块列表
    FetchResult fetchRange(size_t index, size_t maxBatch);
    // 批量归还块
    void returnRange(void *start, size_t index);

    CentralCache(const CentralCache &) = delete;
    CentralCache &operator=(const CentralCache &) = delete;

  private:
    CentralCache();
    ~CentralCache();

    // 向 PageCache 申请一个 span
    SpanTracker * fetchFromPageCache(size_t index);
    // 按块地址获取对应的 SpanTracker
    SpanTracker *getSpanTracker(void *block, size_t index);

    void returnToPageCache(size_t index, SpanTracker* st);

    inline void pushFront(size_t index, SpanTracker* st);
    // head 是链表头的引用，st 是要摘除的节点
    inline void removeFromList(SpanTracker*& head, SpanTracker* st);

  private:
    struct alignas(64) SpinLock{
        std::atomic_flag flag = ATOMIC_FLAG_INIT;
    };

    std::array<SpanTracker *, NUM_CLASSES> centralFree_ = {};
    std::array<size_t, NUM_CLASSES> emptySpanCount_{}; //完全空闲的SpanTracker有几个
    std::array<SpinLock, NUM_CLASSES> locks_;

    // key：span 的起始地址（用 uintptr_t 存储以便比较）
    std::array<std::map<uintptr_t, SpanTracker*>, NUM_CLASSES> spanMap_{};

    // 微型spanTracker池,必须在index自旋锁内使用
    std::array<SpanTracker *, NUM_CLASSES> spanTrackerPools_;
    inline SpanTracker* getSpanTrackerFromPool(size_t index) {
        SpanTracker* node = spanTrackerPools_[index];
        if (node) {
            spanTrackerPools_[index] = node->next;
            node->next = nullptr;
            node->prev = nullptr;
            return node;
        }
        return new SpanTracker(); // fallback 到堆分配
    }

    inline void putSpanTrackerToPool(SpanTracker* tr, size_t index) {
        tr->next = spanTrackerPools_[index];
        tr->prev = nullptr;
        spanTrackerPools_[index] = tr;
    }
};

} // namespace memory_pool
