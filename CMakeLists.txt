cmake_minimum_required(VERSION 3.10)

project(MultiReactor VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

# 核心库：除 main.cpp 外的所有源文件。main 是唯一进程入口，测试目标不链接它，
# 但 HttpContext/Router/StaticFileHandler 等纯逻辑模块需要完整参与单测
file(GLOB SRC_FILES ${PROJECT_SOURCE_DIR}/src/*.cpp)
list(REMOVE_ITEM SRC_FILES ${PROJECT_SOURCE_DIR}/src/main.cpp)
add_library(core STATIC ${SRC_FILES})
target_include_directories(core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(core PUBLIC Threads::Threads)

# 服务器可执行文件
add_executable(multireactor ${PROJECT_SOURCE_DIR}/src/main.cpp)
target_link_libraries(multireactor PRIVATE core)

# 单元测试：HttpContext/Router/StaticFileHandler 不依赖事件循环与网络，直接单测。
# 运行：cmake --build build && ctest --test-dir build
# 注意 glob 只匹配 *.cpp，test/e2e.py 不会被误抓进本目标
file(GLOB TEST_FILES ${PROJECT_SOURCE_DIR}/test/*.cpp)
add_executable(unit_tests ${TEST_FILES})
target_link_libraries(unit_tests PRIVATE core)
enable_testing()
add_test(NAME unit_tests COMMAND unit_tests)

# 端到端回归：启动真实服务端进程 + 裸 socket 逐字节校验，覆盖单测碰不到的
# 事件循环、连接生命周期、SIGTERM 排空（详见 test/e2e.py 头部说明）。
# 纯 Python 标准库，无第三方依赖；未安装 python3 时静默跳过而不阻断构建。
# 显式指定 WORKING_DIRECTORY：ctest 默认在 build/ 下运行，而脚本以仓库根为基准。
find_program(PYTHON3_EXECUTABLE NAMES python3 python)
if(PYTHON3_EXECUTABLE)
    add_test(NAME e2e COMMAND ${PYTHON3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/test/e2e.py)
    set_tests_properties(e2e PROPERTIES WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
else()
    message(STATUS "未找到 python3，跳过端到端回归测试 (test/e2e.py)")
endif()
