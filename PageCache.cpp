#include "PageCache.h"

namespace memory_pool
{

PageCache &PageCache::getInstance()
{
    static PageCache instance;
    return instance;
}

PageCache::~PageCache()
{
    std::lock_guard<std::mutex> lock(mutex_);
    // 释放所有 span（包括空闲和正在用的，startMap_ 包含了全量）（但是不包含SpanPool）
    for (auto &p : spanStartMap_)
    {
        Span *s = p.second;
        auto size = s->numPages * PAGE_SIZE;
        munmap(s->pageAddr, size);
        SpanPool::put(s);
    }
    // 清空所有容器
    freeSpans_.clear();
    spanStartMap_.clear();
    spanEndMap_.clear();
    // 删除池中所有 Span
    SpanPool::clear();
}

// 申请numPages个页的内存
void *PageCache::allocateSpan(std::size_t numPages)
{
    if (numPages == 0)
        return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);

    // 先在空闲链表中找 ≥numPages 的块
    auto it = freeSpans_.lower_bound(numPages);
    if (it != freeSpans_.end())
    {
        Span *span = it->second;
        detachFromFreeList(span); // 从 freeSpans_ 摘掉

        //  若太大则拆分尾巴
        if (span->numPages > numPages)
        {
            spanEndMap_.erase(endAddr(span)); // 原尾失效

            Span *tail = SpanPool::get();
            tail->pageAddr = static_cast<char *>(span->pageAddr) + numPages * PAGE_SIZE;
            tail->numPages = span->numPages - numPages;
            tail->next = nullptr;
            tail->prev = nullptr;

            pushToFreeList(tail); // 尾巴回收
            spanStartMap_[tail->pageAddr] = tail;
            spanEndMap_[endAddr(tail)] = tail;

            span->numPages = numPages; // 缩小主块
        }
        spanStartMap_[span->pageAddr] = span;
        spanEndMap_[endAddr(span)] = span; // 更新新尾
        return span->pageAddr;
    }

    // 未找到块，向 OS 申请
    void *mem = systemAlloc(numPages);
    if (!mem)
        return nullptr;

    Span* span = SpanPool::get();
    span->pageAddr = mem;
    span->numPages = numPages;
    span->prev = nullptr;
    span->next = nullptr;

    spanStartMap_[span->pageAddr] = span;
    spanEndMap_[endAddr(span)] = span;
    return mem;
}

// 释放
void PageCache::deallocateSpan(void *ptr)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = spanStartMap_.find(ptr);
    if (it == spanStartMap_.end())
        return; // 非本分配器内存

    Span *span = it->second;

    // 与右邻合并
    void *rightAddr = endAddr(span);
    auto rIt = spanStartMap_.find(rightAddr);
    if (rIt != spanStartMap_.end())
    {
        Span *rightSpan = rIt->second;
        if (detachFromFreeList(rightSpan))
        { // 只合并空闲块
            spanStartMap_.erase(rightSpan->pageAddr);
            spanEndMap_.erase(endAddr(rightSpan));

            spanEndMap_.erase(endAddr(span)); // old tail
            span->numPages += rightSpan->numPages;
            spanEndMap_[endAddr(span)] = span; // new tail
            SpanPool::put(rightSpan); // 被并入，删掉元数据
        }
    }

    // 与左邻合并
    auto lIt = spanEndMap_.find(ptr); // 左邻尾 == 我开头
    if (lIt != spanEndMap_.end())
    {
        Span *leftSpan = lIt->second;
        if (detachFromFreeList(leftSpan))
        {
            spanEndMap_.erase(endAddr(leftSpan));
            leftSpan->numPages += span->numPages;
            spanEndMap_[endAddr(leftSpan)] = leftSpan;

            spanStartMap_.erase(span->pageAddr);
            spanEndMap_.erase(endAddr(span));
            SpanPool::put(span); // 被并入，删掉元数据
            span = leftSpan; // 更新为合并后块
        }
    }

    // 插回空闲链表
    pushToFreeList(span);
}

// 尝试摘下目标 span（若在空闲链表）,失败（不在空闲链表）返回false
bool PageCache::detachFromFreeList(Span *s)
{
    auto listIt = freeSpans_.find(s->numPages);
    if (listIt == freeSpans_.end())
        return false;

    Span *&head = listIt->second;
    if (!head)
        return false;

    if (head == s) { // 头部
        head = s->next;
    } else if (s->prev) { // 非头部
        s->prev->next = s->next;
    } else { // 不存在
        return false;
    }

    if (s->next){
        s->next->prev = s->prev;
    }
    if (!head) freeSpans_.erase(listIt); // 链表清空
    s->next = s->prev = nullptr;
    return true;
}

// 头插入空闲链表
void PageCache::pushToFreeList(Span *s)
{
    Span*& head = freeSpans_[s->numPages];
    s->next = head;
    if (head) head->prev = s;
    head = s;
    s->prev = nullptr;
}

// 向 os 请求物理页
void *PageCache::systemAlloc(std::size_t numPages)
{
    std::size_t size = numPages * PAGE_SIZE;
    void *ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (ptr == MAP_FAILED) ? nullptr : ptr; // 匿名映射已按需清零
}

} // namespace memory_pool
