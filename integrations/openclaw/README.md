# CIPHER — Operation OpenClaw Bridge (Optional Assistant Integration POC)

## 1. Overview & Objectives

This document establishes the architecture decision, interface design, test verification, and operational guidelines for **Operation OpenClaw Bridge**—an isolated proof of concept (POC) investigating whether CIPHER can use an existing open-source assistant ([OpenClaw](https://github.com/openclaw/openclaw)) as an optional capability provider while strictly retaining its independent C++17 core, persistent memory, and provider-independent architecture.

> **Founding Principle:** Build it so that no single company, model, or generation of technology can determine CIPHER's future. The core must not require a hosted AI API to start or perform its basic duties. Provider integrations, if added, must be optional adapters with clearly defined interfaces.

---

## 2. Architecture Decision: Route A (CLI) vs Route B (Gateway WebSocket)

### Context
OpenClaw exposes two primary integration surfaces for external applications:
* **Route A (CLI):** One-shot execution via `openclaw agent [options]`, capturing structured JSON outputs.
* **Route B (Gateway WebSocket RPC):** Long-lived WebSocket connection to the OpenClaw Gateway daemon on `ws://127.0.0.1:18789`, executing a two-frame handshake (`connect` -> `hello-ok`), dispatching the `agent` RPC method, and awaiting completion via `agent.wait` or event streaming.

### Decision
For the initial proof of concept, **Route A (CLI)** is selected as the primary integration mechanism, while **Route B (Gateway Protocol)** is fully documented and structured as the target for future streaming/interactive milestones.

### Rationale
1. **Decoupling & Non-Invasiveness:** Route A requires zero network daemon supervision, zero persistent connection state, and zero additional networking libraries inside CIPHER.
2. **Deterministic Process Boundaries:** Subprocess execution with explicit arguments (`shell=False`) allows absolute control over execution arguments, prevents command injection, and enables OS-level timeout enforcement.
3. **No Credential Exposure:** Route A utilizes the user's existing local OpenClaw profile without requiring CIPHER to pass or store Gateway bearer tokens across network sockets.
4. **Maintenance Simplicity:** The CLI contract (`openclaw agent --message <prompt> --model <model> --json`) is a high-level stable interface intended for scripts and external apps, insulating CIPHER from underlying wire-protocol changes in `packages/gateway-protocol/src/schema.ts`.

---

## 3. Comprehensive Comparison: CLI vs Gateway WebSocket Protocol

| Dimension | Route A: One-Shot CLI (`openclaw agent`) | Route B: Gateway WebSocket Protocol (`ws://127.0.0.1:18789`) |
| :--- | :--- | :--- |
| **Complexity** | **Very Low:** Standard subprocess invocation, synchronous stdout/stderr capture, JSON response parsing. | **High:** WebSocket framing, handshake sequence (`connect` -> `hello-ok`), JSON-RPC framing, `agent` -> `runId` followed by `agent.wait`. |
| **Compatibility** | **High:** Supported across all official OpenClaw distributions. High-level CLI flags remain backwards-compatible. | **Medium:** Bound to specific wire-protocol versions (`packages/gateway-protocol`) and TypeBox schema changes; requires tracking protocol releases. |
| **Session Persistence** | **Supported:** Explicitly supported via `--session-id <id>` or `--session-key <key>` flags matching Gateway sessions. | **Native:** Managed through `sessions.*` RPC family with full session catalog browsing, subscriptions, and recovery. |
| **Cancellation** | **Immediate:** Standard process termination (SIGINT / `taskkill` / process kill) immediately terminates the child process. | **Cooperative:** Handled via RPC (`agent.cancel` or `stopReason: superseded` upon subsequent turn). |
| **Timeout Behavior** | **Double-Guarded:** CLI flag `--timeout <seconds>` combined with caller-enforced `subprocess.TimeoutExpired`. Cannot hang. | **Polling/Heartbeat:** `agent.wait` returns `status: "pending"` or `"timeout"` when wait expires, requiring client event loop management. |
| **Authentication** | **Local Profile:** Inherits local user environment and configuration without exposing raw tokens in CIPHER. | **Explicit Handshake:** Requires passing bearer token or device pairing signature during initial WebSocket `connect` frame. |
| **Streaming / Latency** | **Turn-Based:** Buffers complete turn output before returning. Not suited for real-time token-by-token streaming. | **Real-Time:** Supports native event subscriptions (`stream: "assistant"`, `stream: "lifecycle"`) for immediate token streaming. |
| **Maintenance Cost** | **Minimal:** Isolated bridge file (~150 LOC) with zero external C++ or Python dependencies. | **Substantial:** Requires dedicated WebSocket client library, heartbeat monitors, reconnection logic, and schema synchronizers. |

---

## 4. Replaceable Integration Contract

CIPHER defines a generic, provider-agnostic contract for optional assistant capability providers. This boundary ensures CIPHER never becomes locked into OpenClaw, Ollama, or any specific provider.

### Python Abstract Interface (`integrations/openclaw/assistant_contract.py`)
```python
class BaseAssistantBackend(ABC):
    @abstractmethod
    def submit_prompt(self, request: AssistantRequest) -> AssistantResponse:
        """Submit a single text prompt and receive the resulting text response."""
        pass

    @abstractmethod
    def is_available(self) -> bool:
        """Check whether the assistant backend runtime is reachable and ready."""
        pass

    @abstractmethod
    def get_status(self) -> Dict[str, Any]:
        """Return diagnostic health and configuration state (no secrets)."""
        pass
```

### Future C++ Interface (`integrations/openclaw/assistant_contract.hpp`)
```cpp
namespace cipher {
class IAssistantBackend {
public:
    virtual ~IAssistantBackend() = default;
    virtual AssistantResponse submit_prompt(const AssistantRequest& request) = 0;
    virtual bool is_available() = 0;
    virtual std::string backend_name() const = 0;
};
}
```

---

## 5. Local-First Enforcement & Cloud Rejection Policy

To uphold CIPHER's Core Directives, the OpenClaw bridge implements strict local-first gating **before** executing any external command:

1. **Explicit Enablement Gate:** Disabled by default (`CIPHER_OPENCLAW_ENABLED=0`). If not enabled, the bridge immediately returns `status="disabled"` without invoking any process.
2. **Loopback Network Binding:** Gateway URLs are restricted to loopback (`127.0.0.1`, `localhost`, `[::1]`). Any remote IP or host is rejected as a security violation.
3. **Model Allowlist & Cloud Pattern Denylist:**
   * **Approved Local Prefixes:** `ollama/`, `local/`, `lmstudio/`, `llamacpp/`, `vllm/`.
   * **Forbidden Cloud Patterns:** `openai`, `anthropic`, `claude`, `gpt-`, `gpt_`, `ollama-cloud`, `gemini`, `mistral-ai`, `bedrock`, `azure`, `groq`, `together`, `fireworks`, `deepseek-cloud`.
   * If a model matches any cloud pattern or lacks an approved local prefix under `strict_local=True`, execution is rejected with `status="rejected_cloud_model"` and no network or subprocess activity occurs.
4. **Tool Execution Lockdown:** Subprocess arguments strictly omit `--deliver`, `--tool`, or permission bypass flags. The bridge acts exclusively as a text-in, text-out assistant.
5. **Credential Masking:** Error reporting sanitizes authorization headers, tokens, and keys (`[REDACTED]`).

---

## 6. Documented Setup Procedure for Safe Live Testing

When the project owner chooses to conduct a safe live test with a local model, follow these steps:

### Step 1: Install Ollama (Local Model Runner)
Download and install Ollama from [https://ollama.com](https://ollama.com).
Verify local operation:
```bash
ollama --version
ollama run llama3.2 "Say hello from local Ollama"
```

### Step 2: Install OpenClaw
Install OpenClaw globally or in a local Node environment (Node.js 20+ required):
```bash
npm install -g openclaw
openclaw --version
```

### Step 3: Configure OpenClaw with Local Ollama Provider
Configure OpenClaw to connect to Ollama without cloud fallbacks:
```bash
openclaw config set models.default "ollama/llama3.2"
openclaw config set providers.ollama.baseUrl "http://127.0.0.1:11434"
```
Verify the security baseline:
```bash
openclaw security audit
```
Ensure all external cloud providers (OpenAI, Anthropic) are disabled or unset.

### Step 4: Run Safe Text-Only Test
Enable the CIPHER integration via environment variable and submit a test prompt:
```bash
# Windows PowerShell
$env:CIPHER_OPENCLAW_ENABLED = "1"
python -c "
from integrations.openclaw.bridge import OpenClawBridge
from integrations.openclaw.assistant_contract import AssistantRequest

bridge = OpenClawBridge()
print('Status:', bridge.get_status())
resp = bridge.submit_prompt(AssistantRequest(prompt='Hello CIPHER!'))
print('Response status:', resp.status)
print('Reply:', resp.text)
"
```

---

## 7. Limitations, Costs, and Security Risks

### Limitations
1. **No Token Streaming:** Route A buffers the complete model response before returning. Interactive streaming requires Route B (WebSocket).
2. **Cold Model Latency:** Local models run on CPU or local GPU; initial prompt processing may take several seconds depending on model size and hardware.
3. **Single Turn Execution:** The current POC submits one-shot prompts. Multi-turn dialogue history must be passed via explicit session identifiers or prompt context assembly.

### Costs
* **Monetary Cost:** **$0.00.** Zero cloud API fees, zero token billing, and zero hosted subscription requirements.
* **Hardware Cost:** Requires local RAM/VRAM depending on model (e.g. 4GB for `llama3.2:1b` or `llama3.2:3b`; 8GB+ for 7B/8B models).

### Security Risks & Mitigations
* **Tool Escalation Risk:** OpenClaw includes powerful tool-execution capabilities (`system.run`, browser control).
  * *Mitigation:* The CIPHER bridge strictly forbids passing tool flags, bypass flags, or channel delivery flags. Subprocess is executed with `shell=False` and a hardcoded literal argument list.
* **Prompt Injection:** Adversarial prompt text could attempt to trigger unintended agent actions.
  * *Mitigation:* Because tool execution is not enabled and input is passed as a literal parameter value, prompt injection is confined to the generated text response and cannot execute commands on the host OS.
* **Accidental Cloud Egress:** Misconfiguration could route queries to cloud providers.
  * *Mitigation:* The bridge enforces strict model prefix matching and explicitly denies cloud patterns before process launch.
