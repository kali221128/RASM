cmake_minimum_required(VERSION 3.10)
project(RASM CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -O2)
endif()

set(SRC
    src/main.cpp
    src/buffer.cpp
    src/lexer.cpp
    src/symbol.cpp
    src/opcode.cpp
    src/encoder.cpp
    src/assembler.cpp
    src/pe.cpp
)

add_executable(rasm ${SRC})
target_include_directories(rasm PRIVATE src)
