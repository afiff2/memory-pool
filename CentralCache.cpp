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

CentralCache &CentralCache::instance()
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

void *CentralCache::fetchRange(size_t index)
{
    if (index >= FREE_LIST_SIZE) //超过上限，错误申请
        return nullptr;
    SpinGuard guard{locks_[index].flag};

    void *blk = centralFree_[index].head.load(std::memory_order_relaxed);
    if (!blk)
    { // 没有中心缓存，申请页缓存
        blk = fetchFromPageCache(index);
        return blk;
    }

    // 与链表断开
    void *next = *reinterpret_cast<void **>(blk);
    centralFree_[index].head.store(next, std::memory_order_relaxed);
    *reinterpret_cast<void **>(blk) = nullptr;

    // 查找并更新SpanTracker
    if (auto *tr = getSpanTracker(blk, index))
        tr->freeCount.fetch_sub(1, std::memory_order_relaxed);

    return blk;
}

void CentralCache::returnRange(void *start, size_t size, size_t index)
{
    if (!start || index >= FREE_LIST_SIZE) //错误请求
        return;

    const size_t blkSize = (index + 1) * ALIGNMENT; //块的大小
    const size_t blkCount = size / blkSize; //块数

    SpinGuard guard{locks_[index].flag};

    // 到最后一个块
    void *tail = start;
    for (size_t i = 1; i < blkCount && *reinterpret_cast<void **>(tail); ++i)
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

bool CentralCache::shouldDelayReturn(size_t index, size_t curCnt, std::chrono::steady_clock::time_point now) const
{
    return curCnt >= MAX_DELAY_COUNT || (now - lastReturn_[index]) >= DELAY_INTERVAL;
}

void CentralCache::performDelayedReturn(size_t index)
{
    delay_[index].cnt.store(0, std::memory_order_relaxed);
    lastReturn_[index] = std::chrono::steady_clock::now();

    // 统计每一页的空闲块数
    std::unordered_map<SpanTracker *, size_t> counter;
    void *blk = centralFree_[index].head.load(std::memory_order_relaxed);
    while (blk)
    {
        if (auto *tr = getSpanTracker(blk, index))
            ++counter[tr];
        blk = *reinterpret_cast<void **>(blk);
    }

    //更新每个span的空闲计数并检查是否可以归还
    for (auto [tr, add] : counter)
        updateSpanFreeCount(tr, add, index);
}

void CentralCache::updateSpanFreeCount(SpanTracker *tr, size_t add, size_t index)
{
    if (!tr)
        return;
    size_t newFree = tr->freeCount.fetch_add(add, std::memory_order_relaxed) + add; //更新空闲块的数量
    if (newFree != tr->blockCount.load(std::memory_order_relaxed)) //不是所有的块都空闲，暂时不归还
        return;

    // 所有的块都空闲
    void *spanBase = tr->spanAddr.load(std::memory_order_relaxed); //启示地址
    size_t pages = tr->numPages.load(std::memory_order_relaxed); //页数

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

    PageCache::getInstance().deallocateSpan(spanBase);
}

void *CentralCache::fetchFromPageCache(size_t index)
{
    const size_t blkSize = (index + 1) * ALIGNMENT; // 需要的块的大小

    // 计算需要的页数
    size_t pages = (blkSize + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;
    if (pages < SPAN_PAGES)
        pages = SPAN_PAGES;

    void *span = PageCache::getInstance().allocateSpan(pages);
    if (!span)
        return nullptr; // 申请失败

    const size_t blkNum = (pages * PageCache::PAGE_SIZE) / blkSize; //拿到的块数（有内碎片）

    // 切割成blocks
    char *base = static_cast<char *>(span);
    for (size_t i = 1; i < blkNum; ++i)
        *reinterpret_cast<void **>(base + (i - 1) * blkSize) = base + i * blkSize;
    *reinterpret_cast<void **>(base + (blkNum - 1) * blkSize) = nullptr;

    centralFree_[index].head.store(base + blkSize, std::memory_order_relaxed); //挂载第二个块

    // 添加一个新的tracker
    size_t trIdx = spanCount_.fetch_add(1, std::memory_order_acq_rel); //确保对其他线程可见
    assert(trIdx < spanTrackers_.size() && "SpanTracker overflow – increase capacity");

    SpanTracker &tr = spanTrackers_[trIdx];
    tr.spanAddr.store(span, std::memory_order_relaxed);
    tr.numPages.store(pages, std::memory_order_relaxed);
    tr.blockCount.store(blkNum, std::memory_order_relaxed);
    tr.freeCount.store(blkNum - 1, std::memory_order_relaxed);

    for (size_t p = 0; p < pages; ++p)
        spanPageMap_[index][base + p * PageCache::PAGE_SIZE] = &tr;//记录每一页的SpanTracker

    return base; // first block to caller
}

} // namespace memory_pool
