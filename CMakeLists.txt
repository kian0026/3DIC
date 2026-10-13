cmake_minimum_required(VERSION 3.16)
project(thdic_router LANGUAGES CXX)

# 禁止源码目录内构建, 产物统一进 build/
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "请在 build/ 子目录构建: cmake -S . -B build && cmake --build build")
endif()

set(CMAKE_CXX_STANDARD 17)           # std::filesystem / 结构化绑定
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 可执行文件统一输出到 build/ (配合 -B build, 不污染源码目录)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# 源文件清单
set(THDIC_SOURCES
    src/parser/Tokenizer.cpp
    src/parser/LefParser.cpp
    src/parser/DefParser.cpp
    src/parser/ThermalParser.cpp
    src/main.cpp
)
# DesignDatabase.cpp 属第 5 步; 文件就位后自动纳入构建, 无需改本文件
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/parser/DesignDatabase.cpp")
    list(APPEND THDIC_SOURCES src/parser/DesignDatabase.cpp)
endif()

add_executable(thdic_router ${THDIC_SOURCES})

# 头文件路径: include/ 是 parser/*.h 的根; src/ 一并加入便于 src/test 复用
target_include_directories(thdic_router PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(thdic_router PRIVATE
        -O2 -Wall -Wextra -Werror   # 所有 warning 当错误对待
    )
endif()
