#pragma once
#include "Common.h"
#include "PageCache.h"
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace memory_pool
{

// 跟踪一段连续span的分配状态
class alignas(64) SpanTracker // 注：SpanTracker > 64字节
{
  public:
    void *spanAddr = nullptr;                   // 这段 span 的起始地址
    size_t numPages = 0;                        // 这段 span 包含多少页
    static constexpr size_t BLOCK_COUNT = 1024; // 每个 span 固定包含 1024 个块
    size_t freeCount = BLOCK_COUNT;             // 实时记录空闲块数量
    static constexpr size_t BITMAP_WORDS = BLOCK_COUNT / 32;
    uint32_t bitmap[BITMAP_WORDS] = {}; // 每位标识一个块的使用情况，共 32*32 = 1024 位
    SpanTracker *next = nullptr;
    SpanTracker *prev = nullptr;

    // 标记第 idx 块为已分配
    inline void setAllocated(size_t idx)
    {
        size_t w = idx >> 5, b = idx & 31;
        // 只有从 0 -> 1 时才减少 freeCount
        if ((bitmap[w] & (1u << b)) == 0)
        {
            bitmap[w] |= (1u << b);
            --freeCount;
        }
    }
    // 标记第 idx 块为可用
    inline void setFree(size_t idx)
    {
        size_t w = idx >> 5, b = idx & 31;
        // 只有从 1 -> 0 时才增加 freeCount
        if (bitmap[w] & (1u << b))
        {
            bitmap[w] &= ~(1u << b);
            ++freeCount;
        }
    }
    // 检查第 idx 块是否空闲
    inline bool isFree(size_t idx) const { return !(bitmap[idx >> 5] & (1u << (idx & 31))); }
    // O(1) 判断是否全空闲
    inline bool allFree() const { return freeCount == BLOCK_COUNT; }
    // O(1) 判断是否全已分配
    inline bool allAllocated() const { return freeCount == 0; }
    // 将所有块标记为已分配（全部改成 1）
    inline void allocateAll()
    {
        for (size_t i = 0; i < BITMAP_WORDS; ++i)
        {
            bitmap[i] = 0xFFFFFFFFu;
        }
        freeCount = 0;
    }
    // 将所有块标记为可用（全部改成 0）
    inline void freeAll()
    {
        for (size_t i = 0; i < BITMAP_WORDS; ++i)
        {
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
    SpanTracker *fetchFromPageCache(size_t index);
    // 按块地址获取对应的 SpanTracker
    SpanTracker *getSpanTracker(void *block, size_t index);

    void returnToPageCache(size_t index, SpanTracker *st);

    inline void pushFront(size_t index, SpanTracker *st);
    // head 是链表头的引用，st 是要摘除的节点
    inline void removeFromList(SpanTracker *&head, SpanTracker *st);

  private:
    // 单个 index 最多保留 4 MB
    static constexpr size_t kMaxBytesPerIndex = 4 * 1024 * 1024; // 4 MB

    struct alignas(64) CentralClass
    {
        SpanTracker *freeList = nullptr;          // 原 centralFree_[i]
        size_t emptyCount = 0;                    // 原 emptySpanCount_[i]
        std::atomic_flag lock = ATOMIC_FLAG_INIT; // 已经是 64 字节对齐
    };
    static_assert(sizeof(CentralClass) == 64, "CentralClass must be exactly 64 bytes to avoid false sharing.");

    std::array<CentralClass, NUM_CLASSES> central_;

    template <typename Map> struct alignas(64) AlignedMap
    {
        Map m;
    };

    // 记录页对应的SpanTracker(加锁对index执行，所以有index项)
    std::array<AlignedMap<std::unordered_map<void *, SpanTracker *>>, CLS_MEDIUM> spanPageMap_;
    std::array<AlignedMap<std::map<uintptr_t, SpanTracker *>>, NUM_CLASSES - CLS_MEDIUM> spanMap_;

    // 微型spanTracker池,必须在index自旋锁内使用
    class SpanTrackerPool
    {
      private:
        struct SpanPage
        {
            SpanPage *next;
            // 后面紧跟若干 SpanTracker 对象
        };
        static constexpr size_t HeaderSize = 64;
        static_assert(HeaderSize >= sizeof(SpanPage), "HeaderSize must cover SpanPage metadata");
        static_assert(PageCache::PAGE_SIZE > HeaderSize + sizeof(SpanTracker),
                      "Page too small to hold any SpanTracker");

        static inline std::array<SpanPage *, NUM_CLASSES> pageList_{};
        static inline std::array<SpanTracker *, NUM_CLASSES> freeList_{};

        static void allocateNewPage(size_t idx)
        {
            // 从 PageCache 取 1 页
            void *raw = PageCache::getInstance().allocateSpan(1);
            if (!raw)
                throw std::bad_alloc();
            auto *page = static_cast<SpanPage *>(raw);
            page->next = pageList_[idx];
            pageList_[idx] = page;

            // 划分 slot
            constexpr size_t ObjSize = sizeof(SpanTracker);
            size_t count = (PageCache::PAGE_SIZE - HeaderSize) / ObjSize;
            char *base = reinterpret_cast<char *>(page) + HeaderSize;
            for (size_t i = 0; i < count; ++i)
            {
                auto *slot = reinterpret_cast<SpanTracker *>(base + i * ObjSize);
                // new (slot) SpanTracker(); // placement new
                slot->next = freeList_[idx];
                freeList_[idx] = slot;
            }
        }

      public:
        static SpanTracker *get(size_t idx)
        {
            if (!freeList_[idx])
                allocateNewPage(idx);
            auto *r = freeList_[idx];
            freeList_[idx] = r->next;
            return r;
        }

        static void put(SpanTracker *t, size_t idx)
        {
            if (!t)
                return;
            // t->~SpanTracker();
            t->next = freeList_[idx];
            freeList_[idx] = t;
        }
    };
};

} // namespace memory_pool
