cmake_minimum_required(VERSION 3.16)
project(HPHttpServer VERSION 1.0.0 LANGUAGES CXX)

# 0. 构建类型
#    不显式指定 CMAKE_BUILD_TYPE 时，CMake 不会附加任何 -O 选项（等价于 -O0），
#    压测结果会差一个数量级，所以这里默认 Release。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)

# 1. C++ 标准
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 2. 寻找依赖库
# Boost (asio, beast 依赖 system 组件)
find_package(Boost REQUIRED COMPONENTS system)

# jsoncpp
find_package(jsoncpp REQUIRED)

# 线程库
find_package(Threads REQUIRED)

# jsoncpp 各发行版导出的 target 名不统一，逐个探测，最后退回裸库名（-ljsoncpp）
if(TARGET JsonCpp::JsonCpp)
    set(HP_JSONCPP JsonCpp::JsonCpp)
elseif(TARGET jsoncpp_lib)
    set(HP_JSONCPP jsoncpp_lib)
elseif(TARGET jsoncpp_static)
    set(HP_JSONCPP jsoncpp_static)
else()
    set(HP_JSONCPP jsoncpp)
endif()

# gperftools 的 profiler 设为可选：装了才链接，换机器时不至于直接构建失败
find_library(HP_PROFILER_LIB NAMES profiler)
if(HP_PROFILER_LIB)
    message(STATUS "找到 gperftools profiler: ${HP_PROFILER_LIB}")
else()
    message(STATUS "未找到 gperftools profiler，跳过链接")
endif()

# 3. 可执行文件
set(HP_SOURCES
    main.cpp
    httpServer.cpp
    httpSession.cpp
    IOServicePool.cpp
    logicProcessLayer.cpp
)

# 头文件只用于 IDE 索引，不参与编译
set(HP_HEADERS
    httpServer.h
    httpSession.h
    IOServicePool.h
    logicProcessLayer.h
    Singleton.h
    shared_object_pool.hpp
)

add_executable(main ${HP_SOURCES} ${HP_HEADERS})

# 4. 头文件路径
target_include_directories(main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# 5. 链接库
target_link_libraries(main PRIVATE
    Boost::system
    ${HP_JSONCPP}
    Threads::Threads
)

if(HP_PROFILER_LIB)
    target_link_libraries(main PRIVATE ${HP_PROFILER_LIB})
    # 用 #ifdef HP_HAVE_PROFILER 包住 ProfilerStart/ProfilerStop，即可在缺少该库时正常编译
    target_compile_definitions(main PRIVATE HP_HAVE_PROFILER)
endif()

# 6. 编译选项
# Release 下不要追加 -O2：CMAKE_CXX_FLAGS_RELEASE 已经带 -O3 -DNDEBUG，
# 而 target_compile_options 排在命令行更靠后的位置，-O2 会把 -O3 覆盖掉（旧配置的 bug）。
# Debug 下 CMAKE_CXX_FLAGS_DEBUG 只有 -g，这里显式补 -O0。
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(main PRIVATE
        -Wall
        -Wextra
        $<$<CONFIG:Debug>:-O0>
    )
endif()

# 7. 可选优化开关（默认关闭，仅建议本机压测时打开）
option(HP_NATIVE_ARCH "针对本机 CPU 生成指令 (-march=native)，换机器编译请关闭" OFF)
option(HP_ENABLE_LTO   "开启链接时优化 (LTO)" OFF)

if(HP_NATIVE_ARCH AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(main PRIVATE $<$<CONFIG:Release>:-march=native>)
endif()

if(HP_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT HP_IPO_SUPPORTED OUTPUT HP_IPO_MESSAGE)
    if(HP_IPO_SUPPORTED)
        set_property(TARGET main PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(WARNING "LTO 不可用，已跳过：${HP_IPO_MESSAGE}")
    endif()
endif()
