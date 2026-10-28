#include "cipher/ask_commands.hpp"
#include "cipher/cipher_core.hpp"
#include "cipher/config.hpp"
#include "cipher/logger.hpp"
#include "cipher/memory_commands.hpp"
#include "cipher/model_tool_loop.hpp"
#include "cipher/ollama_backend.hpp"
#include "cipher/tool_commands.hpp"
#include "cipher/tool_registry.hpp"

#include <iostream>
#include <memory>
#include <string>

// ---------------------------------------------------------------------------
// main.cpp  —  Terminal entry point for CIPHER
//
// Startup sequence:
//   1. Load configuration from "cipher.conf" (optional; defaults are used
//      when the file is absent).
//   2. Initialise the logger using the configured level and log file.
//   3. Log the startup event.
//   4. Run the command loop.
//   5. Log the shutdown event.
//
// Configuration errors stop startup with a clear message.  A missing config
// file is not an error — CIPHER is useful out of the box with no setup.
//
// Why is the logger initialised in main and not inside CipherCore?
//   Keeping the Logger in main keeps the core independent of I/O policy.
//   The core only knows how to execute commands; it does not decide where
//   diagnostic messages go.  This makes the core easier to test.
// ---------------------------------------------------------------------------

static constexpr const char* CONFIG_FILE = "cipher.conf";

int main() {
    // -----------------------------------------------------------------------
    // 1. Load configuration.
    // -----------------------------------------------------------------------
    const cipher::ConfigResult cfg_result = cipher::ConfigParser::load(CONFIG_FILE);

    if (!cfg_result.ok()) {
        std::cerr << "CIPHER: configuration error(s) in '" << CONFIG_FILE << "':\n";
        for (const auto& err : cfg_result.errors) {
            if (err.line > 0) {
                std::cerr << "  Line " << err.line << ": " << err.message << '\n';
            } else {
                std::cerr << "  " << err.message << '\n';
            }
        }
        std::cerr << "Fix the errors above and restart CIPHER.\n";
        return 1;
    }

    const cipher::Config& cfg = cfg_result.config;

    // -----------------------------------------------------------------------
    // 2. Initialise the logger.
    // -----------------------------------------------------------------------
    cipher::Logger log(cfg.log_level, cfg.log_file);

    // -----------------------------------------------------------------------
    // 3. Log startup.
    // -----------------------------------------------------------------------
    log.info("CIPHER v" + std::string(cipher::CipherCore::version()) + " starting");

    // Log the effective configuration regardless of whether it came from a
    // file or defaults.  We do not claim the file was loaded here because
    // ConfigParser::load() returns ok() for both "file absent" (defaults) and
    // "file parsed successfully" — we cannot distinguish them without adding
    // extra state.  "Configuration active" is always accurate.
    log.debug("Configuration active (log_level=" +
              std::string(cipher::log_level_name(cfg.log_level)) + ")");


    // -----------------------------------------------------------------------
    // 4. Initialise core and run the command loop.
    // -----------------------------------------------------------------------
    cipher::CipherCore core;

    // Memory is opt-in. A memory file that exists but cannot be parsed stops
    // startup, so the next save can never overwrite the owner's data.
    cipher::PermissionPolicy memory_policy;
    for (const std::string& pattern : cfg.memory_deny) {
        memory_policy.add_blacklist_pattern(pattern);
    }
    cipher::MemoryCommands memory(cfg.memory_file, memory_policy);
    const cipher::MemoryResult memory_result = memory.open();
    if (!memory_result.is_ok()) {
        std::cerr << "CIPHER: could not load memory file '" << cfg.memory_file << "':\n";
        for (const auto& err : memory_result.errors) {
            if (err.line > 0) {
                std::cerr << "  Line " << err.line << ": " << err.message << '\n';
            } else {
                std::cerr << "  " << err.message << '\n';
            }
        }
        std::cerr << "Fix or move the file, then restart CIPHER.\n";
        return 1;
    }

    // Tools are offered input after memory and before the core, like memory.
    cipher::ToolCommands tools(cfg.memory_file, cfg.filesystem_read_root);

    // The local Ollama backend is opt-in. With ollama_port unset, `ask` reports that it is disabled.
    std::unique_ptr<cipher::OllamaBackend> ollama;
    if (cfg.ollama_port != 0) {
        cipher::OllamaConfig ollama_config;
        ollama_config.port = cfg.ollama_port;
        ollama = std::make_unique<cipher::OllamaBackend>(ollama_config);
    }
    // Model tool calls are opt-in and need ollama_port. They share the owner's registry, so the owner's
    // policy also gates them and `tool log` shows them with origin=model. Only the fixed offered list is exposed.
    std::unique_ptr<cipher::ModelToolLoop> tool_loop;
    if (ollama && cfg.model_tool_calls) {
        tool_loop = std::make_unique<cipher::ModelToolLoop>(
            *ollama, tools.registry(),
            [](const std::string& tool_id, const std::string& description) {
                std::cout << "The model asks to run '" << tool_id << "' (" << description
                          << "). Type exact yes to allow: " << std::flush;
                std::string answer;
                return std::getline(std::cin, answer) && answer == "yes";
            },
            120.0, cipher::model_offered_tool_ids());
    } else if (cfg.model_tool_calls) {
        std::cerr << "model_tool_calls is set but ollama_port is not; model tool calls stay off.\n";
    }
    cipher::AskCommands ask(ollama.get(), 120.0, tool_loop.get(), &memory);

    std::cout << "CIPHER v" << cipher::CipherCore::version() << '\n';
    std::cout << "Contextual Intelligence Platform for Home, Enterprise & Research\n";
    std::cout << "Type 'help' to begin. This build runs locally; `ask` needs ollama_port in cipher.conf.\n\n";

    std::string input;
    while (true) {
        std::cout << "CIPHER> ";
        if (!std::getline(std::cin, input)) {
            std::cout << "\nInput closed. CIPHER session ended.\n";
            log.info("CIPHER session ended (input stream closed)");
            break;
        }

        const cipher::MemoryCommandResult memory_reply = memory.execute(input);
        if (memory_reply.handled) {
            std::cout << memory_reply.text << '\n';
            continue;
        }

        const cipher::ToolCommandResult tool_reply = tools.execute(input);
        if (tool_reply.handled) {
            std::cout << tool_reply.text << '\n';
            continue;
        }

        const cipher::AskCommandResult ask_reply = ask.execute(input);
        if (ask_reply.handled) {
            if (!ask_reply.shared_memory_id.empty()) {
                log.info("ask --memory: memory id '" + ask_reply.shared_memory_id + "' passed to the local backend");
            }
            std::cout << ask_reply.text << '\n';
            continue;
        }

        const cipher::CommandResponse response = core.execute(input);
        if (!response.text.empty()) {
            std::cout << response.text << '\n';
        }
        if (!memory_reply.text.empty()) {
            std::cout << memory_reply.text << '\n';
        }
        if (response.exit_requested) {
            log.info("CIPHER session ended (user request)");
            break;
        }
    }

    // -----------------------------------------------------------------------
    // 5. Log shutdown.
    // -----------------------------------------------------------------------
    log.info("CIPHER shutdown complete");
    return 0;
}
