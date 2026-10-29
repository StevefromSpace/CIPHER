#include "cipher/filesystem_tools.hpp"
#include "cipher/memory.hpp"
#include "cipher/model_tool_loop.hpp"
#include "cipher/ollama_backend.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// test_model_tool_loop.cpp  â€”  model-proposed tool calls (v0.5 owner-gated loop)
//
// A scripted backend returns queued replies, so these tests need no server.
// They check that every model-proposed call needs owner confirmation, that policy
// and the offered-tool list are enforced, that malformed or unknown requests run
// nothing, that tool output stays off non-local routes, and that the round cap holds.
// ---------------------------------------------------------------------------

namespace {

int g_failures = 0;
int g_passes   = 0;

void pass(const std::string& name) {
    std::cout << "PASS: " << name << '\n';
    ++g_passes;
}

void fail(const std::string& name, const std::string& reason) {
    std::cerr << "FAIL: " << name << "\n  " << reason << '\n';
    ++g_failures;
}

void check(bool condition, const std::string& name, const std::string& reason) {
    if (condition) pass(name);
    else           fail(name, reason);
}

class ScriptedBackend : public cipher::IAssistantBackend {
public:
    cipher::ProcessingRoute route = cipher::ProcessingRoute::VerifiedLocal;
    std::vector<std::string> replies;
    std::vector<std::string> prompts;

    std::string backend_id() const override { return "scripted_backend"; }
    bool is_available() override { return true; }
    cipher::AssistantCapabilities capabilities() const override {
        cipher::AssistantCapabilities caps;
        caps.processing_route = route;
        return caps;
    }
    cipher::AssistantResponse execute_request(const cipher::AssistantRequest& request) override {
        prompts.push_back(request.prompt);
        cipher::AssistantResponse resp;
        resp.status = cipher::AssistantStatus::Ok;
        const std::size_t index = std::min(prompts.size() - 1, replies.size() - 1);
        resp.text = replies[index];
        return resp;
    }
};

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// Builds a loop around a scripted backend and records how often the owner was asked.
struct Harness {
    ScriptedBackend backend;
    cipher::ToolRegistry registry;
    int confirmations = 0;
    bool owner_answer = true;
    std::string last_target;

    cipher::ModelToolLoop make_loop(const std::vector<std::string>& offered) {
        return cipher::ModelToolLoop(backend, registry,
            [this](const std::string&, const std::string&, const std::string& target) {
                ++confirmations;
                last_target = target;
                return owner_answer;
            },
            30.0, offered);
    }
};

void test_plain_answer_passes_through_without_tools() {
    Harness h;
    h.backend.replies = {"pong"};
    cipher::register_builtin_tools(h.registry);
    cipher::ModelToolLoop loop = h.make_loop(cipher::model_offered_tool_ids());

    const std::string text = loop.run("say pong");
    check(contains(text, "pong") && contains(text, "CIPHER ran no tools"), "plain answer: text is returned with footer",
          "text was: " + text);
    check(h.confirmations == 0, "plain answer: owner is not asked",
          "confirmations were " + std::to_string(h.confirmations));
    check(h.backend.prompts.size() == 1, "plain answer: one round",
          "rounds were " + std::to_string(h.backend.prompts.size()));
}

void test_yes_runs_tool_and_feeds_result_back() {
    Harness h;
    h.backend.replies = {"TOOL_CALL: time.now", "It is noon."};
    cipher::register_builtin_tools(h.registry);
    cipher::ModelToolLoop loop = h.make_loop(cipher::model_offered_tool_ids());

    check(starts_with(loop.run("what time is it"), "It is noon."), "yes: final model answer is returned",
          "text was not returned");
    check(h.confirmations == 1, "yes: owner asked once, even for a Safe tool",
          "confirmations were " + std::to_string(h.confirmations));
    check(h.backend.prompts.size() == 2 && contains(h.backend.prompts[1], "Tool result for time.now:"),
          "yes: tool result is sent to the model on the next round",
          "second prompt lacked the result");
    check(h.registry.audit_log().size() == 1 &&
              h.registry.audit_log()[0].origin == cipher::CallOrigin::Model,
          "yes: audit record is tagged origin=model", "audit record missing or tagged owner");
}

void test_no_runs_nothing() {
    Harness h;
    h.owner_answer = false;
    h.backend.replies = {"TOOL_CALL: time.now"};
    cipher::register_builtin_tools(h.registry);
    cipher::ModelToolLoop loop = h.make_loop(cipher::model_offered_tool_ids());

    const std::string text = loop.run("what time is it");
    check(text == "Owner confirmation was not given: time.now. Nothing was run.",
          "no: owner refusal is reported", "text was: " + text);
    check(h.backend.prompts.size() == 1, "no: loop stops after refusal",
          "rounds were " + std::to_string(h.backend.prompts.size()));
    check(h.registry.audit_log().empty(), "no: registry never invoked the tool",
          "audit records were " + std::to_string(h.registry.audit_log().size()));
}

void test_block_tier_is_denied_without_asking_owner() {
    Harness h;
    h.backend.replies = {"TOOL_CALL: test.blocked"};
    int handler_calls = 0;
    cipher::ToolDefinition blocked;
    blocked.id = "test.blocked";
    blocked.description = "test-only blocked tool";
    blocked.category = "test";
    blocked.default_tier = cipher::RiskTier::Block;
    blocked.handler = [&handler_calls](const std::string&) {
        ++handler_calls;
        return std::string("ran");
    };
    h.registry.register_tool(blocked);
    cipher::ModelToolLoop loop = h.make_loop({"test.blocked"});

    const std::string text = loop.run("run it");
    check(h.confirmations == 0, "block: owner is not asked for a Block tool",
          "confirmations were " + std::to_string(h.confirmations));
    check(handler_calls == 0, "block: handler is never called",
          "handler calls were " + std::to_string(handler_calls));
    check(contains(text, "Denied by permission policy (block): test.blocked"),
          "block: denial is reported", "text was: " + text);
}

void test_unknown_and_unoffered_tools_run_nothing() {
    Harness h;
    h.backend.replies = {"TOOL_CALL: filesystem.read"};
    cipher::register_builtin_tools(h.registry);
    cipher::ModelToolLoop loop = h.make_loop(cipher::model_offered_tool_ids());

    const std::string text = loop.run("read a file");
    check(text == "The model asked for tool 'filesystem.read', which is not offered. Nothing was run.",
          "unoffered id: refused before any call", "text was: " + text);
    check(h.confirmations == 0 && h.registry.audit_log().empty(),
          "unoffered id: owner not asked and registry not invoked", "a call happened");
}

void test_malformed_requests_run_nothing() {
    const char* bad[] = {"TOOL_CALL: time.now please", "Sure. TOOL_CALL: time.now",
                         "TOOL_CALL:", "TOOL_CALL: time.now\nTOOL_CALL: system.info"};
    for (const char* reply : bad) {
        Harness h;
        h.backend.replies = {reply};
        cipher::register_builtin_tools(h.registry);
        cipher::ModelToolLoop loop = h.make_loop(cipher::model_offered_tool_ids());

        const std::string text = loop.run("q");
        check(text == "The model's tool request was malformed. Nothing was run." && h.confirmations == 0,
              std::string("malformed: nothing runs for ") + reply, "text was: " + text);
    }
}

void test_round_cap_stops_repeated_requests() {
    Harness h;
    h.backend.replies = {"TOOL_CALL: time.now"};
    cipher::register_builtin_tools(h.registry);
    cipher::ModelToolLoop loop = h.make_loop(cipher::model_offered_tool_ids());

    const std::string text = loop.run("loop forever");
    check(text == "Tool call limit reached. Nothing more was run. Calls that ran: time.now.",
          "round cap: limit message names the calls that ran", "text was: " + text);
    check(static_cast<int>(h.backend.prompts.size()) == cipher::ModelToolLoop::kMaxModelRounds,
          "round cap: submits are bounded",
          "submits were " + std::to_string(h.backend.prompts.size()));
    check(h.confirmations == 1,
          "round cap: a repeated call is confirmed and run once only",
          "confirmations were " + std::to_string(h.confirmations));
}

void test_non_local_route_withholds_tool_output() {
    Harness h;
    h.backend.route = cipher::ProcessingRoute::Unknown;
    h.backend.replies = {"TOOL_CALL: time.now"};
    cipher::register_builtin_tools(h.registry);
    cipher::ModelToolLoop loop = h.make_loop(cipher::model_offered_tool_ids());

    const std::string text = loop.run("what time");
    check(text == "ask stopped: the backend is not a verified local route. Nothing was run.",
          "non-local: loop refuses before any submit", "text was: " + text);
    check(h.backend.prompts.empty() && h.confirmations == 0,
          "non-local: no request, no owner prompt, no tool call", "a call happened");
}

void test_memory_list_is_never_offered() {
    const std::vector<std::string> offered = cipher::model_offered_tool_ids();
    check(std::find(offered.begin(), offered.end(), "memory.list") == offered.end(),
          "memory.list: not in the model-offered set",
          "memory.list was offered");

    Harness h;
    h.backend.replies = {"TOOL_CALL: memory.list"};
    // Even with memory.list registered, the model's own offered set leaves it out.
    cipher::register_memory_tools(h.registry, "unused.jsonl");
    cipher::ModelToolLoop loop = h.make_loop(cipher::model_offered_tool_ids());

    const std::string text = loop.run("list memories");
    check(contains(text, "which is not offered"), "memory.list: a request for it is refused",
          "text was: " + text);
    check(h.backend.prompts.size() == 1 && !contains(h.backend.prompts[0], "memory.list"),
          "memory.list: prompt never mentions it", "prompt mentioned memory.list");
}

void test_oversized_output_is_withheld() {
    Harness h;
    h.backend.replies = {"TOOL_CALL: test.big", "done"};
    cipher::ToolDefinition big;
    big.id = "test.big";
    big.description = "test-only large output tool";
    big.category = "test";
    big.default_tier = cipher::RiskTier::Safe;
    big.handler = [](const std::string&) { return std::string(cipher::ModelToolLoop::kMaxToolOutputBytes + 1, 'x'); };
    h.registry.register_tool(big);
    cipher::ModelToolLoop loop = h.make_loop({"test.big"});

    const std::string text = loop.run("big");
    check(contains(text, "larger than 4096 bytes and was not sent to the model"),
          "oversize: owner is told the output was withheld", "text was: " + text);
    check(h.backend.prompts.size() == 1, "oversize: the model gets no further round with the output",
          "rounds were " + std::to_string(h.backend.prompts.size()));
}


// ---------------------------------------------------------------------------
// Model-proposed filesystem.read(path)
// ---------------------------------------------------------------------------

namespace fs = std::filesystem;

// A temp folder holding note.txt and cipher.conf, with cipher.conf registered as a CIPHER-private file.
struct FsHarness : Harness {
    fs::path root;

    FsHarness() {
        static int counter = 0;
        root = fs::temp_directory_path() / ("cipher_model_fs_" + std::to_string(++counter));
        fs::remove_all(root);
        fs::create_directories(root);
        write("note.txt", "hello from the file");
        write("cipher.conf", "secret=1");
        cipher::register_builtin_tools(registry);
        cipher::register_filesystem_tools(registry, root.string(), {(root / "cipher.conf").string()});
    }
    ~FsHarness() { std::error_code ignored; fs::remove_all(root, ignored); }

    void write(const std::string& name, const std::string& text) {
        std::ofstream(root / name, std::ios::binary) << text;
    }
    cipher::ModelToolLoop loop() { return make_loop(cipher::model_offered_tool_ids()); }
};

void test_fs_valid_read_needs_yes_and_reaches_model() {
    FsHarness h;
    h.backend.replies = {"TOOL_CALL: filesystem.read note.txt", "The file greets you."};
    cipher::ModelToolLoop loop = h.loop();

    check(starts_with(loop.run("what is in note.txt"), "The file greets you."), "fs read: final answer is returned",
          "answer missing");
    check(h.confirmations == 1, "fs read: owner asked exactly once",
          "confirmations were " + std::to_string(h.confirmations));
    check(fs::path(h.last_target).filename() == "note.txt" && fs::path(h.last_target).is_absolute(),
          "fs read: owner sees the resolved absolute target", "target was: " + h.last_target);
    check(h.backend.prompts.size() == 2 &&
              contains(h.backend.prompts[1], "Tool result for filesystem.read:\nhello from the file"),
          "fs read: file content goes back to the model", "second prompt lacked the content");
    check(contains(h.backend.prompts[0], "filesystem.read:"), "fs read: tool is listed in the prompt",
          "prompt did not list filesystem.read");
    check(h.registry.audit_log().size() == 1 &&
              h.registry.audit_log()[0].origin == cipher::CallOrigin::Model &&
              h.registry.audit_log()[0].executed,
          "fs read: audit record is origin=model", "audit record missing");
}

void test_fs_path_with_spaces_is_one_argument() {
    FsHarness h;
    h.write("my notes.txt", "spaced");
    h.backend.replies = {"TOOL_CALL: filesystem.read my notes.txt", "ok"};
    cipher::ModelToolLoop loop = h.loop();

    loop.run("q");
    check(h.backend.prompts.size() == 2 && contains(h.backend.prompts[1], "\nspaced"),
          "fs read: a path containing spaces is read whole", "file was not read");
}

void test_fs_denied_or_unanswered_confirmation_never_reads() {
    {
        FsHarness h;
        h.owner_answer = false;
        h.backend.replies = {"TOOL_CALL: filesystem.read note.txt"};
        cipher::ModelToolLoop loop = h.loop();
        const std::string text = loop.run("q");
        check(text == "Owner confirmation was not given: filesystem.read. Nothing was run." &&
                  h.registry.audit_log().empty() && h.backend.prompts.size() == 1,
              "fs read: owner 'no' reads nothing and sends nothing", "text was: " + text);
    }
    {
        // No confirmation callback at all behaves like a refusal.
        FsHarness h;
        h.backend.replies = {"TOOL_CALL: filesystem.read note.txt"};
        cipher::ModelToolLoop loop(h.backend, h.registry, cipher::ModelToolConfirmation{}, 30.0,
                                   cipher::model_offered_tool_ids());
        const std::string text = loop.run("q");
        check(contains(text, "Owner confirmation was not given") && h.registry.audit_log().empty(),
              "fs read: missing confirmation callback reads nothing", "text was: " + text);
    }
}

void test_fs_bad_arguments_run_nothing() {
    const char* bad[] = {"TOOL_CALL: filesystem.read",                                   // missing path
                         "TOOL_CALL: filesystem.read    ",                              // blank path
                         "TOOL_CALL: filesystem.read note.txt\nTOOL_CALL: time.now",   // two lines
                         "TOOL_CALL: filesystem.read note\t.txt",                      // control character
                         "TOOL_CALL: time.now note.txt",                               // argument on a bare tool
                         "TOOL_CALL: system.info x"};
    for (const char* reply : bad) {
        FsHarness h;
        h.backend.replies = {reply};
        cipher::ModelToolLoop loop = h.loop();
        const std::string text = loop.run("q");
        check(text == "The model's tool request was malformed. Nothing was run." && h.confirmations == 0 &&
                  h.registry.audit_log().empty(),
              std::string("fs bad argument: nothing runs for ") + reply, "text was: " + text);
    }
    FsHarness h;
    h.backend.replies = {"TOOL_CALL: filesystem.read " + std::string(cipher::ModelToolLoop::kMaxArgumentBytes + 1, 'a')};
    cipher::ModelToolLoop loop = h.loop();
    check(contains(loop.run("q"), "malformed") && h.confirmations == 0,
          "fs bad argument: an over-long path is malformed", "over-long path was accepted");
}

void test_fs_hostile_paths_are_refused_before_asking_owner() {
    const char* hostile[] = {"../outside.txt", "sub/../note.txt", "/etc/passwd", "C:/Windows/win.ini",
                             "C:\\Windows\\win.ini", "\\\\server\\share\\f.txt", "note.txt:stream",
                             "cipher.conf", "./cipher.conf", "missing.txt", "."};
    for (const char* path : hostile) {
        FsHarness h;
        h.backend.replies = {std::string("TOOL_CALL: filesystem.read ") + path};
        cipher::ModelToolLoop loop = h.loop();
        const std::string text = loop.run("q");
        check(text.rfind("Denied: ", 0) == 0 && h.confirmations == 0 && h.registry.audit_log().empty() &&
                  static_cast<int>(h.backend.prompts.size()) == cipher::ModelToolLoop::kMaxToolFailures,
              std::string("fs hostile path: refused without asking or reading: ") + path, "text was: " + text);
        check(!contains(text, "secret=1"), std::string("fs hostile path: no content leaks: ") + path,
              "content leaked");
    }
}

void test_fs_private_file_via_hard_link_is_refused() {
    FsHarness h;
    std::error_code error;
    fs::create_hard_link(h.root / "cipher.conf", h.root / "alias.txt", error);
    if (error) { pass("fs private file: hard link unsupported here, skipped"); return; }
    h.backend.replies = {"TOOL_CALL: filesystem.read alias.txt"};
    cipher::ModelToolLoop loop = h.loop();
    const std::string text = loop.run("q");
    check(text.rfind("Denied: ", 0) == 0 && h.confirmations == 0,
          "fs private file: hard link to cipher.conf is refused", "text was: " + text);
}

void test_fs_symlink_is_refused() {
    FsHarness h;
    std::error_code error;
    fs::create_symlink(h.root / "note.txt", h.root / "link.txt", error);
    if (error) { pass("fs symlink: symlinks unsupported here, skipped"); return; }
    h.backend.replies = {"TOOL_CALL: filesystem.read link.txt"};
    cipher::ModelToolLoop loop = h.loop();
    const std::string text = loop.run("q");
    check(text.rfind("Denied: ", 0) == 0 && h.confirmations == 0, "fs symlink: link is refused before asking",
          "text was: " + text);
}

void test_fs_policy_block_is_refused_without_asking() {
    FsHarness h;
    h.registry.policy().add_blacklist_pattern("filesystem.read*");
    h.backend.replies = {"TOOL_CALL: filesystem.read note.txt"};
    cipher::ModelToolLoop loop = h.loop();
    const std::string text = loop.run("q");
    check(contains(text, "Denied by permission policy (block): filesystem.read") && h.confirmations == 0,
          "fs read: a policy block is refused without asking", "text was: " + text);
}

void test_fs_output_cap_is_enforced() {
    FsHarness h;
    h.write("big.txt", std::string(cipher::ModelToolLoop::kMaxToolOutputBytes + 1, 'x'));
    h.backend.replies = {"TOOL_CALL: filesystem.read big.txt"};
    cipher::ModelToolLoop loop = h.loop();
    const std::string text = loop.run("q");
    check(contains(text, "larger than 4096 bytes and was not sent to the model") &&
              h.backend.prompts.size() == 1,
          "fs cap: a file over 4096 bytes is withheld whole", "text was: " + text);

    FsHarness edge;
    edge.write("edge.txt", std::string(cipher::ModelToolLoop::kMaxToolOutputBytes, 'y'));
    edge.backend.replies = {"TOOL_CALL: filesystem.read edge.txt", "done"};
    cipher::ModelToolLoop edge_loop = edge.loop();
    check(starts_with(edge_loop.run("q"), "done"), "fs cap: a file of exactly 4096 bytes is sent", "edge file was withheld");
}

void test_fs_read_refused_on_non_local_route() {
    FsHarness h;
    h.backend.route = cipher::ProcessingRoute::Unknown;
    h.backend.replies = {"TOOL_CALL: filesystem.read note.txt"};
    cipher::ModelToolLoop loop = h.loop();
    const std::string text = loop.run("q");
    check(contains(text, "not a verified local route") && h.backend.prompts.empty() && h.confirmations == 0,
          "fs route: nothing is read or sent over a non-local route", "text was: " + text);
}

void test_fs_unregistered_tool_is_not_offered() {
    Harness h;
    h.backend.replies = {"TOOL_CALL: filesystem.read note.txt"};
    cipher::register_builtin_tools(h.registry);
    cipher::ModelToolLoop loop = h.make_loop(cipher::model_offered_tool_ids());
    check(contains(loop.run("q"), "which is not offered") && !contains(h.backend.prompts[0], "filesystem.read"),
          "fs offered: without a configured root the tool is neither listed nor runnable",
          "filesystem.read leaked into the prompt or ran");
}

// Same loop, real OllamaBackend, fake HTTP transport: the Ollama path end to end.
struct FakeOllama {
    std::vector<std::string> generate_replies;
    std::vector<std::string> generate_bodies;

    cipher::HttpResult operator()(int, const std::string&, const std::string& path, const std::string& body, double) {
        cipher::HttpResult result;
        result.outcome = cipher::HttpOutcome::Ok;
        result.status_code = 200;
        if (path != "/api/generate") { result.body = "{}"; return result; }
        const std::size_t index = std::min(generate_bodies.size(), generate_replies.size() - 1);
        generate_bodies.push_back(body);
        result.body = "{\"model\":\"m\",\"response\":\"" + generate_replies[index] + "\",\"done\":true}";
        return result;
    }
};

void test_fs_read_through_ollama_backend() {
    FsHarness h;
    FakeOllama server;
    server.generate_replies = {"TOOL_CALL: filesystem.read note.txt", "It says hello."};
    cipher::OllamaBackend ollama(cipher::OllamaConfig{}, std::ref(server));
    cipher::ModelToolLoop loop(ollama, h.registry,
        [&h](const std::string&, const std::string&, const std::string&) { ++h.confirmations; return true; },
        30.0, cipher::model_offered_tool_ids());

    check(starts_with(loop.run("what does note.txt say"), "It says hello."), "ollama: end-to-end answer is returned",
          "answer missing");
    check(h.confirmations == 1, "ollama: owner asked once", "confirmations were " + std::to_string(h.confirmations));
    check(server.generate_bodies.size() == 2 && contains(server.generate_bodies[1], "hello from the file"),
          "ollama: file content is only in the follow-up request to the local server", "content missing");
    check(!server.generate_bodies.empty() && !contains(server.generate_bodies[0], "hello from the file"),
          "ollama: the first request carries no file content", "content in first request");
}

// llama.cpp cannot be driven without a model file; this scripted backend reports the same capabilities
// (VerifiedLocal, no native tools) and proves the loop is backend-agnostic. The real in-process backend
// is wired to the same loop in main.cpp.
class LlamaLikeBackend : public ScriptedBackend {
public:
    std::string backend_id() const override { return "llama_cpp"; }
};

void test_fs_read_through_llama_like_backend() {
    FsHarness h;
    LlamaLikeBackend llama;
    llama.replies = {"TOOL_CALL: filesystem.read note.txt", "Llama read it."};
    cipher::ModelToolLoop loop(llama, h.registry,
        [&h](const std::string&, const std::string&, const std::string&) { ++h.confirmations; return true; },
        30.0, cipher::model_offered_tool_ids());

    check(starts_with(loop.run("q"), "Llama read it.") && h.confirmations == 1,
          "llama: same TOOL_CALL protocol works without native tool calling", "loop did not complete");
    check(llama.prompts.size() == 2 && contains(llama.prompts[1], "hello from the file"),
          "llama: result reaches the model on the next round", "content missing");
}

} // namespace

void test_v1_multi_step_list_then_read_with_trace() {
    FsHarness h;
    h.backend.replies = {"TOOL_CALL: filesystem.list", "TOOL_CALL: filesystem.read note.txt", "It says hello."};
    cipher::ModelToolLoop loop = h.loop();
    const std::string text = loop.run("inspect the folder");
    check(starts_with(text, "It says hello.") && contains(text, "filesystem.list") && contains(text, "filesystem.read note.txt") &&
              contains(text, "not verified"),
          "v1 chain: list then read then answer carries a CIPHER trace", "text was: " + text);
    check(h.confirmations == 2, "v1 chain: every call asked the owner", "confirmations were " + std::to_string(h.confirmations));
    check(contains(h.backend.prompts[1], "note.txt") && !contains(h.backend.prompts[1], "cipher.conf"),
          "v1 list: names shown, private file hidden", "listing was wrong");
}

void test_v1_named_unread_file_is_read_before_answer() {
    FsHarness h;
    h.backend.replies = {"TOOL_CALL: filesystem.list", "It says hello.", "TOOL_CALL: filesystem.read note.txt", "It says hello."};
    cipher::ModelToolLoop loop = h.loop();
    const std::string text = loop.run("What is in the folder? Then tell me what note.txt says.");
    check(starts_with(text, "It says hello.") && contains(text, "filesystem.read note.txt") && h.confirmations == 2 &&
              h.backend.prompts.size() == 4 && contains(h.backend.prompts[2], "TOOL_CALL: filesystem.read note.txt"),
          "v1 verify: an answer about a listed file waits until that file is read", "text was: " + text);
}

void test_v1_named_unread_file_stops_after_budget() {
    FsHarness h;
    h.backend.replies = {"TOOL_CALL: filesystem.list", "It says hello.", "It says bye."};
    cipher::ModelToolLoop loop = h.loop();
    const std::string text = loop.run("What is in the folder? Then tell me what note.txt says.");
    check(starts_with(text, "Not verified: the question asks about note.txt") && h.confirmations == 1,
          "v1 verify: ungrounded answers stop within the failure budget", "text was: " + text);
}

void test_v1_recovery_after_refused_path() {
    FsHarness h;
    h.backend.replies = {"TOOL_CALL: filesystem.read ../x", "TOOL_CALL: filesystem.read note.txt", "Recovered."};
    cipher::ModelToolLoop loop = h.loop();
    const std::string text = loop.run("q");
    check(starts_with(text, "Recovered.") && h.confirmations == 1 && contains(h.backend.prompts[1], "was not run"),
          "v1 recovery: a refused call is reported and the model may correct itself", "text was: " + text);
}

void test_v1_malformed_reply_gets_correction() {
    Harness h;
    h.backend.replies = {"TOOL_CALL: time.now\nTOOL_CALL: system.info", "TOOL_CALL: time.now", "It is noon."};
    cipher::register_builtin_tools(h.registry);
    cipher::ModelToolLoop loop = h.make_loop(cipher::model_offered_tool_ids());
    const std::string text = loop.run("q");
    check(starts_with(text, "It is noon.") && h.backend.prompts.size() == 3 &&
              contains(h.backend.prompts[1], "not one valid tool request") && h.registry.audit_log().size() == 1,
          "v1 malformed: a two-line reply is corrected and nothing from it runs", "text was: " + text);
}

void test_v1_repeat_failures_stop() {
    FsHarness h;
    h.backend.replies = {"TOOL_CALL: filesystem.read missing.txt"};
    cipher::ModelToolLoop loop = h.loop();
    const std::string text = loop.run("q");
    check(starts_with(text, "Denied: ") && h.confirmations == 0, "v1 recovery: failures are bounded", "text was: " + text);
}

void test_v1_list_refuses_escape() {
    FsHarness h;
    h.backend.replies = {"TOOL_CALL: filesystem.list ../"};
    cipher::ModelToolLoop loop = h.loop();
    const std::string text = loop.run("q");
    check(starts_with(text, "Denied: ") && h.confirmations == 0, "v1 list: parent directory refused", "text was: " + text);
}

void test_v1_memory_search_governed() {
    const fs::path memory_file = fs::temp_directory_path() / "cipher_v1_memory.json";
    {
        cipher::MemoryStore store;
        store.set("alpha", "the launch code is zebra", "general");
        store.save(memory_file.string());
    }
    for (int denied = 0; denied < 2; ++denied) {
        Harness h;
        cipher::PermissionPolicy policy;
        if (denied) policy.add_blacklist_pattern("memory");
        cipher::register_memory_tools(h.registry, memory_file.string(), policy);
        h.backend.replies = {"TOOL_CALL: memory.search zebra", "done"};
        cipher::ModelToolLoop loop = h.make_loop(cipher::model_offered_tool_ids());
        const std::string text = loop.run("q");
        const bool leaked = contains(h.backend.prompts.size() > 1 ? h.backend.prompts[1] : "", "launch code");
        check(h.confirmations == 1 && h.last_target == "zebra" && leaked == (denied == 0),
              denied ? "v1 memory: deny policy hides entries from the model" : "v1 memory: search returns matches after yes",
              "text was: " + text);
    }
    std::error_code ignored;
    fs::remove(memory_file, ignored);
}

void test_v1_memory_share_deny_hides_entries() {
    const fs::path memory_file = fs::temp_directory_path() / "cipher_v1_share_memory.json";
    {
        cipher::MemoryStore store;
        store.set("alpha", "the launch code is zebra", "general");
        store.save(memory_file.string());
    }
    Harness h;
    cipher::PermissionPolicy policy;
    policy.add_blacklist_pattern("share *");
    cipher::register_memory_tools(h.registry, memory_file.string(), policy);
    h.backend.replies = {"TOOL_CALL: memory.search zebra", "done"};
    cipher::ModelToolLoop loop = h.make_loop(cipher::model_offered_tool_ids());
    loop.run("q");
    check(h.backend.prompts.size() > 1 && !contains(h.backend.prompts[1], "launch code"),
          "v1 memory: memory_deny share * keeps snippets from the model",
          "prompt leaked the entry");
    std::error_code ignored;
    fs::remove(memory_file, ignored);
}

void test_v1_read_in_subfolder_verifies_listed_file() {
    FsHarness h;
    fs::create_directories(h.root / "sub");
    h.write("sub/note.txt", "hello from the subfolder");
    h.backend.replies = {"TOOL_CALL: filesystem.list sub", "It says hello.", "TOOL_CALL: filesystem.read sub/note.txt", "It says hello."};
    cipher::ModelToolLoop loop = h.loop();
    const std::string text = loop.run("What is in sub? Then tell me what note.txt says.");
    check(starts_with(text, "It says hello.") && h.confirmations == 2 && h.backend.prompts.size() == 4,
          "v1 verify: a read of the listed subfolder path satisfies the check", "text was: " + text);
}

void test_v1_question_name_match_ignores_case() {
    FsHarness h;
    h.backend.replies = {"TOOL_CALL: filesystem.list", "It says hello.", "TOOL_CALL: filesystem.read note.txt", "It says hello."};
    cipher::ModelToolLoop loop = h.loop();
    const std::string text = loop.run("What is in the folder? Then tell me what NOTE.TXT says.");
    check(starts_with(text, "It says hello.") && h.confirmations == 2 && h.backend.prompts.size() == 4,
          "v1 verify: a named file in different case still waits for a read", "text was: " + text);
}

void test_v1_last_round_answer_is_not_sent_back_for_correction() {
    FsHarness h;
    h.backend.replies = {"TOOL_CALL: filesystem.list", "TOOL_CALL: filesystem.list", "TOOL_CALL: filesystem.list",
                         "TOOL_CALL: filesystem.list", "It says hello."};
    cipher::ModelToolLoop loop = h.loop();
    const std::string text = loop.run("What is in the folder? Then tell me what note.txt says.");
    check(starts_with(text, "Not verified: the question asks about note.txt") && h.confirmations == 1 &&
              h.backend.prompts.size() == 5,
          "v1 verify: an unverified answer in the last round stops at once", "text was: " + text);
}

void test_unoffered_after_success_names_the_ran_call() {
    Harness h;
    h.backend.replies = {"TOOL_CALL: time.now", "TOOL_CALL: filesystem.read note.txt"};
    cipher::register_builtin_tools(h.registry);
    cipher::ModelToolLoop loop = h.make_loop({"time.now"});

    const std::string text = loop.run("q");
    check(text == "The model asked for tool 'filesystem.read', which is not offered. "
                  "Calls that ran earlier in this turn: time.now.",
          "early exit: an unoffered request after a run names the call that ran", "text was: " + text);
    check(!contains(text, "Nothing was run."), "early exit: no false 'Nothing was run.' after a run",
          "text was: " + text);
}

void test_malformed_after_success_names_the_ran_call() {
    Harness h;
    h.backend.replies = {"TOOL_CALL: time.now", "TOOL_CALL: time.now please"};
    cipher::register_builtin_tools(h.registry);
    cipher::ModelToolLoop loop = h.make_loop(cipher::model_offered_tool_ids());

    const std::string text = loop.run("q");
    check(text == "The model's tool request was malformed. Calls that ran earlier in this turn: time.now.",
          "early exit: a malformed request after a run names the call that ran", "text was: " + text);
}

void test_policy_block_after_success_names_the_ran_call() {
    Harness h;
    h.backend.replies = {"TOOL_CALL: time.now", "TOOL_CALL: test.blocked"};
    cipher::register_builtin_tools(h.registry);
    cipher::ToolDefinition blocked;
    blocked.id = "test.blocked";
    blocked.description = "test-only blocked tool";
    blocked.category = "test";
    blocked.default_tier = cipher::RiskTier::Block;
    blocked.handler = [](const std::string&) { return std::string("ran"); };
    h.registry.register_tool(blocked);
    cipher::ModelToolLoop loop = h.make_loop({"time.now", "test.blocked"});

    const std::string text = loop.run("q");
    check(text == "Denied by permission policy (block): test.blocked. Calls that ran earlier in this turn: time.now.",
          "early exit: a policy block after a run names the call that ran", "text was: " + text);
}

void test_owner_refusal_after_success_names_the_ran_call() {
    Harness h;
    h.backend.replies = {"TOOL_CALL: time.now", "TOOL_CALL: system.info"};
    cipher::register_builtin_tools(h.registry);
    int asked = 0;
    cipher::ModelToolLoop loop(h.backend, h.registry,
        [&asked](const std::string&, const std::string&, const std::string&) { return asked++ == 0; },
        30.0, {"time.now", "system.info"});

    const std::string text = loop.run("q");
    check(text == "Owner confirmation was not given: system.info. Calls that ran earlier in this turn: time.now.",
          "early exit: an owner refusal after a run names the call that ran", "text was: " + text);
}

int main() {
    test_v1_memory_share_deny_hides_entries();
    test_v1_read_in_subfolder_verifies_listed_file();
    test_v1_question_name_match_ignores_case();
    test_v1_last_round_answer_is_not_sent_back_for_correction();
    test_plain_answer_passes_through_without_tools();
    test_yes_runs_tool_and_feeds_result_back();
    test_no_runs_nothing();
    test_block_tier_is_denied_without_asking_owner();
    test_unknown_and_unoffered_tools_run_nothing();
    test_malformed_requests_run_nothing();
    test_round_cap_stops_repeated_requests();
    test_non_local_route_withholds_tool_output();
    test_memory_list_is_never_offered();
    test_oversized_output_is_withheld();
    test_fs_valid_read_needs_yes_and_reaches_model();
    test_fs_path_with_spaces_is_one_argument();
    test_fs_denied_or_unanswered_confirmation_never_reads();
    test_fs_bad_arguments_run_nothing();
    test_fs_hostile_paths_are_refused_before_asking_owner();
    test_fs_private_file_via_hard_link_is_refused();
    test_fs_symlink_is_refused();
    test_fs_policy_block_is_refused_without_asking();
    test_fs_output_cap_is_enforced();
    test_fs_read_refused_on_non_local_route();
    test_fs_unregistered_tool_is_not_offered();
    test_fs_read_through_ollama_backend();
    test_fs_read_through_llama_like_backend();
    test_v1_multi_step_list_then_read_with_trace();
    test_v1_named_unread_file_is_read_before_answer();
    test_v1_named_unread_file_stops_after_budget();
    test_v1_recovery_after_refused_path();
    test_v1_malformed_reply_gets_correction();
    test_v1_repeat_failures_stop();
    test_v1_list_refuses_escape();
    test_v1_memory_search_governed();
    test_unoffered_after_success_names_the_ran_call();
    test_malformed_after_success_names_the_ran_call();
    test_policy_block_after_success_names_the_ran_call();
    test_owner_refusal_after_success_names_the_ran_call();

    std::cout << "\n" << g_passes << " passed, " << g_failures << " failed\n";
    return g_failures == 0 ? 0 : 1;
}
