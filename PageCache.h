#pragma once
#include <cstddef>
#include <map>
#include <mutex>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <sys/mman.h>

// ToDo: 回收回 OS（munmap）
// 每次拆分都 new Span，每次合并都 delete Span，在高并发场景下可能成为瓶颈。

namespace memory_pool
{

class PageCache
{
  public:
    static inline constexpr std::size_t PAGE_SIZE = 4096; // 系统页大小

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

    // 使用前要加锁
    class SpanPool
    {
      private:
        static constexpr std::size_t SPAN_PAGE_COUNT = 1;
        static constexpr std::size_t SPAN_POOL_SIZE = SPAN_PAGE_COUNT * PAGE_SIZE;

        // 用于链接空闲Span的联合体
        union SpanStorage
        {
            alignas(Span) char data[sizeof(Span)];
            SpanStorage *next; // 空闲时用作链表指针
        };

        struct SpanPage
        {
            SpanStorage blocks[(SPAN_POOL_SIZE - sizeof(SpanPage*)) / sizeof(SpanStorage)];
            SpanPage *next;
        };

        static inline SpanPage *pageList_ = nullptr;
        static inline SpanStorage *freeList_ = nullptr;

        static void allocateNewPage()
        {
            void *mem = mmap(nullptr, SPAN_POOL_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (mem == MAP_FAILED)
            {
                throw std::bad_alloc();
            }

            SpanPage *newPage = static_cast<SpanPage *>(mem);
            newPage->next = pageList_;
            pageList_ = newPage;

            // 将新页中的所有SpanStorage链接到空闲链表
            for (std::size_t i = 0; i < sizeof(newPage->blocks) / sizeof(newPage->blocks[0]); ++i)
            {
                newPage->blocks[i].next = freeList_;
                freeList_ = &newPage->blocks[i];
            }
        }

      public:
        static Span *get()
        {
            if (!freeList_)
            {
                allocateNewPage();
            }

            SpanStorage *storage = freeList_;
            freeList_ = freeList_->next;

            return reinterpret_cast<Span*>(storage);
        }

        static void put(Span *s)
        {
            if (!s)
                return;

            SpanStorage *storage = reinterpret_cast<SpanStorage *>(s);
            storage->next = freeList_;
            freeList_ = storage;
        }

        // PageCache析构时使用
        static void clear()
        {
            SpanPage *current = pageList_;
            while (current)
            {
                SpanPage *next = current->next;
                munmap(current, SPAN_POOL_SIZE);
                current = next;
            }
            pageList_ = nullptr;
            freeList_ = nullptr;
        }
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
