cmake_minimum_required(VERSION 3.16)

project(CIPHER VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Generated version header (single source of truth from project VERSION)
# ---------------------------------------------------------------------------
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/cipher/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/include/cipher/version.hpp
    @ONLY
)

# ---------------------------------------------------------------------------
# cipher_core  —  command handling
# ---------------------------------------------------------------------------
add_library(cipher_core
    src/cipher_core.cpp
)
target_include_directories(cipher_core PUBLIC
    include
    ${CMAKE_CURRENT_BINARY_DIR}/generated/include
)

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
# cipher_memory  —  persistent memory subsystem (v0.3 foundation)
# ---------------------------------------------------------------------------
add_library(cipher_memory
    src/memory.cpp
)
target_include_directories(cipher_memory PUBLIC include)

if(MSVC)
    target_compile_options(cipher_memory PRIVATE /W4 /permissive-)
else()
    target_compile_options(cipher_memory PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# cipher_memory_commands  —  REPL memory commands (remember/recall/forget)
# ---------------------------------------------------------------------------
add_library(cipher_memory_commands
    src/memory_commands.cpp
)
target_include_directories(cipher_memory_commands PUBLIC include)
target_link_libraries(cipher_memory_commands PUBLIC cipher_memory cipher_assistant)

if(MSVC)
    target_compile_options(cipher_memory_commands PRIVATE /W4 /permissive-)
else()
    target_compile_options(cipher_memory_commands PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# cipher_assistant  —  assistant capability provider boundary (v0.4 preview)
# ---------------------------------------------------------------------------
add_library(cipher_assistant
    src/assistant_backend.cpp
)
target_include_directories(cipher_assistant PUBLIC include)

if(MSVC)
    target_compile_options(cipher_assistant PRIVATE /W4 /permissive-)
else()
    target_compile_options(cipher_assistant PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# cipher_tools  —  v0.5 permissioned tool registry (static, built-in only)
# ---------------------------------------------------------------------------
add_library(cipher_tools
    src/tool_registry.cpp
)
target_include_directories(cipher_tools PUBLIC include)
target_link_libraries(cipher_tools PUBLIC cipher_assistant cipher_core cipher_memory)

if(MSVC)
    target_compile_options(cipher_tools PRIVATE /W4 /permissive-)
else()
    target_compile_options(cipher_tools PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# cipher  —  main executable
# ---------------------------------------------------------------------------
add_executable(cipher src/main.cpp)
target_link_libraries(cipher PRIVATE cipher_core cipher_config cipher_logger cipher_memory_commands)

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

    # Persistent memory subsystem tests.
    add_executable(cipher_memory_tests tests/test_memory.cpp)
    target_link_libraries(cipher_memory_tests PRIVATE cipher_memory)
    add_test(NAME cipher_memory_tests COMMAND cipher_memory_tests)

    add_executable(cipher_memory_commands_tests tests/test_memory_commands.cpp)
    target_link_libraries(cipher_memory_commands_tests PRIVATE cipher_memory_commands)
    add_test(NAME cipher_memory_commands_tests COMMAND cipher_memory_commands_tests)

    # Assistant capability provider subsystem tests.
    add_executable(cipher_assistant_tests tests/test_assistant_backend.cpp)
    target_link_libraries(cipher_assistant_tests PRIVATE cipher_assistant)
    add_test(NAME cipher_assistant_tests COMMAND cipher_assistant_tests)

    # v0.4 no-model verification: core, memory and provider layer with no provider.
    add_executable(cipher_no_model_tests tests/test_no_model_runtime.cpp)
    target_link_libraries(cipher_no_model_tests PRIVATE cipher_core cipher_memory_commands cipher_assistant)
    add_test(NAME cipher_no_model_tests COMMAND cipher_no_model_tests)

    # v0.5 Phase 1-2: tool registry, lifecycle, permission gate and audit.
    add_executable(cipher_tool_registry_tests tests/test_tool_registry.cpp)
    target_link_libraries(cipher_tool_registry_tests PRIVATE cipher_tools)
    add_test(NAME cipher_tool_registry_tests COMMAND cipher_tool_registry_tests)
endif()
