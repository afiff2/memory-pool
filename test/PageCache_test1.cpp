#include <iostream>
#include <cassert>
#include <cstdint>
#include "../PageCache.h"

int main() {
    using namespace memory_pool;

    PageCache& cache = PageCache::getInstance();

    // 1) 申请一个 5 页的块，释放后再申请 2 页 + 3 页，应发生拆分
    void* p1 = cache.allocateSpan(5);
    assert(p1 != nullptr);
    std::cout << "Allocated 5 pages at " << p1 << std::endl;

    cache.deallocateSpan(p1);
    std::cout << "Deallocated 5-page block\n";

    // 申请 2 页，应该拿到原来的 p1
    void* p2 = cache.allocateSpan(2);
    assert(p2 == p1);
    std::cout << "Allocated 2 pages at " << p2 << std::endl;

    // 紧接着申请 3 页，应该拿到 p1+2*PAGE_SIZE
    void* p3 = cache.allocateSpan(3);
    std::uintptr_t addr2 = reinterpret_cast<std::uintptr_t>(p2);
    std::uintptr_t addr3 = reinterpret_cast<std::uintptr_t>(p3);
    assert(addr3 == addr2 + 2 * PageCache::PAGE_SIZE);
    std::cout << "Allocated 3 pages at " << p3 << " (adjacent)\n";

    // 释放 2 页、3 页，测试能否合并回一个 5 页块
    cache.deallocateSpan(p2);
    cache.deallocateSpan(p3);
    std::cout << "Deallocated 2-page and 3-page blocks\n";

    // 再次申请 5 页，应该复用原 p1
    void* p4 = cache.allocateSpan(5);
    assert(p4 == p1);
    std::cout << "Re-allocated 5 pages at " << p4 << std::endl;

    cache.deallocateSpan(p4);
    std::cout << "Deallocated 5-page block again\n";

    std::cout << "All tests passed!\n";
    return 0;
}