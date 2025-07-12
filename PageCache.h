#pragma once
#include <cstddef>
#include <map>
#include <mutex>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// ToDo: 回收回 OS（munmap）
// 每次拆分都 new Span，每次合并都 delete Span，在高并发场景下可能成为瓶颈。

namespace memory_pool
{

class PageCache
{
  public:
    static inline std::size_t PAGE_SIZE = sysconf(_SC_PAGESIZE); // 系统页大小

    static PageCache &getInstance();

    void *allocateSpan(std::size_t numPages); // 分配 ≥numPages 页
    void deallocateSpan(void *ptr);           // 归还整段内存

    ~PageCache();

  private:
    PageCache() = default;
    PageCache(const PageCache &) = delete;
    PageCache &operator=(const PageCache &) = delete;

    struct Span
    {
        void *pageAddr;       // 起始地址
        std::size_t numPages; // 页数
        Span *prev;           // 前驱指针
        Span *next;           // 空闲链表指针
    };

    //使用前要加锁
    class SpanPool {
      public:
        static Span* get() {
            if (head_) {
                Span* s = head_;
                head_ = head_->next;
                return s;
            }
            return new Span();
        }

        static void put(Span* s) {
            s->next = head_;
            head_ = s;
        }

        // PageCache 析构时使用
        static void clear() {
            Span* current = head_;
            while (current) {
                Span* next = current->next;
                delete current;
                current = next;
            }
            head_ = nullptr;
        }

      private:
        static inline Span* head_ = nullptr;
    };

    void *systemAlloc(std::size_t numPages);
    void *endAddr(const Span *s) const
    { // span 尾后一字节
        return static_cast<char *>(s->pageAddr) + s->numPages * PAGE_SIZE;
    }
    bool detachFromFreeList(Span *s); // 摘链表
    void pushToFreeList(Span *s);     // 头插链表

    std::map<std::size_t, Span *> freeSpans_;         // key=页数，value=链表头
    std::unordered_map<void *, Span *> spanStartMap_; // 起始地址 → Span*
    std::unordered_map<void *, Span *> spanEndMap_;   // 结束地址 → Span*
    std::mutex mutex_;
};

} // namespace memory_pool
