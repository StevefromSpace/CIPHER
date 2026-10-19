#include "cipher/assistant_backend.hpp"

#include <iostream>
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
// Test Invariant 1: Blank or Whitespace-Only Prompts
// ---------------------------------------------------------------------------
void test_prompt_validation() {
    cipher::AssistantCapabilities local_caps;
    local_caps.processing_route = cipher::ProcessingRoute::VerifiedLocal;

    {
        cipher::AssistantRequest req;
        req.prompt = "";
        auto res = cipher::validate_assistant_request(req, local_caps);
        check(!res.is_valid, "empty prompt rejected", "Expected failure on empty prompt");
        check(res.error_message.find("empty") != std::string::npos,
              "empty prompt error message mentions empty", "Missing descriptive error");
    }

    {
        cipher::AssistantRequest req;
        req.prompt = "    \t \n \r  ";
        auto res = cipher::validate_assistant_request(req, local_caps);
        check(!res.is_valid, "whitespace-only prompt rejected", "Expected failure on whitespace-only prompt");
    }

    {
        cipher::AssistantRequest req;
        req.prompt = "What is the status?";
        auto res = cipher::validate_assistant_request(req, local_caps);
        check(res.is_valid, "valid prompt accepted", "Expected success on valid prompt");
    }
}

// ---------------------------------------------------------------------------
// Test Invariant 2: Bounded Timeouts [1.0s, 300.0s]
// ---------------------------------------------------------------------------
void test_timeout_bounds() {
    cipher::AssistantCapabilities local_caps;
    local_caps.processing_route = cipher::ProcessingRoute::VerifiedLocal;

    // Below 1.0s
    {
        cipher::AssistantRequest req;
        req.prompt = "Valid prompt";
        req.timeout_seconds = 0.5;
        auto res = cipher::validate_assistant_request(req, local_caps);
        check(!res.is_valid, "timeout < 1.0s rejected", "Expected failure on sub-second timeout");
    }

    // Negative timeout
    {
        cipher::AssistantRequest req;
        req.prompt = "Valid prompt";
        req.timeout_seconds = -10.0;
        auto res = cipher::validate_assistant_request(req, local_caps);
        check(!res.is_valid, "negative timeout rejected", "Expected failure on negative timeout");
    }

    // Above 300.0s
    {
        cipher::AssistantRequest req;
        req.prompt = "Valid prompt";
        req.timeout_seconds = 300.5;
        auto res = cipher::validate_assistant_request(req, local_caps);
        check(!res.is_valid, "timeout > 300s rejected", "Expected failure on timeout > 300s");
    }

    // Boundaries: 1.0s and 300.0s
    {
        cipher::AssistantRequest req1;
        req1.prompt = "Valid prompt";
        req1.timeout_seconds = 1.0;
        auto res1 = cipher::validate_assistant_request(req1, local_caps);
        check(res1.is_valid, "boundary timeout 1.0s accepted", "Expected valid for 1.0s");

        cipher::AssistantRequest req2;
        req2.prompt = "Valid prompt";
        req2.timeout_seconds = 300.0;
        auto res2 = cipher::validate_assistant_request(req2, local_caps);
        check(res2.is_valid, "boundary timeout 300.0s accepted", "Expected valid for 300.0s");
    }
}

// ---------------------------------------------------------------------------
// Test Invariant 3: Tool Execution Lockdown (Fails Closed)
// ---------------------------------------------------------------------------
void test_tool_request_lockdown() {
    cipher::AssistantCapabilities local_caps;
    local_caps.processing_route = cipher::ProcessingRoute::VerifiedLocal;

    cipher::AssistantRequest req;
    req.prompt = "List directory files";
    req.request_tools = true;

    auto res = cipher::validate_assistant_request(req, local_caps);
    check(!res.is_valid, "tool execution request fails closed", "Expected failure when request_tools is true");
    check(res.error_message.find("prohibited") != std::string::npos,
          "error mentions tool prohibition", "Missing explanation of tool prohibition");
}

// ---------------------------------------------------------------------------
// Test Invariant 4: Local-Only Route Enforcement & Provenance Verification
// ---------------------------------------------------------------------------
void test_local_only_routing_invariants() {
    cipher::AssistantRequest req;
    req.prompt = "Explain quantum computing";
    req.local_only = true;

    // 1. VerifiedLocal route accepted
    {
        cipher::AssistantCapabilities caps;
        caps.processing_route = cipher::ProcessingRoute::VerifiedLocal;
        auto res = cipher::validate_assistant_request(req, caps);
        check(res.is_valid, "local_only accepted with VerifiedLocal route", "Expected success");
    }

    // 2. RemoteCloud route rejected under local_only
    {
        cipher::AssistantCapabilities caps;
        caps.processing_route = cipher::ProcessingRoute::RemoteCloud;
        auto res = cipher::validate_assistant_request(req, caps);
        check(!res.is_valid, "local_only rejects RemoteCloud route", "Expected rejection");
        check(res.error_message.find("Local-only policy violation") != std::string::npos,
              "error mentions local-only policy violation", "Missing expected error phrasing");
    }

    // 3. Unknown route rejected (model name / URL alone is not proof)
    {
        cipher::AssistantCapabilities caps;
        caps.processing_route = cipher::ProcessingRoute::Unknown;
        auto res = cipher::validate_assistant_request(req, caps);
        check(!res.is_valid, "local_only rejects Unknown route even with local model override", "Expected rejection");
    }

    // 4. Mixed route rejected
    {
        cipher::AssistantCapabilities caps;
        caps.processing_route = cipher::ProcessingRoute::Mixed;
        auto res = cipher::validate_assistant_request(req, caps);
        check(!res.is_valid, "local_only rejects Mixed route", "Expected rejection on mixed/hybrid route");
    }

    // 5. Remote route permitted when local_only is explicitly false
    {
        cipher::AssistantRequest cloud_req;
        cloud_req.prompt = "Public weather query";
        cloud_req.local_only = false;
        cipher::AssistantCapabilities caps;
        caps.processing_route = cipher::ProcessingRoute::RemoteCloud;
        auto res = cipher::validate_assistant_request(cloud_req, caps);
        check(res.is_valid, "RemoteCloud route permitted when local_only is explicitly false", "Expected pass");
    }
}

// ---------------------------------------------------------------------------
// Test AssistantManager Central Dispatcher & Gating
// ---------------------------------------------------------------------------
void test_assistant_manager_dispatch() {
    // 1. Null backend handling
    {
        cipher::AssistantRequest req;
        req.prompt = "Hello";
        auto resp = cipher::AssistantManager::submit(nullptr, req);
        check(resp.status == cipher::AssistantStatus::Unavailable,
              "null backend returns Unavailable", "Expected Unavailable status");
        check(resp.error.has_value(), "null backend returns descriptive error", "Expected error message");
    }

    // 2. Unavailable/offline backend handling
    {
        MockUnavailableBackend offline_backend;
        cipher::AssistantRequest req;
        req.prompt = "Hello";
        auto resp = cipher::AssistantManager::submit(&offline_backend, req);
        check(resp.status == cipher::AssistantStatus::Unavailable,
              "offline backend returns Unavailable without throwing", "Expected Unavailable status");
    }

    // 3. Policy violation interception before backend invocation
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
// Test Enum to String Conversions
// ---------------------------------------------------------------------------
void test_enum_to_string() {
    check(cipher::to_string(cipher::AssistantStatus::Ok) == "ok", "status ok string", "mismatch");
    check(cipher::to_string(cipher::AssistantStatus::PolicyViolation) == "policy_violation", "status policy violation string", "mismatch");
    check(cipher::to_string(cipher::ProcessingRoute::VerifiedLocal) == "verified_local", "route verified_local string", "mismatch");
    check(cipher::to_string(cipher::ProcessingRoute::RemoteCloud) == "remote_cloud", "route remote_cloud string", "mismatch");
}

} // anonymous namespace

int main() {
    std::cout << "Running CIPHER Assistant Backend Subsystem Test Suite...\n\n";

    test_prompt_validation();
    test_timeout_bounds();
    test_tool_request_lockdown();
    test_local_only_routing_invariants();
    test_assistant_manager_dispatch();
    test_enum_to_string();

    std::cout << "\n-------------------------------------------------------------\n";
    std::cout << "Tests passed: " << g_passes << '\n';
    std::cout << "Tests failed: " << g_failures << '\n';
    std::cout << "-------------------------------------------------------------\n";

    return (g_failures == 0) ? 0 : 1;
}
