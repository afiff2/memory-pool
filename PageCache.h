#pragma once
#include <cstddef>
#include <map>
#include <mutex>
#include <sys/mman.h>
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
        // 每个页的头部元信息，紧接着放 Span 对象数组
        struct SpanPage
        {
            SpanPage *next; // 链到下一个 page
            // Span 对象紧跟在这里开始
        };
        static inline SpanPage *pageList_ = nullptr;
        static inline Span *freeList_ = nullptr;

        // 一个页最多能放多少个 Span
        static constexpr size_t HeaderSize = 64;
        static_assert(HeaderSize >= sizeof(SpanPage), "HeaderSize must cover SpanPage metadata");
        static_assert(PageCache::PAGE_SIZE > HeaderSize + sizeof(Span), "Page too small to hold any Span");

        // 从 PageCache 申请一页并拆成若干 Span
        static void allocateNewPage()
        {
            void *raw =
                ::mmap(nullptr, PageCache::PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (raw == MAP_FAILED)
                throw std::bad_alloc();

            auto *page = static_cast<SpanPage *>(raw);
            page->next = pageList_;
            pageList_ = page;

            char *base = reinterpret_cast<char *>(page) + HeaderSize;
            size_t count = (PageCache::PAGE_SIZE - HeaderSize) / sizeof(Span);
            for (size_t i = 0; i < count; ++i)
            {
                auto *slot = reinterpret_cast<Span *>(base + i * sizeof(Span));
                slot->next = freeList_;
                freeList_ = slot;
            }
        }

      public:
        // 取一个 Span（若空则先新开一页）
        static Span *get()
        {
            if (!freeList_)
            {
                allocateNewPage();
            }
            Span *s = freeList_;
            freeList_ = freeList_->next;
            // placement-new Span 的内部成员（必要时）
            s->prev = s->next = nullptr;
            return s;
        }

        // 归还一个 Span 到池里
        static void put(Span *s)
        {
            if (!s)
                return;
            // 如果需要析构，调用 ~Span()
            s->next = freeList_;
            freeList_ = s;
        }

        // 在 PageCache 析构时清理所有页面
        static void clear()
        {
            // 释放所有 mmap 的页
            SpanPage *p = pageList_;
            while (p)
            {
                SpanPage *nxt = p->next;
                ::munmap(p, PageCache::PAGE_SIZE);
                p = nxt;
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
