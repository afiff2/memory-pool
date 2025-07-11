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
constexpr size_t MAX_SMALL_SZ   = 512;
constexpr size_t MAX_MEDIUM_SZ  = 4 * 1024;
constexpr size_t MAX_LARGE_SZ   = 64 * 1024;
constexpr size_t MAX_BYTES      = 256 * 1024;      // 256KB

// 级别步长
constexpr size_t STEP_SMALL   = ALIGNMENT;
constexpr size_t STEP_MEDIUM  = 64;
constexpr size_t STEP_LARGE   = 512;
constexpr size_t STEP_XLARGE  = 4096;

// 各级数量
constexpr size_t CLS_SMALL   = (MAX_SMALL_SZ   + STEP_SMALL - 1)  / STEP_SMALL;
constexpr size_t CLS_MEDIUM  = (MAX_MEDIUM_SZ  - MAX_SMALL_SZ   + STEP_MEDIUM - 1) / STEP_MEDIUM;
constexpr size_t CLS_LARGE   = (MAX_LARGE_SZ   - MAX_MEDIUM_SZ  + STEP_LARGE    - 1) / STEP_LARGE;
constexpr size_t CLS_XLARGE  = (MAX_BYTES      - MAX_LARGE_SZ   + STEP_XLARGE   - 1) / STEP_XLARGE;
constexpr size_t NUM_CLASSES = CLS_SMALL + CLS_MEDIUM + CLS_LARGE + CLS_XLARGE;

// 大小类管理
class SizeClass
{
  public:
    // 将任意 size 映射到 [0 .. NUM_CLASSES-1]
    static size_t getIndex(size_t bytes) noexcept {
        if (bytes <= MAX_SMALL_SZ) {
            return (bytes + STEP_SMALL - 1) / STEP_SMALL - 1;
        }
        else if (bytes <= MAX_MEDIUM_SZ) {
            size_t off = (bytes - MAX_SMALL_SZ + STEP_MEDIUM - 1) / STEP_MEDIUM - 1;
            return CLS_SMALL + off;
        }
        else if (bytes <= MAX_LARGE_SZ) {
            size_t off = (bytes - MAX_MEDIUM_SZ + STEP_LARGE - 1) / STEP_LARGE - 1;
            return CLS_SMALL + CLS_MEDIUM + off;
        }
        else {
            size_t off = (bytes - MAX_LARGE_SZ + STEP_XLARGE - 1) / STEP_XLARGE - 1;
            return CLS_SMALL + CLS_MEDIUM + CLS_LARGE + off;
        }
    }
    //由 index 映射到 size
    static size_t getSize(size_t index) noexcept {
        if (index < CLS_SMALL) {
            return (index + 1) * STEP_SMALL;
        }
        index -= CLS_SMALL;

        if (index < CLS_MEDIUM) {
            return MAX_SMALL_SZ + (index + 1) * STEP_MEDIUM;
        }
        index -= CLS_MEDIUM;

        if (index < CLS_LARGE) {
            return MAX_MEDIUM_SZ + (index + 1) * STEP_LARGE;
        }
        index -= CLS_LARGE;

        if (index < CLS_XLARGE) {
            return MAX_LARGE_SZ + (index + 1) * STEP_XLARGE;
        }

        // 超出支持范围
        return 0;
    }
};

struct FetchResult {
    void* head;
    size_t count;
};

} // namespace memory_pool