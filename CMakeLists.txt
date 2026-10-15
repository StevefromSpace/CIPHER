cmake_minimum_required(VERSION 3.16)

project(CIPHER VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# cipher_core  —  command handling (unchanged from v0.1)
# ---------------------------------------------------------------------------
add_library(cipher_core
    src/cipher_core.cpp
)
target_include_directories(cipher_core PUBLIC include)

if(MSVC)
    target_compile_options(cipher_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(cipher_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# cipher_config  —  configuration parser
# ---------------------------------------------------------------------------
add_library(cipher_config
    src/config.cpp
)
target_include_directories(cipher_config PUBLIC include)

if(MSVC)
    target_compile_options(cipher_config PRIVATE /W4 /permissive-)
else()
    target_compile_options(cipher_config PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# cipher_logger  —  diagnostic logger
# ---------------------------------------------------------------------------
add_library(cipher_logger
    src/logger.cpp
)
target_include_directories(cipher_logger PUBLIC include)
target_link_libraries(cipher_logger PUBLIC cipher_config)

if(MSVC)
    target_compile_options(cipher_logger PRIVATE /W4 /permissive-)
else()
    target_compile_options(cipher_logger PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# cipher  —  main executable
# ---------------------------------------------------------------------------
add_executable(cipher src/main.cpp)
target_link_libraries(cipher PRIVATE cipher_core cipher_config cipher_logger)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    # Original command tests (all 18 must keep passing).
    add_executable(cipher_tests tests/test_cipher_core.cpp)
    target_link_libraries(cipher_tests PRIVATE cipher_core)
    add_test(NAME cipher_core_tests COMMAND cipher_tests)

    # Configuration subsystem tests.
    add_executable(cipher_config_tests tests/test_config.cpp)
    target_link_libraries(cipher_config_tests PRIVATE cipher_config)
    add_test(NAME cipher_config_tests COMMAND cipher_config_tests)

    # Logger tests.
    add_executable(cipher_logger_tests tests/test_logger.cpp)
    target_link_libraries(cipher_logger_tests PRIVATE cipher_logger)
    add_test(NAME cipher_logger_tests COMMAND cipher_logger_tests)
endif()
