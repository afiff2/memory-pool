# 自定义内存池 README

## 项目简介

本项目实现了一个高性能的自定义内存分配器（Memory Pool），支持从小对象到大对象的高效分配与回收。通过多级缓存设计（线程本地缓存、中央缓存、页面缓存）和分级大小管理，实现了对比系统默认 `malloc/free` 更优的性能表现。

## 核心特性

- **多级缓存架构**：
  - **线程本地缓存（ThreadCache）**：快速响应线程内的小对象分配与回收，最大限度地减少锁竞争。
  - **中央缓存（CentralCache）**：在线程本地缓存用尽时，统一管理跨线程内存块的分配与回收，采用自旋锁保证并发安全。
    - **位图跟踪（Bitmap Tracking）**：CentralCache 中的 `SpanTracker` 通过 `uint32_t bitmap[]` 位图高效管理 1024 个块的分配状态，实现 O(1) 的可用块检查与批量分配/回收。
  - **页面缓存（PageCache）**：直接向操作系统申请或回收大页，支持页级别的合并与拆分。
- **分级大小管理**：根据请求大小（对齐、步长）自动映射到对应大小类（SizeClass），快速定位可用区块。
- **批量操作优化**：批量获取/归还内存块，降低调用频次与锁争用。
- **高并发安全**：使用线程局部存储（thread\_local）和自旋锁/互斥锁，确保多线程环境下稳定高效。
## 性能对比

以下基准测试在相同机器上对比了本自定义内存池与系统 `malloc/free` 的性能：

| 指标          | 自定义内存池   | malloc/free   |
| ------------- | ------------- | ------------- |
| **Ops/Sec**   | 32,773,453.50 | 28,202,115.16 |
| **Avg alloc** | 0.05          | 0.08          |
| **P99 alloc** | 0.13          | 0.27          |
| **Avg free**  | 0.05          | 0.05          |
| **P99 free**  | 0.07          | 0.15          |

## 模块结构

```
memory_pool/
├── Common.h          # 对齐与大小类定义
├── MemoryPool.h      # 对外接口封装
├── ThreadCache.h/.cpp   # 线程本地缓存
├── CentralCache.h/.cpp  # 跨线程中央缓存
├── PageCache.h/.cpp     # OS 页面缓存管理
└── …
```

## 安装与编译

1. 克隆仓库：
   ```bash
   git clone https://github.com/afiff2/memory-pool.git
   cd memory-pool
   ```
2. 使用 CMake 构建：
   ```bash
   mkdir build && cd build
   cmake ..
   make -j$(nproc)
   ```

## 测试

在 `tests/` 目录下提供了简单的功能测试与性能基准，使用如下命令运行：

```bash
cd build
./benchmark 16 1000000 9 # 16 线程 × 1 M 操作, seed=9
```

