#include "ThreadCache.h"
#include "CentralCache.h"
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
        // 大对象直接从系统分配
        return malloc(size);
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
        free(ptr);
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
bool ThreadCache::shouldReturnToCentralCache(size_t index)
{
    // 设定阈值，例如：当自由链表的大小超过一定数量时
    size_t threshold = 256;
    return (freeListSize_[index] > threshold);
}

void *ThreadCache::fetchFromCentralCache(size_t index)
{
    // 从中心缓存批量获取内存
    void *start = CentralCache::getInstance().fetchRange(index);
    if (!start)
        return nullptr;

    // 取一个返回，其余放入自由链表
    void *result = start;
    freeList_[index] = *reinterpret_cast<void **>(start);

    // 更新自由链表大小
    size_t batchNum = 0;
    void *current = start; // 从start开始遍历

    // 计算从中心缓存获取的内存块数量
    while (current != nullptr)
    {
        batchNum++;
        current = *reinterpret_cast<void **>(current); // 遍历下一个内存块
    }

    // 更新freeListSize_，增加获取的内存块数量(有一块被返回了，要减1)
    freeListSize_[index] += batchNum - 1;

    return result;
}

void ThreadCache::returnToCentralCache(size_t index)
{
    size_t total = freeListSize_[index]; // 总共的自由块
    size_t keep = std::max(total / 4, size_t(1)); //保留 1/4
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