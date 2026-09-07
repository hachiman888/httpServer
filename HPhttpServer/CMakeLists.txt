cmake_minimum_required(VERSION 3.16)
project(HPHttpServer VERSION 1.0.0 LANGUAGES CXX)

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


# 3. 可执行文件
add_executable(main
    main.cpp
    httpServer.cpp
    httpServer.h
    httpSession.cpp
    httpSession.h
    IOServicePool.cpp
    IOServicePool.h
    logicProcessLayer.cpp
    logicProcessLayer.h
    Singleton.h
    shared_object_pool.hpp
)

# 4. 头文件路径
target_include_directories(main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# 5. 链接库
target_link_libraries(main
    Boost::system
    jsoncpp
    Threads::Threads
    profiler
)

# 6. Debug 编译选项
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(main PRIVATE -g -O0)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(main PRIVATE -O2)
endif()