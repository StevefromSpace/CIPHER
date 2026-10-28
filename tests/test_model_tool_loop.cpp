#include "cipher/model_tool_loop.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// test_model_tool_loop.cpp  —  model-proposed tool calls (v0.5 owner-gated loop)
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

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// Builds a loop around a scripted backend and records how often the owner was asked.
struct Harness {
    ScriptedBackend backend;
    cipher::ToolRegistry registry;
    int confirmations = 0;
    bool owner_answer = true;

    cipher::ModelToolLoop make_loop(const std::vector<std::string>& offered) {
        return cipher::ModelToolLoop(backend, registry,
            [this](const std::string&, const std::string&) {
                ++confirmations;
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

    check(loop.run("say pong") == "pong", "plain answer: text is returned",
          "text was not returned");
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

    check(loop.run("what time is it") == "It is noon.", "yes: final model answer is returned",
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
    check(text == "Tool call limit reached. Nothing was run.", "round cap: limit message is returned",
          "text was: " + text);
    check(static_cast<int>(h.backend.prompts.size()) == cipher::ModelToolLoop::kMaxModelRounds,
          "round cap: submits are bounded",
          "submits were " + std::to_string(h.backend.prompts.size()));
    check(h.confirmations == cipher::ModelToolLoop::kMaxModelRounds - 1,
          "round cap: owner is asked once per executed call only",
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

} // namespace

int main() {
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

    std::cout << "\n" << g_passes << " passed, " << g_failures << " failed\n";
    return g_failures == 0 ? 0 : 1;
}
