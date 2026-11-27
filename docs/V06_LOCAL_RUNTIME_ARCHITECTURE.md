# v0.6 Local Inference Runtime: Architecture Proposal

Status: APPROVED by owner (design gate closed; approved with the recommended answers to section 7 items 1-4, items 5-6 remain open and do not block phases 1-2). Implementation proceeds in the phases in section 4. No implementation exists in this document's PR.
Scope: the smallest change that lets CIPHER answer a prompt with a model running **inside the CIPHER process on the owner's machine**, with no network path and no hosted dependency.

Baseline: `main` at `304a458` (v0.1 to v0.5 merged, 10/10 CTest). Nothing in this document has been prototyped or build-verified; library facts marked *(verify)* must be checked against llama.cpp primary docs before phase 1.

## 1. Current baseline

| Concern | Existing code | Notes |
|---|---|---|
| Backend interface | `IAssistantBackend` in `include/cipher/assistant_backend.hpp`: `backend_id()`, `is_available()`, `capabilities()`, `execute_request()` | Reused unchanged. |
| Request / response | `AssistantRequest { prompt, session_id, timeout_seconds, local_only, request_tools, model_override, budget }`, `AssistantResponse { status, text, model, route, duration_ms, error }` | Reused unchanged. |
| Local-only proof | `validate_assistant_request` invariant 4: `local_only` requires `capabilities().processing_route == VerifiedLocal` | An in-process backend can honestly declare `VerifiedLocal`. HTTP bridges cannot. |
| Dispatch | `AssistantManager::submit`: single backend, no fallback, `request_tools=false` enforced | Unchanged. Tools stay out of the model loop in v0.6. |
| Constraint | C++ app has no networking (PROJECT_STATE section 8) | v0.6 must not add any. |

## 2. Design questions

### 2.1 How does CIPHER run a model?

| Option | Verdict | Reason |
|---|---|---|
| A. Link `llama.cpp` in-process (C API) | **Recommended** | Matches DECISIONS #3 (own the runtime). No sockets, no child process, no Issue #6 exposure. `VerifiedLocal` is true by construction. |
| B. Spawn `llama-server`, call over loopback HTTP | Rejected | Needs an HTTP client (new networking in the C++ app) and a child process (Process side effect, Issue #6 process-tree kill unresolved). `VerifiedLocal` would rest on a URL check, which invariant 4 already says is not proof. |
| C. Spawn `llama-cli` per request | Rejected | Process side effect, per-request model load, parsing human-oriented output. |
| D. Write own inference engine | Rejected | Violates "no speculative architecture"; far beyond owner-maintainable. |

### 2.2 Build integration (the dependency decision)

- New CMake option `CIPHER_WITH_LLAMA`, **default OFF**. Default build, default CI and `scripts/verify.ps1` stay dependency-free and unchanged.
- When ON, `llama.cpp` is pulled at a **pinned commit** via `FetchContent` (or a vendored submodule; owner decision 10.2). CPU-only first; no CUDA/Metal/Vulkan in v0.6.
- The adapter is one new translation unit, `src/llama_backend.cpp`, compiled only when the option is ON. Nothing else includes llama headers.
- *(verify)* current llama.cpp C API names and license (MIT expected) before pinning; record the pinned hash and license in PROJECT_STATE.

### 2.3 Adapter shape

`LlamaBackend : IAssistantBackend`
- `backend_id()` returns `"llama.cpp"`; `capabilities()` returns `supports_text_conversation=true`, `supports_sessions=false`, `supports_streaming=false`, `processing_route=VerifiedLocal`.
- `is_available()` is true only if the model file loaded successfully. Load failure is `Unavailable`, never a crash.
- `execute_request()`: tokenize, decode, sample greedy by default, stop at `budget` output limit or end-of-sequence. Returns `AssistantResponse` with `route=VerifiedLocal`, `model` = model file stem.
- **Timeout:** an in-process call cannot be killed (same reasoning as v0.5 2.1). The adapter checks a deadline **between token decodes** and returns `AssistantStatus::Timeout` with the partial text discarded. Granularity is one token; documented limit.
- One request at a time (mutex). No concurrency in v0.6.
- Context is rebuilt per request from `prompt`; no session state (`supports_sessions=false`). History and memory injection are a later milestone.

### 2.4 Model files and config

- `config` gains `runtime.model_path` and `runtime.max_tokens`. No default model; **CIPHER ships no weights**.
- Path is validated at startup: must exist, be a regular file, have extension `.gguf` *(verify format)*, and be under the CIPHER data directory or an owner-set allowlisted root (reuse the v0.5 canonical-path-under-root check from `filesystem_tools`).
- Absent or invalid path: backend reports `Unavailable`; the existing no-model path keeps working. This preserves the v0.4 "no model attached" behavior as a tested invariant.
- Model downloading is **out of scope** (would need networking). The owner places the file manually; documented in DEVELOPMENT.md.

### 2.5 Security boundary

- No network code added. A CI/grep guard (phase 3) fails the build if `llama_backend.cpp` references socket or HTTP headers.
- Prompt is bounded by existing `ContextBudget.max_prompt_bytes`; output bounded by `runtime.max_tokens`. Both enforced before decode.
- Model files are untrusted input to a native parser (GGUF). Mitigation: allowlisted root, pinned llama.cpp commit, and a documented note that loading a hostile model file is a memory-safety risk accepted by the owner (section 9).
- Tools remain unreachable from the model: `request_tools=false` still enforced. The model's text output is never parsed as a command in v0.6.
- Logging: log model path stem and token counts only, never prompt text (consistent with existing logger redaction rules).

### 2.6 Testing without a real model

- Real-model tests are not CI-viable (weights are large, nondeterministic). Strategy:
  1. A `FakeLlamaApi` seam: `LlamaBackend` talks to a tiny internal interface (`load`, `generate(prompt, max_tokens, deadline)`), with the real llama.cpp implementation and a deterministic fake. Unit tests cover availability, missing file, timeout, token cap, `VerifiedLocal` route, validation path. This seam is the only abstraction added and exists for testing, not generality.
  2. Optional `CIPHER_TEST_MODEL` env var: if set to a small `.gguf`, a smoke test runs a real generation. Skipped (reported as skipped, not passed) when unset.
- Existing 10 suites must still pass with the option OFF and ON.

## 3. Files and ADRs

New: `src/llama_backend.cpp`, `include/cipher/llama_backend.hpp`, `tests/llama_backend_tests.cpp`, CMake option block. Changed: `config` (two keys), `main.cpp` (construct backend when ON and path set), docs. ADR: "In-process llama.cpp behind a default-OFF build option" added to DECISIONS.md at approval time.

## 4. Phases (each its own PR, small and verifiable)

1. **Seam + fake**: `LlamaBackend` against `FakeLlamaApi`, config keys, tests. No llama.cpp dependency yet. CI unchanged.
2. **Real adapter**: pinned llama.cpp, `CIPHER_WITH_LLAMA=ON` build path, load/generate, optional smoke test. Windows verified locally; Linux CI job added only if owner approves cost (10.3).
3. **REPL wiring + guard**: `ask` path selects the backend when available; no-network grep guard; DEVELOPMENT.md model setup steps.

## 5. External research: adopt / adapt / reject

| Item | Decision |
|---|---|
| llama.cpp C API | Adopt (option A). *(verify)* API and license. |
| llama-server loopback | Reject (2.1 B). |
| Ollama | Reject for v0.6: separate daemon, HTTP, `Unknown` route. Existing bridges cover it if the owner wants it. |
| Streaming tokens | Defer; `supports_streaming=false`. |
| GPU backends | Defer; CPU only keeps the build portable and small. |

## 6. Risks

| Risk | Mitigation |
|---|---|
| Hostile or corrupt `.gguf` crashes the process | Allowlisted root, pinned llama.cpp, owner acceptance (10.4). |
| llama.cpp build breaks Windows/MSVC or inflates CI time | Default-OFF option; phase 2 verified locally before any CI change. |
| Timeout granularity is one token; a slow first-token (prompt eval) can overshoot | Documented; cap `max_prompt_bytes`; revisit with subprocess isolation later. |
| Maintenance burden of a fast-moving upstream | Pinned commit, upgrade is an explicit PR. |
| Owner cannot maintain native inference code | Adapter is one file; engine itself stays upstream. |

## 7. Owner decisions required

1. **Approve option A** (in-process llama.cpp, default-OFF build option)?
2. **Dependency form:** FetchContent at pinned commit (recommended, no repo bloat) or git submodule?
3. **CI:** local-only verification for phase 2 (recommended), or add a CI job with the real library?
4. **Accept** the hostile-model-file risk with the allowlisted-root mitigation?
5. **Model choice** for the optional smoke test (owner supplies a small `.gguf`; none is bundled).
6. **License:** still unchosen; must be decided before any release that bundles or links llama.cpp (MIT-compatible expected, *(verify)*).
