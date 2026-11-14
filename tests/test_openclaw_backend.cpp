#include "cipher/openclaw_backend.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& name) {
    if (ok) std::cerr << "PASS: " << name << '\n';
    else { std::cerr << "FAIL: " << name << '\n'; ++g_failures; }
}

using cipher::orchestration::ProcessResult;

// Scripted CLI: records every argv and answers the tool probe and the agent call separately.
struct FakeRunner : cipher::orchestration::ProcessRunner {
    ProcessResult tools{0, false, false, "[]", ""};
    ProcessResult agent{0, false, false, R"({"payloads":[{"text":"hello"}]})", ""};
    std::vector<std::vector<std::string>> calls;
    ProcessResult run(const std::vector<std::string>& argv, int) override {
        calls.push_back(argv);
        return argv.size() > 1 && argv[1] == "tools" ? tools : agent;
    }
};

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path fake_cli = fs::temp_directory_path() / "cipher_fake_openclaw_cli";
    { std::ofstream(fake_cli) << "x"; }

    auto make = [&](std::shared_ptr<FakeRunner> r, std::string path = {}) {
        cipher::OpenClawConfig cfg;
        cfg.cli_path = path.empty() ? fake_cli.string() : path;
        return cipher::OpenClawBackend(cfg, std::move(r));
    };
    cipher::AssistantRequest req;
    req.prompt = "hi";

    {
        auto r = std::make_shared<FakeRunner>();
        auto b = make(r);
        const auto resp = b.execute_request(req);
        check(b.is_available(), "an existing absolute CLI path is available");
        check(resp.status == cipher::AssistantStatus::Ok && resp.text == "hello", "reply text is read from payloads[0].text");
        check(r->calls.size() == 2 && r->calls[1][1] == "agent", "one tool probe then one agent call");
        check(r->calls[1].back() == "--json", "the agent call asks for JSON");
    }
    {
        auto r = std::make_shared<FakeRunner>();
        auto b = make(r, "relative/openclaw");
        check(!b.is_available() && b.execute_request(req).status == cipher::AssistantStatus::Unavailable
                  && r->calls.empty(), "a relative CLI path is refused without running anything");
    }
    {
        auto r = std::make_shared<FakeRunner>();
        auto b = make(r);
        auto hosted = req;
        hosted.model_override = "openai/gpt";
        check(b.execute_request(hosted).status == cipher::AssistantStatus::PolicyViolation && r->calls.empty(),
              "a non-ollama model is refused before any process runs");
        auto tooling = req;
        tooling.request_tools = true;
        check(b.execute_request(tooling).status == cipher::AssistantStatus::PolicyViolation && r->calls.empty(),
              "a tool request is refused before any process runs");
    }
    {
        auto r = std::make_shared<FakeRunner>();
        r->tools.stdout_text = R"({"tools":[{"name":"shell"}]})";
        auto b = make(r);
        check(b.execute_request(req).status == cipher::AssistantStatus::Unavailable && r->calls.size() == 1,
              "any exposed tool blocks the agent call");
        r->tools.stdout_text = "not json";
        check(b.execute_request(req).status == cipher::AssistantStatus::Unavailable,
              "an unreadable tool inventory fails closed");
    }
    {
        auto r = std::make_shared<FakeRunner>();
        r->agent.timed_out = true;
        check(make(r).execute_request(req).status == cipher::AssistantStatus::Timeout, "a timeout is reported as Timeout");
        r->agent = {1, false, false, "", ""};
        check(make(r).execute_request(req).status == cipher::AssistantStatus::Error, "a non-zero exit is an error");
        r->agent = {0, false, false, "{}", ""};
        check(make(r).execute_request(req).status == cipher::AssistantStatus::Error, "output without reply text is an error");
        r->agent = {0, false, true, "{}", ""};
        check(make(r).execute_request(req).status == cipher::AssistantStatus::Error, "output overflow is an error");
    }
    {
        auto r = std::make_shared<FakeRunner>();
        auto with_session = req;
        with_session.session_id = "s1";
        make(r).execute_request(with_session);
        const auto& argv = r->calls[1];
        check(argv.back() == "s1" && argv[argv.size() - 2] == "--session-id", "a session id is passed through");
    }

    std::error_code ec;
    fs::remove(fake_cli, ec);
    std::cerr << (g_failures == 0 ? "ALL PASSED\n" : "FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
