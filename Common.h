#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>
#include <bit>

namespace memory_pool
{
// ALIGNMENT 是平台最大对齐
constexpr size_t ALIGNMENT = alignof(std::max_align_t);
// 保证是 2 的幂
static_assert((ALIGNMENT & (ALIGNMENT - 1)) == 0, "ALIGNMENT must be power-of-two");
constexpr size_t MAX_BYTES = 256 * 1024;              // 256KB
constexpr size_t FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT; // ALIGNMENT等于指针void*的大小

// 大小类管理
class SizeClass
{
  public:
    [[nodiscard]]
    static inline constexpr size_t roundUp(size_t bytes) noexcept // constexpr 可以在编译期内求常量值
    {
        return (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    }

    [[nodiscard]]
    static inline constexpr size_t getIndex(size_t bytes) noexcept // 向上整除后-1
    {
        // 已知 ALIGNMENT 是 2^k，除以 ALIGNMENT 相当于右移 k 位
        return ((bytes + ALIGNMENT - 1) >> std::countr_zero(ALIGNMENT)) - 1;
    }
};

} // namespace memory_pool