#cmake_minimum_required(VERSION 3.20)
#project(sharehash)
#
#set(CMAKE_CXX_STANDARD 14)
##add_compile_options(-pg -O3)
#add_compile_options(-pg -g -O1)
##add_compile_options(-fno-inline-small-functions -fno-inline)
#
## 添加 sanitizer 开关变量（仅此而已）
#option(USE_ASAN "Enable AddressSanitizer" OFF)
#option(USE_TSAN "Enable ThreadSanitizer" OFF)
#
## 冲突检查（安全）
#if (USE_ASAN AND USE_TSAN)
#    message(FATAL_ERROR "ASAN and TSAN cannot be enabled at the same time.")
#endif()
#
#find_package(Boost COMPONENTS program_options thread REQUIRED)
#find_package(Threads REQUIRED)
#find_package(OpenMP REQUIRED)
#
#include_directories(benchmark/competitors include ${Boost_INCLUDE_DIRS} ${Threads_INCLUDE_DIRS} ${OpenMP_INCLUDE_DIRS})
#
#add_executable(sharehash main.cpp)
#add_executable(benchmark benchmark/benchmark.cpp)
#
## 启用 ASan
#if (USE_ASAN)
#    message(STATUS "✅ AddressSanitizer enabled")
#    target_compile_options(benchmark PRIVATE -fsanitize=address)
#    target_link_options(benchmark PRIVATE -fsanitize=address)
#endif()
#
## 启用 TSan
#if (USE_TSAN)
#    message(STATUS "✅ ThreadSanitizer enabled")
#    target_compile_options(benchmark PRIVATE -fsanitize=thread)
#    target_link_options(benchmark PRIVATE -fsanitize=thread)
#endif()
#
#target_link_libraries(benchmark Boost::program_options Boost::thread Threads::Threads OpenMP::OpenMP_CXX)


#----------------------------------------------------------------

#
#cmake_minimum_required(VERSION 3.20)
#project(sharehash)
#
#set(CMAKE_CXX_STANDARD 14)
##add_compile_options(-pg -O3)
#add_compile_options(-pg -g -O1)
##add_compile_options(-fno-inline-small-functions -fno-inline)
#
#find_package(Boost COMPONENTS program_options thread REQUIRED)
#find_package(Threads REQUIRED)
#find_package(OpenMP REQUIRED)
#
#include_directories(benchmark/competitors include ${Boost_INCLUDE_DIRS} ${Threads_INCLUDE_DIRS} ${OpenMP_INCLUDE_DIRS})
#
#add_executable(sharehash main.cpp)
#
#add_executable(benchmark benchmark/benchmark.cpp)
#
## --- 添加以下行以启用 ASan for benchmark ---
#message(STATUS "✅ AddressSanitizer enabled")
#target_compile_options(benchmark PRIVATE -fsanitize=address)
#target_link_options(benchmark PRIVATE -fsanitize=address)
##target_link_libraries(benchmark PRIVATE -fsanitize=address)
## --- 添加结束 ---
#
## --- 启用 TSan (注释掉 ASan 部分) ---
##message(STATUS "✅ ThreadSanitizer enabled")
##target_compile_options(benchmark PRIVATE -fsanitize=thread -g)
##target_link_options(benchmark PRIVATE -fsanitize=thread)
##target_link_libraries(benchmark PRIVATE -fsanitize=thread)
## --- 添加结束 ---
#
#target_link_libraries(benchmark Boost::program_options Boost::thread Threads::Threads OpenMP::OpenMP_CXX)











#cmake_minimum_required(VERSION 3.20)
#project(sharehash)
#
#set(CMAKE_CXX_STANDARD 14)
##add_compile_options(-pg -O3)
#add_compile_options(-pg -g -O1)
#
## 查找 Boost 并指定 thread 和 program_options 组件
## 添加 system 组件，因为 thread 经常依赖它
#find_package(Boost REQUIRED COMPONENTS program_options thread system)
#find_package(Threads REQUIRED) # 这是标准的 CMake 线程库查找，通常用于 pthreads
#find_package(OpenMP REQUIRED)
#
## 确保 Boost 的包含目录被正确添加
## Threads_INCLUDE_DIRS 和 OpenMP_INCLUDE_DIRS 通常由 find_package 设置
#include_directories(
#    benchmark/competitors
#    include
#    ${Boost_INCLUDE_DIRS}
#    # ${Threads_INCLUDE_DIRS} # Threads::Threads 通常不需要单独的 include_directories
#    # ${OpenMP_INCLUDE_DIRS}  # OpenMP::OpenMP_CXX 通常不需要单独的 include_directories
#)
#
#add_executable(sharehash main.cpp)
#add_executable(benchmark benchmark/benchmark.cpp)
#
## 链接 sharehash 目标
#target_link_libraries(sharehash
#    PRIVATE # 或者 PUBLIC/INTERFACE 取决于您的需求，PRIVATE 通常是好的开始
#    Boost::thread
#    Boost::system
#    Threads::Threads # 如果您的代码直接使用 pthreads 或 std::thread
#    # OpenMP::OpenMP_CXX # 如果 sharehash 本身也使用 OpenMP
#)
#
## 链接 benchmark 目标
#target_link_libraries(benchmark
#    PRIVATE
#    Boost::program_options
#    Boost::thread
#    Boost::system
#    Threads::Threads
#    OpenMP::OpenMP_CXX
#)


cmake_minimum_required(VERSION 3.20)
project(sharehash)

set(CMAKE_CXX_STANDARD 14)
# For profiling with gprof
add_compile_options(-pg -g -O1)

# Sanitizer options
option(USE_ASAN "Enable AddressSanitizer" OFF)
option(USE_TSAN "Enable ThreadSanitizer" OFF)

# Ensure only one sanitizer is used at a time
if(USE_ASAN AND USE_TSAN)
    message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer cannot be enabled at the same time.")
endif()

# Find required packages
find_package(Boost REQUIRED COMPONENTS program_options thread system)
find_package(Threads REQUIRED)
find_package(OpenMP REQUIRED)

include_directories(
        benchmark/competitors
        include
        ${Boost_INCLUDE_DIRS}
)

add_executable(sharehash main.cpp)
add_executable(benchmark benchmark/benchmark.cpp)
target_compile_definitions(benchmark PRIVATE
        SHAREHASH_RESULT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/result"
)

# Apply sanitizers if enabled
if(USE_ASAN)
    message(STATUS "✅ AddressSanitizer enabled")
    foreach(target IN ITEMS sharehash benchmark)
        target_compile_options(${target} PRIVATE -fsanitize=address)
        target_link_options(${target} PRIVATE -fsanitize=address)
    endforeach()
endif()

if(USE_TSAN)
    message(STATUS "✅ ThreadSanitizer enabled")
    foreach(target IN ITEMS sharehash benchmark)
        target_compile_options(${target} PRIVATE -fsanitize=thread)
        target_link_options(${target} PRIVATE -fsanitize=thread)
    endforeach()
endif()

# Link libraries to targets
target_link_libraries(sharehash PRIVATE
        Boost::thread
        Boost::system
        Threads::Threads
)

target_link_libraries(benchmark PRIVATE
        Boost::program_options
        Boost::thread
        Boost::system
        Threads::Threads
        OpenMP::OpenMP_CXX
)
