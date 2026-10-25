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
target_link_libraries(cipher_memory_commands PUBLIC cipher_memory)

if(MSVC)
    target_compile_options(cipher_memory_commands PRIVATE /W4 /permissive-)
else()
    target_compile_options(cipher_memory_commands PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# cipher_assistant  —  assistant capability provider boundary (v0.4 preview)
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# cipher_process  —  bounded, shell-free child process runner (provider CLIs)
# ---------------------------------------------------------------------------
add_library(cipher_process
    src/process_runner.cpp
)
target_include_directories(cipher_process PUBLIC include)
target_link_libraries(cipher_process PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(cipher_process PRIVATE /W4 /permissive-)
else()
    target_compile_options(cipher_process PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# cipher_assistant  —  assistant capability provider boundary (v0.4 preview)
# ---------------------------------------------------------------------------
add_library(cipher_assistant
    src/assistant_backend.cpp
    src/assistant_cli.cpp
)
target_include_directories(cipher_assistant PUBLIC include)
target_link_libraries(cipher_assistant PUBLIC cipher_process)

if(MSVC)
    target_compile_options(cipher_assistant PRIVATE /W4 /permissive-)
else()
    target_compile_options(cipher_assistant PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# cipher_assistant_commands  —  REPL commands for ask and ai
# ---------------------------------------------------------------------------
add_library(cipher_assistant_commands
    src/assistant_commands.cpp
)
target_include_directories(cipher_assistant_commands PUBLIC include)
target_link_libraries(cipher_assistant_commands PUBLIC cipher_assistant)

if(MSVC)
    target_compile_options(cipher_assistant_commands PRIVATE /W4 /permissive-)
else()
    target_compile_options(cipher_assistant_commands PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# cipher  —  main executable
# ---------------------------------------------------------------------------
add_executable(cipher src/main.cpp)
target_link_libraries(cipher PRIVATE cipher_core cipher_config cipher_logger cipher_memory_commands cipher_assistant_commands)

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

    # Deterministic stand-in for the Claude Code CLI. Not a test itself; never
    # contacts a service. Named so the CI pipeline-verifier glob does not pick it up.
    add_executable(cipher_fake_cli tests/fixtures/fake_cli.cpp)

    # Bounded process runner, exercised against the fake CLI.
    add_executable(cipher_process_tests tests/test_process_runner.cpp)
    target_link_libraries(cipher_process_tests PRIVATE cipher_process)
    target_compile_definitions(cipher_process_tests PRIVATE
        "CIPHER_FAKE_CLI_PATH=\"$<TARGET_FILE:cipher_fake_cli>\"")
    add_dependencies(cipher_process_tests cipher_fake_cli)
    add_test(NAME cipher_process_tests COMMAND cipher_process_tests)

    # Claude Code and Hermes adapters: scripted-runner contract tests plus end-to-end.
    add_executable(cipher_assistant_cli_tests tests/test_assistant_cli.cpp)
    target_link_libraries(cipher_assistant_cli_tests PRIVATE cipher_assistant)
    target_compile_definitions(cipher_assistant_cli_tests PRIVATE
        "CIPHER_FAKE_CLI_PATH=\"$<TARGET_FILE:cipher_fake_cli>\"")
    add_dependencies(cipher_assistant_cli_tests cipher_fake_cli)
    add_test(NAME cipher_assistant_cli_tests COMMAND cipher_assistant_cli_tests)

    # REPL commands for ask and ai.
    add_executable(cipher_assistant_commands_tests tests/test_assistant_commands.cpp)
    target_link_libraries(cipher_assistant_commands_tests PRIVATE cipher_assistant_commands)
    add_test(NAME cipher_assistant_commands_tests COMMAND cipher_assistant_commands_tests)
endif()
