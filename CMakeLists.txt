cmake_minimum_required(VERSION 3.16)

project(CIPHER VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(cipher_core
    src/cipher_core.cpp
)
target_include_directories(cipher_core PUBLIC include)

if(MSVC)
    target_compile_options(cipher_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(cipher_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(cipher src/main.cpp)
target_link_libraries(cipher PRIVATE cipher_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(cipher_tests tests/test_cipher_core.cpp)
    target_link_libraries(cipher_tests PRIVATE cipher_core)
    add_test(NAME cipher_core_tests COMMAND cipher_tests)
endif()
