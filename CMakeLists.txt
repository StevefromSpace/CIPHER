cmake_minimum_required(VERSION 3.16)

project(CIPHER VERSION 1.0.0 LANGUAGES CXX)

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
    src/tool_commands.cpp
    src/filesystem_tools.cpp
)
target_include_directories(cipher_tools PUBLIC include)
target_link_libraries(cipher_tools PUBLIC cipher_assistant cipher_core cipher_memory)

if(MSVC)
    target_compile_options(cipher_tools PRIVATE /W4 /permissive-)
else()
    target_compile_options(cipher_tools PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# cipher_orchestration  —  v1.x Stage 1 pure workflow state machine (no GitHub, model or network)
# ---------------------------------------------------------------------------
add_library(cipher_orchestration
    src/orchestration/state_machine.cpp
    src/orchestration/github_facts.cpp
    src/orchestration/orchestrator.cpp
    src/orchestration/job_store_file.cpp
)
target_include_directories(cipher_orchestration PUBLIC include)

if(MSVC)
    target_compile_options(cipher_orchestration PRIVATE /W4 /permissive-)
else()
    target_compile_options(cipher_orchestration PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# cipher_ollama  —  loopback HTTP client, Ollama assistant backend, and `ask` command
# ---------------------------------------------------------------------------
add_library(cipher_ollama
    src/loopback_http.cpp
    src/ollama_backend.cpp
    src/ask_commands.cpp
    src/model_tool_loop.cpp
)
target_include_directories(cipher_ollama PUBLIC include)
# Vendored single header (nlohmann/json 3.11.3, MIT); SYSTEM keeps its warnings out of /W4.
target_include_directories(cipher_ollama SYSTEM PRIVATE third_party)
target_link_libraries(cipher_ollama PUBLIC cipher_assistant cipher_tools cipher_memory_commands)
if(WIN32)
    target_link_libraries(cipher_ollama PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_options(cipher_ollama PRIVATE /W4 /permissive-)
else()
    target_compile_options(cipher_ollama PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# cipher_llama  —  optional in-process llama.cpp backend (v0.6)
#
# Off by default so the core build stays dependency-free. When ON, CMake fetches the
# pinned llama.cpp source release below and checks its SHA-256. For an offline build,
# pass -DFETCHCONTENT_SOURCE_DIR_LLAMA_CPP=<path to an unpacked llama.cpp v0.7.0>.
# No model is downloaded; the owner sets llama_model_path in cipher.conf.
# ---------------------------------------------------------------------------
option(CIPHER_WITH_LLAMA "Build the in-process llama.cpp backend" OFF)

if(CIPHER_WITH_LLAMA)
    if(CMAKE_VERSION VERSION_LESS 3.24)
        message(FATAL_ERROR "CIPHER_WITH_LLAMA needs CMake 3.24 or newer (found ${CMAKE_VERSION}).")
    endif()
    include(FetchContent)
    # Static, CPU-only libllama with no tools, server, network (RPC/HTTPS) or shared common code.
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_UI OFF CACHE BOOL "" FORCE)
    set(LLAMA_OPENSSL OFF CACHE BOOL "" FORCE)
    set(GGML_RPC OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(llama_cpp
        URL https://github.com/ggml-org/llama.cpp/archive/refs/tags/v0.7.0.tar.gz
        URL_HASH SHA256=55d67acf3ad2ad747a9bbeb75f9f0e173dbcc0d1753a471038a90c819421c5e1
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(llama_cpp)

    add_library(cipher_llama src/llama_backend.cpp)
    target_include_directories(cipher_llama PUBLIC include)
    target_link_libraries(cipher_llama PUBLIC cipher_assistant PRIVATE llama)
    target_compile_definitions(cipher_llama PUBLIC CIPHER_WITH_LLAMA)

    if(MSVC)
        target_compile_options(cipher_llama PRIVATE /W4 /permissive-)
    else()
        target_compile_options(cipher_llama PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

# ---------------------------------------------------------------------------
# cipher  —  main executable
# ---------------------------------------------------------------------------
add_executable(cipher src/main.cpp)
target_link_libraries(cipher PRIVATE cipher_core cipher_config cipher_logger cipher_memory_commands cipher_tools cipher_ollama)
if(CIPHER_WITH_LLAMA)
    target_link_libraries(cipher PRIVATE cipher_llama)
endif()

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

    # v0.5 Phase 3: owner `tool` REPL commands.
    add_executable(cipher_tool_commands_tests tests/test_tool_commands.cpp)
    target_link_libraries(cipher_tool_commands_tests PRIVATE cipher_tools)
    add_test(NAME cipher_tool_commands_tests COMMAND cipher_tool_commands_tests)

    # v0.5 Phase 4: read-only filesystem.read under an allowlisted root.
    add_executable(cipher_filesystem_tools_tests tests/test_filesystem_tools.cpp)
    target_link_libraries(cipher_filesystem_tools_tests PRIVATE cipher_tools)
    add_test(NAME cipher_filesystem_tools_tests COMMAND cipher_filesystem_tools_tests)

    add_executable(cipher_ollama_tests tests/test_ollama_backend.cpp)
    target_link_libraries(cipher_ollama_tests PRIVATE cipher_ollama)
    add_test(NAME cipher_ollama_tests COMMAND cipher_ollama_tests)

    add_executable(cipher_ask_commands_tests tests/test_ask_commands.cpp)
    target_link_libraries(cipher_ask_commands_tests PRIVATE cipher_ollama)
    add_test(NAME cipher_ask_commands_tests COMMAND cipher_ask_commands_tests)

    add_executable(cipher_model_tool_loop_tests tests/test_model_tool_loop.cpp)
    target_link_libraries(cipher_model_tool_loop_tests PRIVATE cipher_ollama)
    add_test(NAME cipher_model_tool_loop_tests COMMAND cipher_model_tool_loop_tests)

    add_executable(cipher_orchestration_tests tests/test_orchestration.cpp)
    target_link_libraries(cipher_orchestration_tests PRIVATE cipher_orchestration)
    add_test(NAME cipher_orchestration_tests COMMAND cipher_orchestration_tests)

    add_executable(cipher_orchestration_flow_tests tests/test_orchestration_flow.cpp)
    target_link_libraries(cipher_orchestration_flow_tests PRIVATE cipher_orchestration)
    add_test(NAME cipher_orchestration_flow_tests COMMAND cipher_orchestration_flow_tests)

    add_executable(cipher_orchestration_store_tests tests/test_orchestration_store.cpp)
    target_link_libraries(cipher_orchestration_store_tests PRIVATE cipher_orchestration)
    add_test(NAME cipher_orchestration_store_tests COMMAND cipher_orchestration_store_tests)

    # v0.6: in-process llama.cpp backend. Real inference runs only when CIPHER_TEST_GGUF names a model file.
    if(CIPHER_WITH_LLAMA)
        add_executable(cipher_llama_tests tests/test_llama_backend.cpp)
        target_link_libraries(cipher_llama_tests PRIVATE cipher_llama cipher_ollama cipher_config)
        add_test(NAME cipher_llama_tests COMMAND cipher_llama_tests)
    endif()
endif()
