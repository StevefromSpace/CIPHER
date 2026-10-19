#include "cipher/assistant_backend.hpp"

#include <iostream>
#include <limits>
#include <string>

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

// ---------------------------------------------------------------------------
// Mock Assistant Backends for Testing
// ---------------------------------------------------------------------------
class MockLocalBackend : public cipher::IAssistantBackend {
public:
    std::string backend_id() const override { return "mock_local_backend"; }
    bool is_available() override { return true; }
    cipher::AssistantCapabilities capabilities() const override {
        cipher::AssistantCapabilities caps;
        caps.supports_text_conversation = true;
        caps.supports_sessions = true;
        caps.processing_route = cipher::ProcessingRoute::VerifiedLocal;
        return caps;
    }
    cipher::AssistantResponse execute_request(const cipher::AssistantRequest& request) override {
        cipher::AssistantResponse resp;
        resp.status = cipher::AssistantStatus::Ok;
        resp.text = "Echo: " + request.prompt;
        resp.model = request.model_override.value_or("local-model-v1");
        resp.duration_ms = 12.5;
        return resp;
    }
};

class MockCloudBackend : public cipher::IAssistantBackend {
public:
    std::string backend_id() const override { return "mock_cloud_backend"; }
    bool is_available() override { return true; }
    cipher::AssistantCapabilities capabilities() const override {
        cipher::AssistantCapabilities caps;
        caps.supports_text_conversation = true;
        caps.processing_route = cipher::ProcessingRoute::RemoteCloud;
        return caps;
    }
    cipher::AssistantResponse execute_request(const cipher::AssistantRequest& request) override {
        cipher::AssistantResponse resp;
        resp.status = cipher::AssistantStatus::Ok;
        resp.text = "Cloud reply: " + request.prompt;
        resp.model = "cloud-model-v1";
        return resp;
    }
};

class MockUnknownRouteBackend : public cipher::IAssistantBackend {
public:
    std::string backend_id() const override { return "mock_unverified_backend"; }
    bool is_available() override { return true; }
    cipher::AssistantCapabilities capabilities() const override {
        cipher::AssistantCapabilities caps;
        caps.processing_route = cipher::ProcessingRoute::Unknown;
        return caps;
    }
    cipher::AssistantResponse execute_request(const cipher::AssistantRequest&) override {
        cipher::AssistantResponse resp;
        resp.status = cipher::AssistantStatus::Ok;
        return resp;
    }
};

class MockUnavailableBackend : public cipher::IAssistantBackend {
public:
    std::string backend_id() const override { return "mock_offline_backend"; }
    bool is_available() override { return false; }
    cipher::AssistantCapabilities capabilities() const override {
        cipher::AssistantCapabilities caps;
        caps.processing_route = cipher::ProcessingRoute::VerifiedLocal;
        return caps;
    }
    cipher::AssistantResponse execute_request(const cipher::AssistantRequest&) override {
        cipher::AssistantResponse resp;
        resp.status = cipher::AssistantStatus::Error;
        return resp;
    }
};

// ---------------------------------------------------------------------------
// Test Invariant 1: Prompt Validation
// ---------------------------------------------------------------------------
void test_prompt_validation() {
    cipher::AssistantCapabilities caps;
    caps.processing_route = cipher::ProcessingRoute::VerifiedLocal;

    // 1. Empty prompt rejected
    {
        cipher::AssistantRequest req;
        req.prompt = "";
        auto res = cipher::validate_assistant_request(req, caps);
        check(!res.is_valid, "empty prompt rejected", "Expected invalid result");
        check(res.error_message.find("empty") != std::string::npos,
              "empty prompt error message mentions empty", "Unexpected message: " + res.error_message);
    }

    // 2. Whitespace-only prompt rejected
    {
        cipher::AssistantRequest req;
        req.prompt = "   \t\n\r  ";
        auto res = cipher::validate_assistant_request(req, caps);
        check(!res.is_valid, "whitespace-only prompt rejected", "Expected invalid result");
    }

    // 3. Valid prompt accepted
    {
        cipher::AssistantRequest req;
        req.prompt = "Hello CIPHER";
        auto res = cipher::validate_assistant_request(req, caps);
        check(res.is_valid, "valid prompt accepted", "Expected valid result: " + res.error_message);
    }
}

// ---------------------------------------------------------------------------
// Test Invariant 2: Timeout Bounds & Non-Finite Rejection
// ---------------------------------------------------------------------------
void test_timeout_bounds() {
    cipher::AssistantCapabilities caps;
    caps.processing_route = cipher::ProcessingRoute::VerifiedLocal;

    // 1. Sub-second timeout rejected
    {
        cipher::AssistantRequest req;
        req.prompt = "Valid prompt";
        req.timeout_seconds = 0.5;
        auto res = cipher::validate_assistant_request(req, caps);
        check(!res.is_valid, "timeout < 1.0s rejected", "Expected invalid result");
    }

    // 2. Excessive timeout (> 300s) rejected
    {
        cipher::AssistantRequest req;
        req.prompt = "Valid prompt";
        req.timeout_seconds = 301.0;
        auto res = cipher::validate_assistant_request(req, caps);
        check(!res.is_valid, "timeout > 300.0s rejected", "Expected invalid result");
    }

    // 3. Valid timeout boundary values
    {
        cipher::AssistantRequest req;
        req.prompt = "Valid prompt";
        req.timeout_seconds = 1.0;
        check(cipher::validate_assistant_request(req, caps).is_valid, "timeout 1.0s accepted", "Expected valid");

        req.timeout_seconds = 300.0;
        check(cipher::validate_assistant_request(req, caps).is_valid, "timeout 300.0s accepted", "Expected valid");

        req.timeout_seconds = 30.0;
        check(cipher::validate_assistant_request(req, caps).is_valid, "timeout 30.0s accepted", "Expected valid");
    }

    // 4. Non-finite timeouts (NaN, +Inf, -Inf) rejected fail-closed
    {
        cipher::AssistantRequest req;
        req.prompt = "Valid prompt";
        req.timeout_seconds = std::numeric_limits<double>::quiet_NaN();
        auto res_nan = cipher::validate_assistant_request(req, caps);
        check(!res_nan.is_valid, "NaN timeout rejected fail-closed", "Expected invalid result");

        req.timeout_seconds = std::numeric_limits<double>::infinity();
        auto res_inf = cipher::validate_assistant_request(req, caps);
        check(!res_inf.is_valid, "+Inf timeout rejected fail-closed", "Expected invalid result");

        req.timeout_seconds = -std::numeric_limits<double>::infinity();
        auto res_ninf = cipher::validate_assistant_request(req, caps);
        check(!res_ninf.is_valid, "-Inf timeout rejected fail-closed", "Expected invalid result");
    }
}

// ---------------------------------------------------------------------------
// Test Invariant 3: Tool Lockdown
// ---------------------------------------------------------------------------
void test_tool_request_lockdown() {
    cipher::AssistantCapabilities caps;
    caps.processing_route = cipher::ProcessingRoute::VerifiedLocal;

    // 1. Tool execution request rejected
    {
        cipher::AssistantRequest req;
        req.prompt = "Execute tool command";
        req.request_tools = true;
        auto res = cipher::validate_assistant_request(req, caps);
        check(!res.is_valid, "request_tools=true rejected", "Expected invalid result");
        check(res.error_message.find("prohibited") != std::string::npos,
              "tool lockdown error message explains policy", "Unexpected message: " + res.error_message);
    }

    // 2. Default request_tools=false accepted
    {
        cipher::AssistantRequest req;
        req.prompt = "Pure conversation";
        req.request_tools = false;
        auto res = cipher::validate_assistant_request(req, caps);
        check(res.is_valid, "request_tools=false accepted", "Expected valid result");
    }
}

// ---------------------------------------------------------------------------
// Test Invariant 4: Local-Only Route Verification
// ---------------------------------------------------------------------------
void test_local_only_routing_invariants() {
    // 1. local_only with VerifiedLocal succeeds
    {
        cipher::AssistantCapabilities caps;
        caps.processing_route = cipher::ProcessingRoute::VerifiedLocal;

        cipher::AssistantRequest req;
        req.prompt = "Local query";
        req.local_only = true;
        auto res = cipher::validate_assistant_request(req, caps);
        check(res.is_valid, "local_only with VerifiedLocal route accepted", "Expected valid result");
    }

    // 2. local_only with RemoteCloud rejected
    {
        cipher::AssistantCapabilities caps;
        caps.processing_route = cipher::ProcessingRoute::RemoteCloud;

        cipher::AssistantRequest req;
        req.prompt = "Local query";
        req.local_only = true;
        auto res = cipher::validate_assistant_request(req, caps);
        check(!res.is_valid, "local_only with RemoteCloud rejected", "Expected invalid result");
    }

    // 3. local_only with Unknown route rejected (fail-closed)
    {
        cipher::AssistantCapabilities caps;
        caps.processing_route = cipher::ProcessingRoute::Unknown;

        cipher::AssistantRequest req;
        req.prompt = "Local query";
        req.local_only = true;
        auto res = cipher::validate_assistant_request(req, caps);
        check(!res.is_valid, "local_only with Unknown route rejected", "Expected invalid result");
    }

    // 4. local_only=false allows RemoteCloud
    {
        cipher::AssistantCapabilities caps;
        caps.processing_route = cipher::ProcessingRoute::RemoteCloud;

        cipher::AssistantRequest req;
        req.prompt = "Cloud query";
        req.local_only = false;
        auto res = cipher::validate_assistant_request(req, caps);
        check(res.is_valid, "local_only=false allows RemoteCloud", "Expected valid result");
    }
}

// ---------------------------------------------------------------------------
// Test Invariant 5: Context Budget Prompt Bounding
// ---------------------------------------------------------------------------
void test_context_budget_prompt_bounding() {
    cipher::AssistantCapabilities caps;
    caps.processing_route = cipher::ProcessingRoute::VerifiedLocal;

    cipher::AssistantRequest req;
    req.budget.max_prompt_bytes = 100;

    // Small prompt within budget
    req.prompt = "Small prompt";
    check(cipher::validate_assistant_request(req, caps).is_valid,
          "prompt within budget accepted", "Expected valid");

    // Oversized prompt exceeding budget
    req.prompt = std::string(150, 'X');
    auto res_oversized = cipher::validate_assistant_request(req, caps);
    check(!res_oversized.is_valid, "oversized prompt exceeding budget rejected", "Expected invalid");
    check(res_oversized.error_message.find("Context budget violation") != std::string::npos,
          "budget error message contains Context budget violation", "Unexpected message: " + res_oversized.error_message);
}

// ---------------------------------------------------------------------------
// Test AssistantManager Central Dispatch
// ---------------------------------------------------------------------------
void test_assistant_manager_dispatch() {
    // 1. Null backend fails gracefully
    {
        cipher::AssistantRequest req;
        req.prompt = "Test";
        auto resp = cipher::AssistantManager::submit(nullptr, req);
        check(resp.status == cipher::AssistantStatus::Unavailable,
              "null backend returns Unavailable", "Expected Unavailable");
    }

    // 2. Unavailable backend fails gracefully
    {
        MockUnavailableBackend offline_backend;
        cipher::AssistantRequest req;
        req.prompt = "Test";
        auto resp = cipher::AssistantManager::submit(&offline_backend, req);
        check(resp.status == cipher::AssistantStatus::Unavailable,
              "offline backend returns Unavailable", "Expected Unavailable");
    }

    // 3. Policy violation blocks backend execution
    {
        MockLocalBackend local_backend;
        cipher::AssistantRequest req;
        req.prompt = "   "; // Invalid blank prompt
        auto resp = cipher::AssistantManager::submit(&local_backend, req);
        check(resp.status == cipher::AssistantStatus::PolicyViolation,
              "policy violation returns PolicyViolation status", "Expected PolicyViolation");
        check(!resp.is_ok(), "is_ok() is false on PolicyViolation", "Expected false");
    }

    // 4. Successful local execution
    {
        MockLocalBackend local_backend;
        cipher::AssistantRequest req;
        req.prompt = "What is CIPHER?";
        req.local_only = true;
        auto resp = cipher::AssistantManager::submit(&local_backend, req);
        check(resp.status == cipher::AssistantStatus::Ok,
              "valid local request returns Ok", "Expected Ok status");
        check(resp.is_ok(), "is_ok() is true", "Expected true");
        check(resp.text == "Echo: What is CIPHER?",
              "returned text matches backend output", "Unexpected text: " + resp.text);
        check(resp.duration_ms > 0.0, "duration_ms populated", "Expected non-zero duration");
    }
}

// ---------------------------------------------------------------------------
// Test Dodaios-Inspired PermissionPolicy & RiskTier
// ---------------------------------------------------------------------------
void test_risk_tier_and_permission_policy() {
    cipher::PermissionPolicy policy;
    policy.set_default_tier(cipher::RiskTier::Ask);

    // Add rules
    policy.add_blacklist_pattern("run_shell rm -rf*");
    policy.add_blacklist_pattern("exec*");
    policy.add_blacklist_pattern("format*");
    policy.add_whitelist_pattern("read_file*");
    policy.add_whitelist_pattern("time");

    // 1. Blacklist evaluation
    cipher::ActionDescriptor blocked_action{"run_shell", "rm -rf /", "system", cipher::RiskTier::Ask};
    check(policy.evaluate(blocked_action) == cipher::RiskTier::Block,
          "blacklist match evaluates to RiskTier::Block", "Expected Block");
    check(!policy.is_permitted(blocked_action),
          "blocked action is not permitted", "Expected false");

    // 2. Blacklist beats whitelist (Dodaios non-negotiable priority order)
    policy.add_whitelist_pattern("exec*"); // Even if whitelisted, blacklist wins!
    cipher::ActionDescriptor conflict_action{"exec", "whoami", "system", cipher::RiskTier::Ask};
    check(policy.evaluate(conflict_action) == cipher::RiskTier::Block,
          "blacklist beats whitelist on conflict (Dodaios invariant)", "Expected Block");

    // 3. Whitelist downgrades tier to Safe
    cipher::ActionDescriptor safe_action{"read_file", "cipher.conf", "fs", cipher::RiskTier::Ask};
    check(policy.evaluate(safe_action) == cipher::RiskTier::Safe,
          "whitelist match evaluates to RiskTier::Safe", "Expected Safe");
    check(policy.is_permitted(safe_action),
          "safe action is permitted", "Expected true");

    // 4. Default tier fallback when no match
    cipher::ActionDescriptor unconfigured_action{"edit_memory", "key", "memory", cipher::RiskTier::Ask};
    check(policy.evaluate(unconfigured_action) == cipher::RiskTier::Ask,
          "unmatched action falls back to default Ask tier", "Expected Ask");
}

// ---------------------------------------------------------------------------
// Test OpenVoiceOS-Inspired EventMessage
// ---------------------------------------------------------------------------
void test_event_message_envelope() {
    cipher::EventMessage msg;
    msg.type = "recognizer_loop:utterance";
    msg.data = "{\"utterance\":\"hello\"}";
    msg.session_id = "session-12345";
    msg.source = "cipher";
    msg.destination = "assistant";
    msg.timestamp = 1729330000.0;

    check(msg.type == "recognizer_loop:utterance", "event message type", "mismatch");
    check(msg.session_id == "session-12345", "event message session_id", "mismatch");
    check(msg.source == "cipher", "event message source", "mismatch");
}

// ---------------------------------------------------------------------------
// Test Enum to String Conversions
// ---------------------------------------------------------------------------
void test_enum_to_string() {
    check(cipher::to_string(cipher::AssistantStatus::Ok) == "ok", "status ok string", "mismatch");
    check(cipher::to_string(cipher::AssistantStatus::PolicyViolation) == "policy_violation", "status policy violation string", "mismatch");
    check(cipher::to_string(cipher::ProcessingRoute::VerifiedLocal) == "verified_local", "route verified_local string", "mismatch");
    check(cipher::to_string(cipher::ProcessingRoute::RemoteCloud) == "remote_cloud", "route remote_cloud string", "mismatch");
    check(cipher::to_string(cipher::RiskTier::Safe) == "safe", "risk tier safe string", "mismatch");
    check(cipher::to_string(cipher::RiskTier::Block) == "block", "risk tier block string", "mismatch");
}

} // anonymous namespace

int main() {
    std::cout << "Running CIPHER Assistant Backend Subsystem Test Suite...\n\n";

    test_prompt_validation();
    test_timeout_bounds();
    test_tool_request_lockdown();
    test_local_only_routing_invariants();
    test_context_budget_prompt_bounding();
    test_assistant_manager_dispatch();
    test_risk_tier_and_permission_policy();
    test_event_message_envelope();
    test_enum_to_string();

    std::cout << "\n-------------------------------------------------------------\n";
    std::cout << "Tests passed: " << g_passes << '\n';
    std::cout << "Tests failed: " << g_failures << '\n';
    std::cout << "-------------------------------------------------------------\n";

    return (g_failures == 0) ? 0 : 1;
}
