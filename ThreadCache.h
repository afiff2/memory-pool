#pragma once
#include "Common.h"

namespace memory_pool 
{

// 线程本地缓存
class ThreadCache
{
public:
    static ThreadCache* getInstance()
    {
        static thread_local ThreadCache instance; // Thread-local Singleton
        return &instance;
    }

    void* allocate(size_t size);
    void deallocate(void* ptr, size_t size);
private:
    ThreadCache() // 防止外部误初始化
    {
        // 初始化空闲链表和大小统计
        freeList_.fill(nullptr);
        freeListSize_.fill(0);
    }
    
    // 从中心缓存获取内存
    void* fetchFromCentralCache(size_t index);
    // 归还内存到中心缓存
    void returnToCentralCache(size_t index);

    bool shouldReturnToCentralCache(size_t index);
private:
    // 每个线程的空闲链表数组
    std::array<void*, FREE_LIST_SIZE>  freeList_; 
    std::array<size_t, FREE_LIST_SIZE> freeListSize_; // 空闲链表大小统计   
};

} // namespace memory_pool