cmake_minimum_required(VERSION 3.10)

project(MultiReactor VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 收集src下所有源文件
file(GLOB SRC_FILES ${PROJECT_SOURCE_DIR}/src/*.cpp)

# 生成可执行文件
add_executable(multireactor ${SRC_FILES})

# 给multireactor添加头文件目录
target_include_directories(multireactor
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

find_package(Threads REQUIRED)
target_link_libraries(multireactor PRIVATE Threads::Threads)