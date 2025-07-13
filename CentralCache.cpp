#include "CentralCache.h"
#include "PageCache.h"

#include <cassert>
#include <cstdint>
#include <thread>
#include <unordered_map>

namespace memory_pool
{

// RAII 自旋锁
class SpinGuard
{
  public:
    explicit SpinGuard(std::atomic_flag &f) : flag_(f)
    {
        while (flag_.test_and_set(std::memory_order_acquire))
            std::this_thread::yield(); // 线程让步
    }
    ~SpinGuard() { flag_.clear(std::memory_order_release); }

    SpinGuard(const SpinGuard &) = delete;
    SpinGuard &operator=(const SpinGuard &) = delete;

  private:
    std::atomic_flag &flag_;
};

FetchResult SpanTracker::allocateBatch(size_t maxBatch, size_t blockSize) {
    void* head = nullptr;
    void** tailPtr = &head; // 始终指向上一个节点的 next 指针
    size_t got = 0;
    size_t toGrab = std::min(maxBatch, freeCount);
    // 从头扫描所有块
    for (size_t idx = 0; idx < BLOCK_COUNT && got < toGrab; ++idx) {
        if (isFree(idx)) {
            // 标记已分配
            setAllocated(idx);
            void* blk = static_cast<char*>(spanAddr) + idx * blockSize;
            *tailPtr = blk;
            tailPtr = reinterpret_cast<void**>(blk);
            ++got;
        }
    }
    *tailPtr = nullptr;
    return { head, got };
}

CentralCache &CentralCache::getInstance()
{
    static CentralCache g;
    return g;
}

CentralCache::CentralCache()
{
    for (size_t idx = 0; idx < CLS_MEDIUM; ++idx) 
    {
        // 预计这个 size class 下，最多的记录
        size_t maxPages = kMaxBytesPerIndex * 2 / PageCache::PAGE_SIZE;
        // 降低负载因子到 0.5（可选，越小冲突越少，但内存越多）
        spanPageMap_[idx].max_load_factor(0.5f);
        // 分配足够多的桶
        spanPageMap_[idx].rehash(maxPages * 2);
    }
}

//不用还页，上层会全部释放
CentralCache::~CentralCache() {
    // 收集所有唯一的 SpanTracker 指针
    std::unordered_set<SpanTracker*> uniqueTrackers;
    for (auto &pageMap : spanPageMap_) {
        for (auto &entry : pageMap) {
            uniqueTrackers.insert(entry.second);
        }
    }
    for (auto &mp : spanMap_) {
        for (auto &entry : mp) {
            uniqueTrackers.insert(entry.second);
        }
    }

    // 删除所有 SpanTracker
    for (auto *tr : uniqueTrackers) {
        delete tr;
    }

    //清理池子
    for (size_t i = 0; i < NUM_CLASSES; ++i) {
        SpanTracker* cur = spanTrackerPools_[i];
        while (cur) {
            SpanTracker* next = cur->next;
            delete cur;
            cur = next;
        }
    }
}

FetchResult CentralCache::fetchRange(size_t index, size_t maxBatch)
{
    if (index >= NUM_CLASSES || maxBatch == 0) //超过上限，错误申请
        return {nullptr, 0};

    SpinGuard guard{locks_[index].flag};

    // 确保 Free 列表非空，不空才去取
    if (centralFree_[index] == nullptr) {
        SpanTracker* st = fetchFromPageCache(index);
        if (!st) return {nullptr, 0};
        // 把新 span 挂到列表头
        pushFront(index, st);
        //完全空闲计数
        ++emptySpanCount_[index]; 
    }

    // 从列表头取
    SpanTracker* st = centralFree_[index];
    bool wasEmpty = st->allFree();
    const size_t blkSize = SizeClass::getSize(index); // 块的大小
    auto result = st->allocateBatch(maxBatch, blkSize);

    if (wasEmpty && result.count > 0) {
        --emptySpanCount_[index]; // 完全空闲计数
    }

    // 如果这个 span 全分配完了，就把它从列表里摘掉
    if (st->allAllocated()) {
        removeFromList(centralFree_[index], st);
    }

    return result; // 编译器将通过 RVO 或移动语义避免拷贝
}

SpanTracker * CentralCache::fetchFromPageCache(size_t index)
{
    const size_t blkSize = SizeClass::getSize(index); // 需要的块的大小
    const size_t blkNum = SpanTracker::BLOCK_COUNT;

    // 计算需要的页数
    size_t pages = (blkSize * SpanTracker::BLOCK_COUNT + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

    void *span = PageCache::getInstance().allocateSpan(pages);
    if (!span)
        return nullptr; // 申请失败

    // 添加一个新的tracker
    SpanTracker* tr = getSpanTrackerFromPool(index);
    tr->spanAddr = span;
    tr->numPages = pages;
    tr->freeAll();
    tr->next = nullptr;

    if(index < CLS_MEDIUM)
    {
        for (size_t p = 0; p < pages; ++p)
            spanPageMap_[index][static_cast<char*>(span) + p * PageCache::PAGE_SIZE] = tr;//记录每一页的SpanTracker
    }
    else
    {
        uintptr_t base = reinterpret_cast<uintptr_t>(span);
        spanMap_[index-CLS_MEDIUM][base] = tr;
    }
    
    return tr;
}

//传入的块链不一定属于同一个页集
void CentralCache::returnRange(void *start, size_t index)
{
    if (start == nullptr || index >= NUM_CLASSES) //错误请求
        return;

    const size_t blkSize = SizeClass::getSize(index);
    const size_t spanBytes    = blkSize * SpanTracker::BLOCK_COUNT;

    // 计算：为了不超过 kMaxBytesPerIndex(64MB)，最多保留多少个 span
    size_t maxEmptySpans = (kMaxBytesPerIndex + spanBytes - 1) / spanBytes; // 向上取整
    if (maxEmptySpans < 1) maxEmptySpans = 1;           // 最少留 1 个


    SpinGuard guard{locks_[index].flag};

    void* p = start;
    while (p != nullptr) {
        void* next = *reinterpret_cast<void**>(p);

        // 找到这个块对应的 SpanTracker
        SpanTracker* st = getSpanTracker(p, index);
        assert(st && "找不到对应的 SpanTracker");

        // 计算这个块在 span 中的索引
        uintptr_t offset = reinterpret_cast<uintptr_t>(p)
                         - reinterpret_cast<uintptr_t>(st->spanAddr);
        size_t blkIdx = offset / blkSize;

        bool wasFull = st->allAllocated();
        bool wasEmpty = st->allFree();

        // 标记该块空闲
        st->setFree(blkIdx);

        //之前是满分配，重新插入队列
        if (wasFull)
            pushFront(index, st);

        if(!wasEmpty && st->allFree()){
            ++emptySpanCount_[index];
            //如果有10个完全空闲的SpanTracker，就释放一个
            if(emptySpanCount_[index] > maxEmptySpans) //魔数字
                returnToPageCache(index, st);
        }

        p = next;
    }
}

void CentralCache::returnToPageCache(size_t index, SpanTracker* st)
{
    //减去完全空闲的计数
    emptySpanCount_[index]--;

    //从链表中摘除
    removeFromList(centralFree_[index], st);

    void *spanBase = st->spanAddr;
    size_t pages = st->numPages;

    
    if(index < CLS_MEDIUM)
    {
        // 释放对应的哈希表
        for (size_t p = 0; p < pages; ++p)
            spanPageMap_[index].erase(static_cast<char *>(spanBase) + p * PageCache::PAGE_SIZE);
    }
    else
    {
        uintptr_t base = reinterpret_cast<uintptr_t>(st->spanAddr);
        spanMap_[index - CLS_MEDIUM].erase(base);
    }



    // 释放对应的SpanTracker
    putSpanTrackerToPool(st, index);

    PageCache::getInstance().deallocateSpan(spanBase);
}


SpanTracker *CentralCache::getSpanTracker(void *block, size_t index)
{
    if(index < CLS_MEDIUM)
    {
        // 把任意地址 addr 变成它所在页的起始地址 pageBase
        uintptr_t addr = reinterpret_cast<uintptr_t>(block);
        uintptr_t pageMask = ~(PageCache::PAGE_SIZE - 1ULL);
        void *pageBase = reinterpret_cast<void *>(addr & pageMask);

        auto &map = spanPageMap_[index];
        auto it = map.find(pageBase);
        return it == map.end() ? nullptr : it->second;
    }
    else
    {
        uintptr_t addr = reinterpret_cast<uintptr_t>(block);
        auto &m = spanMap_[index-CLS_MEDIUM];
        // upper_bound 找到第一个 key > addr
        auto it = m.upper_bound(addr);
        if (it == m.begin())
            return nullptr;    // 全部 key 都 > addr，找不到
        --it; // 现在 it->first <= addr

        SpanTracker *st = it->second;
        return st;
    }
}

inline void CentralCache::pushFront(size_t index, SpanTracker* st) {
    SpanTracker* oldHead = centralFree_[index];
    st->prev = nullptr;
    st->next = oldHead;
    if (oldHead) {
        oldHead->prev = st;
    }
    centralFree_[index] = st;
}

inline void CentralCache::removeFromList(SpanTracker*& head, SpanTracker* st) {
        SpanTracker* prev = st->prev;
        SpanTracker* next = st->next;

        if (prev) {
            prev->next = next;
        } else {
            // st 是头节点
            head = next;
        }

        if (next) {
            next->prev = prev;
        }

        // 清理 st 自己的指针
        st->prev = st->next = nullptr;
    }

} // namespace memory_pool
