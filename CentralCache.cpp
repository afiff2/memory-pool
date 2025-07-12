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

CentralCache &CentralCache::getInstance()
{
    static CentralCache g;
    return g;
}

CentralCache::CentralCache()
{
    auto now = std::chrono::steady_clock::now();
    for (auto &t : lastReturn_)
        t = now;
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
    if (index >= NUM_CLASSES) //超过上限，错误申请
        return {nullptr, 0};

    SpinGuard guard{locks_[index].flag};

    // 如果 free 列表空，先从页缓存拉出一整条链
    void* list = centralFree_[index].head.load(std::memory_order_relaxed);
    if (!list) {
        auto refill = fetchFromPageCache(index);
        if (!refill.head)
            return {nullptr, 0};
        list = refill.head;
    }
    // 从 list 上 detach 最多 maxBatch 个块
    void* cur  = list;
    void* prev = nullptr;
    size_t cnt = 0;
    while (cur && cnt < maxBatch) {
        prev = cur;
        cur  = *reinterpret_cast<void**>(cur);
        ++cnt;
    }
    FetchResult res{ list, cnt };

    // 把剩下的块存回 centralFree_
    centralFree_[index].head.store(cur, std::memory_order_relaxed);
    // 断开前面这 cnt 块
    if (prev)
        *reinterpret_cast<void**>(prev) = nullptr; 

    return res;
}

//传入的块链不一定属于同一个页集
void CentralCache::returnRange(void *start, size_t index)
{
    if (!start || index >= NUM_CLASSES) //错误请求
        return;

    SpinGuard guard{locks_[index].flag};

    // 到最后一个块
    void *tail = start;
    while (*reinterpret_cast<void **>(tail))
        tail = *reinterpret_cast<void **>(tail);
    //插入空闲头部
    *reinterpret_cast<void **>(tail) = centralFree_[index].head.load(std::memory_order_relaxed);
    centralFree_[index].head.store(start, std::memory_order_relaxed);

    // 更新延迟计数
    size_t curCnt = delay_[index].cnt.fetch_add(1, std::memory_order_relaxed) + 1;
    auto now = std::chrono::steady_clock::now();

    // 检查是否需要执行延迟归还
    if (shouldDelayReturn(index, curCnt, now))
        performDelayedReturn(index);
}


SpanTracker *CentralCache::getSpanTracker(void *block, size_t index)
{
    // 把任意地址 addr 变成它所在页的起始地址 pageBase
    uintptr_t addr = reinterpret_cast<uintptr_t>(block);
    uintptr_t pageMask = ~(PageCache::PAGE_SIZE - 1ULL);
    void *pageBase = reinterpret_cast<void *>(addr & pageMask);

    auto &map = spanPageMap_[index];
    auto it = map.find(pageBase);
    return it == map.end() ? nullptr : it->second;
}

//该index太久没归还 or 接受了太多的ThreadCache的归还
bool CentralCache::shouldDelayReturn(size_t index, size_t curCnt, std::chrono::steady_clock::time_point now) const
{
    return curCnt >= MAX_DELAY_COUNT || (now - lastReturn_[index]) >= DELAY_INTERVAL;
}

void CentralCache::performDelayedReturn(size_t index)
{
    delay_[index].cnt.store(0, std::memory_order_relaxed);
    lastReturn_[index] = std::chrono::steady_clock::now();

    // 统计每一页集的空闲块数
    std::unordered_map<SpanTracker *, size_t> counter;
    void *blk = centralFree_[index].head.load(std::memory_order_relaxed);
    while (blk)
    {
        if (auto *tr = getSpanTracker(blk, index))
            ++counter[tr];
        blk = *reinterpret_cast<void **>(blk);
    }

    //更新每个span的空闲计数并检查是否可以归还
    for (auto [tr, freeCount] : counter)
        updateSpanFreeCount(tr, freeCount, index);
}

void CentralCache::updateSpanFreeCount(SpanTracker *tr, size_t freeCount, size_t index)
{
    if (!tr)
        return;
    if (freeCount != tr->blockCount) //不是所有的块都空闲，暂时不归还
        return;

    // 所有的块都空闲
    void *spanBase = tr->spanAddr; //启示地址
    size_t pages = tr->numPages; //页数

    // 从空闲链中移除
    void *prev = nullptr;
    void *cur = centralFree_[index].head.load(std::memory_order_relaxed);
    while (cur)
    {
        void *nxt = *reinterpret_cast<void **>(cur);
        if (cur >= spanBase && cur < static_cast<char *>(spanBase) + pages * PageCache::PAGE_SIZE) // cur 这个块属于待回收的 span, 需要从链表中删除
        {
            if (prev)
                *reinterpret_cast<void **>(prev) = nxt; // 把上一节点的 next 指向 nxt
            else
                centralFree_[index].head.store(nxt, std::memory_order_relaxed); // 删除头节点
        }
        else
        {
            prev = cur;
        }
        cur = nxt;
    }

    // 释放对应的哈希表
    for (size_t p = 0; p < pages; ++p)
        spanPageMap_[index].erase(static_cast<char *>(spanBase) + p * PageCache::PAGE_SIZE);

    // 释放对应的SpanTracker
    putSpanTrackerToPool(tr, index);

    PageCache::getInstance().deallocateSpan(spanBase);
}

FetchResult CentralCache::fetchFromPageCache(size_t index)
{
    const size_t blkSize = SizeClass::getSize(index); // 需要的块的大小

    // 计算需要的页数
    size_t pages = (blkSize + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;
    if (pages < SPAN_PAGES)
        pages = SPAN_PAGES;

    void *span = PageCache::getInstance().allocateSpan(pages);
    if (!span)
        return {nullptr, 0}; // 申请失败

    const size_t blkNum = (pages * PageCache::PAGE_SIZE) / blkSize; //拿到的块数（有内碎片）

    // 切割成blocks
    char *base = static_cast<char *>(span);
    for (size_t i = 1; i < blkNum; ++i)
        *reinterpret_cast<void **>(base + (i - 1) * blkSize) = base + i * blkSize;
    *reinterpret_cast<void **>(base + (blkNum - 1) * blkSize) = nullptr;

    // 添加一个新的tracker
    SpanTracker* tr = getSpanTrackerFromPool(index);
    tr->spanAddr = span;
    tr->numPages = pages;
    tr->blockCount = blkNum;

    for (size_t p = 0; p < pages; ++p)
        spanPageMap_[index][base + p * PageCache::PAGE_SIZE] = tr;//记录每一页的SpanTracker

    return { base, blkNum }; // first block to caller
}

} // namespace memory_pool
