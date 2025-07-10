# 设置 CMake 最低版本
cmake_minimum_required(VERSION 3.14)

# 项目名称和语言
project(memory-pool LANGUAGES CXX)

# 设置 C++ 标准（推荐使用 C++17 或更高）
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 开启 O2/O3 优化（Release 模式下默认会启用 -O3）
set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type (Debug, Release, etc.)")

# 添加头文件目录（包括 test 目录中的头文件）
include_directories(${PROJECT_SOURCE_DIR})

# 收集所有源文件（除了测试）
file(GLOB SRC_FILES
    "${PROJECT_SOURCE_DIR}/*.cpp"
    "!${PROJECT_SOURCE_DIR}/test/*.cpp"
)

# 构建静态库
add_library(memory_pool STATIC ${SRC_FILES})

# 构建测试程序并链接库
add_executable(PageCacheTest ${PROJECT_SOURCE_DIR}/test/PageCache_test.cpp)
target_link_libraries(PageCacheTest PRIVATE memory_pool)