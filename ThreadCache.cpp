#include "ThreadCache.h"
#include "CentralCache.h"
#include "PageCache.h"
#include <stdlib.h>

namespace memory_pool
{

void *ThreadCache::allocate(size_t size)
{
    if (size <= 0)
    {
        size = ALIGNMENT; // 至少分配一个对齐大小
    }

    if (size > MAX_BYTES)
    {
        // 大对象直接跳级
        size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;
        return PageCache::getInstance().allocateSpan(numPages);
    }

    size_t index = SizeClass::getIndex(size);

    // 检查线程本地自由链表
    // 如果 freeList_[index] 不为空，表示该链表中有可用内存块
    if (void *ptr = freeList_[index])
    {
        // 将freeList_[index]指向的内存块的下一个内存块地址（取决于内存块的实现）
        freeList_[index] = *reinterpret_cast<void **>(ptr);
        // 更新对应自由链表的长度计数
        freeListSize_[index]--;
        return ptr;
    }

    // 如果线程本地自由链表为空，则从中心缓存获取一批内存
    return fetchFromCentralCache(index);
}

void ThreadCache::deallocate(void *ptr, size_t size)
{
    if (!ptr)
        return;
    if (size > MAX_BYTES)
    {
        PageCache::getInstance().deallocateSpan(ptr);
        return;
    }

    size_t index = SizeClass::getIndex(size);

    // 插入到线程本地自由链表
    *reinterpret_cast<void **>(ptr) = freeList_[index];
    freeList_[index] = ptr;

    // 更新对应自由链表的长度计数
    freeListSize_[index]++;

    // 判断是否需要将部分内存回收给中心缓存
    if (shouldReturnToCentralCache(index))
    {
        returnToCentralCache(index);
    }
}

// 判断是否需要将内存回收给中心缓存
bool ThreadCache::shouldReturnToCentralCache(size_t index) {
    size_t blockSize = SizeClass::getSize(index);
    constexpr size_t kMaxBytesPerIndex = 256 * 1024;  // 256 KB 上限

    // 如果本地空闲链表总字节数超过 256 KB，就返还给 CentralCache
    return freeListSize_[index] * blockSize > kMaxBytesPerIndex;
}

void *ThreadCache::fetchFromCentralCache(size_t index)
{
    size_t blockSize = SizeClass::getSize(index);

    // 根据大小选不同的批量抓取数
    size_t batchNum;
    if (blockSize <= MAX_SMALL_SZ) {
        batchNum = 64;      // ≤512B：一次取 64 块
    }
    else if (blockSize <= MAX_MEDIUM_SZ) {
        batchNum = 32;      // 513B–4KB：一次取 32 块
    }
    else if (blockSize <= MAX_LARGE_SZ) {
        batchNum = 16;      // 4KB–64KB：一次取 16 块
    }
    else {
        batchNum = 4;       // >64KB：一次取 4 块
    }

    // 从中心缓存批量获取内存
    auto res = CentralCache::getInstance().fetchRange(index, batchNum);
    if (!res.head) return nullptr;

    // 取一个返回，其余放入自由链表
    freeList_[index] = *reinterpret_cast<void**>(res.head);

    // 更新freeListSize_，增加获取的内存块数量(有一块被返回了，要减1)
    freeListSize_[index] += (res.count - 1);

    return res.head;
}

void ThreadCache::returnToCentralCache(size_t index)
{
    size_t total = freeListSize_[index]; // 总共的自由块
    size_t keep = std::max(total / 2, size_t(1)); //保留 1/2
    // 找到第 keep 个节点
    void* cur = freeList_[index];
    for (size_t i = 1; i < keep; ++i) {
        cur = *reinterpret_cast<void**>(cur);
    }
    void* retHead = *reinterpret_cast<void**>(cur);
    *reinterpret_cast<void**>(cur) = nullptr;    // 截断链表
    freeListSize_[index] = keep;
    CentralCache::getInstance().returnRange(retHead, index);
}

} // namespace memory_pool