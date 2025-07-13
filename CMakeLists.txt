# 设置 CMake 最低版本
cmake_minimum_required(VERSION 3.14)

# option(ENABLE_GPROF "Enable gprof profiling" OFF)
# if(ENABLE_GPROF)
#   add_compile_options(-pg)
#   add_link_options(-pg)
# endif()

# 项目名称和语言
project(memory-pool LANGUAGES CXX)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 开启 O2/O3 优化（Release 模式下默认会启用 -O3）
set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type (Debug, Release, etc.)")

# 添加头文件目录（包括 test 目录中的头文件）
include_directories(${PROJECT_SOURCE_DIR})

# 收集所有源文件（除了测试）
file(GLOB SRC_FILES
    "${PROJECT_SOURCE_DIR}/*.cpp"
    "!${PROJECT_SOURCE_DIR}/test/*.cpp"
)

# 构建静态库
add_library(memory_pool STATIC ${SRC_FILES})

# 查找线程库
find_package(Threads REQUIRED)

# 构建测试程序并分别链接库
add_executable(PageCacheTest1 ${PROJECT_SOURCE_DIR}/test/PageCache_test1.cpp)
add_executable(PageCacheTest2 ${PROJECT_SOURCE_DIR}/test/PageCache_test2.cpp)
add_executable(CentralCacheTest ${PROJECT_SOURCE_DIR}/test/CentralCache_test.cpp)
add_executable(MemoryPoolBenchmark ${PROJECT_SOURCE_DIR}/test/memory_pool_benchmark.cpp)

# 分别链接库
target_link_libraries(PageCacheTest1 PRIVATE memory_pool)
target_link_libraries(PageCacheTest2 PRIVATE memory_pool Threads::Threads)
target_link_libraries(CentralCacheTest PRIVATE memory_pool Threads::Threads)
target_link_libraries(MemoryPoolBenchmark PRIVATE memory_pool Threads::Threads)