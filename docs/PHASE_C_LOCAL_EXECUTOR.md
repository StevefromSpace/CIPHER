# Phase C: Local-Model Implementation Executor (design)

Status: **proposal, no code**. Nothing here is implemented or verified against a live local model.
Governance stays in [AGENT_ROLE_CONTRACT.md](AGENT_ROLE_CONTRACT.md); current orchestration facts are in
[ORCHESTRATION_STATUS.md](ORCHESTRATION_STATUS.md). This document changes neither.

## 1. Goal and non-goals

Goal: let `job step` run the Implementation role with a **local** model (Ollama or in-process llama.cpp), so a job
can complete without any paid API (local-first, AI tools are assistants only: AGENTS.md core directives).

Non-goals: new roles, merge/tag/LICENSE automation, GPU backends, streaming, native tool calling, changes to the
state machine, the 2-execution budget or the contract.

## 2. Which interface to implement

The orchestration loop already has the seam. Do not add a new executor.

| Seam | Where | Use |
|---|---|---|
| `ToolAgent::next(task, transcript) -> AgentStep` | `include/cipher/orchestration/implementation_executor.hpp` | **Implement this.** Its comment: implementations "must not execute anything; they only propose". |
| `ImplementationExecutor` | `src/orchestration/implementation_executor.cpp` | Unchanged. Allocates the worktree, drives `WorktreeTools`, verifies, commits, returns `ImplementationComplete` + head SHA or `ImplementationFailed`. |
| `ImplementationRig` | `include/cipher/orchestration/implementation_rig.hpp` | Production wiring; today hard-codes `ClaudeToolAgent` and `model_family = "claude"` (`src/orchestration/implementation_rig.cpp`). |
| `IAssistantBackend` | `include/cipher/assistant_backend.hpp` | Model transport. `OllamaBackend` and `LlamaCppBackend` (`ollama_backend.hpp`, `llama_backend.hpp`) already implement it. |

New class: `LocalToolAgent final : public ToolAgent`, in `include/cipher/orchestration/local_tool_agent.hpp` and
`src/orchestration/local_tool_agent.cpp`, added to `cipher_orchestration` in `CMakeLists.txt` next to
`implementation_executor.cpp`.

- Takes `IAssistantBackend&` (not a concrete backend), a timeout and a prompt budget.
- `next()` builds one prompt, calls `AssistantManager::submit` (so `validate_assistant_request` runs), and feeds
  `response.text` to the **existing** `parse_agent_reply`.
- Throws on a non-Ok status or malformed reply. The executor already maps any exception to `ImplementationFailed`
  with the audit trail (`failed(...)`, "agent error: ..." in `implementation_executor.cpp`).

Rig change (slice C4): `RigConfig` gains an agent choice; `ImplementationRig` builds `LocalToolAgent` instead of
`ClaudeToolAgent` and sets `settings.model_family` to `"ollama"` or `"llama"`. Research/Architecture roles share the
same `ToolAgent&` (`ReadOnlyExecutor`), but using the local model there is out of scope.

## 3. How the model proposes edits (text protocol)

Reuse, do not invent. Two existing patterns:

1. `ClaudeToolAgent` protocol (`kProtocol` in `implementation_executor.cpp`): one JSON object per reply, either a
   `ToolCall` (`tool`, `path`, `content`, `old_text`, `new_text`, `pattern`) or `{"done":true,"summary":"..."}`.
   `parse_agent_reply` takes the outermost `{...}`, rejects non-string fields and caps `summary` at 500 chars.
2. `ModelToolLoop` (`include/cipher/model_tool_loop.hpp`): text-only protocol (`TOOL_CALL: <id> <arg>`), no native
   tool calling, one tool per round, `kMaxModelRounds`, `kMaxToolFailures`, repeat guard, `kMaxToolOutputBytes`
   (DECISIONS section 21: the llama.cpp path is text only).

Decision: keep the **JSON-object** reply (pattern 1) because it carries multi-line `content` and `old_text` safely
and the parser and its tests exist. Adopt pattern 2's **safety shape**: one call per round, a repeat guard (same
tool and args twice in a row counts as a failure), a bounded transcript, and the hard step cap
(`ImplementationSettings::max_steps`, default 20).

Small-model adjustments, all inside `LocalToolAgent`:
- Shorter protocol text plus one worked example; local models drift more than Claude. `kMaxPromptChars` is 3900 today;
  the llama.cpp context is `llama_context_tokens` (default 4096) and over-long prompts are refused
  (DECISIONS section 21), so the budget must be derived from the backend, not copied.
- Prefer `fs.edit` (`old_text` must occur exactly once) over `fs.write` of whole files; the protocol text says so.
- Tolerate fenced blocks and prose around the JSON (the parser already skips to the first `{` and last `}`).
- No retry inside `next()`. A malformed reply is one failed execution (section 7).

## 4. Worktree isolation and write-path policy

Isolation is enforced below the agent; the local agent inherits it and cannot bypass it.

- Worktree: `WorktreeAllocator::allocate(issue)` + `GitWorktreeBackend` (job-id validation, root containment, owner
  records; `src/orchestration/worktree_allocator.cpp`, `git_worktree_backend.cpp`). The source repository is never
  edited directly (`RigConfig::repository` comment).
- Tools: `WorktreeTools` (`worktree_tools.hpp`) validates paths, asks `PermissionPolicy`, bounds output, writes one
  audit line per call and holds no shell. `process.run` launches only argv registered by id.
- Write policy: `default_worktree_policy()` has default tier `Ask` (denied, no owner), whitelists the read, list,
  search, edit and run tools, and **blacklists `fs.write`/`fs.edit` on** `.git`, `.github`, `tools/`, `tests/`,
  `cmakelists.txt`, `docs/agent_role_contract.md` (`src/orchestration/worktree_tools.cpp:68-77`; QA blacklist from PR #64).
- Phase C adds **no tool and no broader policy**. For the first scope it adds a *narrower* one (section 5).
- The model gets no credentials and no push: the executor only commits (`--no-verify`, no signing).
- Route: requests set `local_only = true`, which `validate_assistant_request` accepts only for
  `ProcessingRoute::VerifiedLocal` (`assistant_backend.hpp`). A backend that cannot prove local is rejected, never used.

## 5. First scope: docs/tests-only issues

`default_worktree_policy()` blacklists `tests/`, so no agent can write tests today. Phase C starts narrower and widens
only by explicit owner decision:

- **Scope 1 (first): docs only.** Add `default_docs_only_policy()` beside `default_worktree_policy()`: same base, but
  `fs.write`/`fs.edit` allowed only for `docs/*` and `*.md`, everything else blocked. Still blocks
  `docs/agent_role_contract.md`, owner-controlled release documentation (contract section 6; `docs/RELEASE.md`) and `LICENSE*`.
- **Scope 2 (later, owner-approved): tests.** Needs a deliberate relaxation of the `tests/` blacklist (it exists so an
  agent cannot weaken the checks that judge it); `CMakeLists.txt` stays blocked. Not in slices C1-C4.
- `src/`, `include/`, security-sensitive paths (contract section 7) and workflows stay out. A diff touching them is a
  failure, not a warning.
- Defense in depth: the executor already requires `git diff --check` (`verify_command_id = "check"`,
  `implementation_rig.cpp`). Slice C3 adds a post-check that `git diff --name-only <base>` lists only allowed paths
  (contract 4.4 completion rule), independent of the policy.

## 6. Owner gates (unchanged)

Phase C must not weaken contract section 6 or the section 8 gates.

- **No merge.** The executor only commits in the worktree. Merge stays `governed_merge.cpp` plus `merge.lock`; the
  local agent has no merge, push, tag or GitHub-write tool.
- **No tags, releases, LICENSE, release docs, governance or credentials.** No tool exists for them, the paths are
  blocked, and task text cannot add commands (QA and verify commands come only from owner config,
  `implementation_rig.cpp`).
- **Write confirmation.** Two compatible modes, decided once (open question 2):
  - *Policy mode (preferred for jobs):* the owner's explicit `job task` / `job step` plus the docs-only write
    whitelist is the authorization. This is how the Claude path behaves today (no per-write prompt in `WorktreeTools`).
  - *Per-write mode:* reuse the `ModelToolConfirmation` shape (`model_tool_loop.hpp`, exact `yes`): each `fs.write`/
    `fs.edit` shows the resolved path first. Needs a confirmation callback on `WorktreeTools` and blocks unattended runs.
- **Independence (contract 1.5).** `model_family` is reported through `Orchestrator::apply(event, model_family)`.
  Local runs report `"ollama"`/`"llama"`, different from a `"claude"` reviewer, which satisfies "different agent
  instances". It does **not** satisfy the Gemini rule, so security-sensitive paths still escalate to `NEEDS_HUMAN`
  (contract 1.5.4); docs-only scope never touches them.

## 7. Two-execution budget and failure handling

The orchestrator owns the budget. The local agent adds no counter and no internal retry
(`implementation_executor.hpp`: "no internal retry; the orchestrator owns the 2-execution limit").

- 5.1.1-5.1.2: 2 executions per issue; the retry receives failure history. The rig (C4) appends the prior
  `evidence-<n>.log` summary to the task, capped to the prompt budget.
- 5.1.6: charge on dispatch, persisted with an in-flight marker before the executor runs. A local run costs no money
  but still consumes an execution; do not exempt it, or a flaky local model could loop. The budget bounds attempts,
  not dollars.
- 5.1.7-5.1.8: a crash mid-run leaves the in-flight marker; `recover` consumes the attempt and never re-runs the
  executor (`ORCHESTRATION_STATUS.md`). Side effects in the worktree may remain, same as the Claude path.
- 5.1.9: `job reconcile` for legacy records is unaffected.
- 5.1.4: a higher tier on attempt 2 is a model change within an execution. Phase C does not auto-switch models
  (open question 3).

Failures that become `ImplementationFailed` (existing paths, `implementation_executor.cpp`): non-Ok backend status
(timeout, unavailable, route not VerifiedLocal), malformed reply, step limit, failing verify command, no changes, git
failure; slice C3 adds an out-of-scope diff. Attempt 1 failure leads to the retry; attempt 2 failure leads to
`NEEDS_HUMAN` (contract 5.1.5, state machine). Repeated `BLOCKED` follows 5.5. "Ollama not running" is environmental
(contract section 2 rule 5) rather than a quality failure, but it is still charged if dispatched; the mapping is
open question 1.

## 8. Test plan

No live model in CI (same convention as `CIPHER_TEST_GGUF`, which prints `SKIP`: `tests/test_llama_backend.cpp:235`).

Deterministic, every CI build, new `tests/test_orchestration_local_agent.cpp` (registered like
`cipher_orchestration_implementation_tests`, `CMakeLists.txt:370-372`):
1. **Scripted fake `IAssistantBackend`** returning queued strings (as `tests/test_model_tool_loop.cpp`). Cases:
   read-edit-done producing a commit in a temp repo; fenced JSON; prose around JSON; malformed JSON; unknown tool;
   non-string field; backend `Timeout`/`Unavailable`; route not `VerifiedLocal`; prompt over budget; repeat guard;
   step limit.
2. **Policy tests** for `default_docs_only_policy()`: docs write allowed; `src/`, `tests/`, `.github/`,
   `docs/agent_role_contract.md`, `LICENSE`, `CMakeLists.txt`, traversal and case variants denied.
3. **Executor integration** with a real temp git repo (as `tests/test_orchestration_implementation.cpp`):
   `ImplementationFailed` per failure mode, `ImplementationComplete` with a full SHA, out-of-scope diff rejected.
4. **Budget**: through `Orchestrator` with a fake factory, two failing local executions end in `NEEDS_HUMAN` with no
   third dispatch (extends `tests/test_orchestration_flow.cpp` and `test_orchestration_recovery.cpp`).
5. No test shells out to `ollama` or loads a model.

Optional local real-model check (never in CI, skips without env): `tests/local_executor_real_validation.cpp`, modeled on
`tests/qa_real_validation.cpp` and `tests/gh_pr_live_validation.cpp`. Uses `CIPHER_TEST_GGUF` or an Ollama port, runs
one docs-only task in a throwaway repo, prints PASS/FAIL/SKIP plus the audit log. A pass is evidence, not a gate; a
small model may fail, and that failure is the expected `ImplementationFailed`.

## 9. Milestone slices (small PRs)

1. **C1: `LocalToolAgent` + tests.** Class and fake-backend tests (8.1). No wiring.
   Acceptance: builds on Ubuntu and Windows CI; all 8.1 cases pass; no behavior change; no new dependency
   (nlohmann/json is already vendored).
2. **C2: docs-only policy.** `default_docs_only_policy()` and 8.2 tests; `ImplementationSettings` gets a policy
   selector defaulting to today's policy. Acceptance: existing implementation tests unchanged and green; every denied
   path in 8.2 denied; docs writes allowed.
3. **C3: scope post-check.** `git diff --name-only <base>` allowlist in `ImplementationExecutor`, for every agent.
   Acceptance: out-of-scope diff gives `ImplementationFailed` with the paths in the evidence log; Claude-path tests
   still pass.
4. **C4: rig and config wiring.** Key such as `orchestration_agent = claude|local` in `include/cipher/config.hpp`
   and `src/main.cpp`; `ImplementationRig` builds `LocalToolAgent` over the configured Ollama or llama backend and sets
   `model_family`; retry-history prompt. Acceptance: default unchanged (`claude`); `local` with no backend configured
   fails closed at startup with a message; a scripted-backend docs-only `job step` completes; the 8.4 budget test passes.
5. **C5 (optional): real-model check and docs.** 8 optional check; update `ORCHESTRATION_STATUS.md`,
   `PROJECT_STATE.md`, `ROADMAP.md`; add a DECISIONS entry. Acceptance: a recorded local run or an honest "not
   verified"; docs claim only what ran.

C2 and C3 are independent of C1 and may land in any order; C4 needs C1-C3.

## 10. Open design questions

1. **Backend unavailable.** Is "Ollama not running" an environmental `BLOCKED` (no quality blame) or a failed
   execution? Either way it is charged if dispatched (5.1.6); which one may consume the single BLOCKED unit (5.5)?
2. **Write authorization.** Policy mode (job start is the authorization, docs-only whitelist) or per-write exact-`yes`?
   Owner decision before C4; it decides whether unattended runs are possible.
3. **Tier-up on attempt 2.** Is a larger local model for the retry (5.1.4) in scope, and how is it configured without
   a new selector? `llama.cpp` refuses `model_override` today (DECISIONS section 21).
4. **Prompt budget vs small context.** How is the transcript compressed for a 4096-token window while keeping an
   edit's `old_text` exact? Needs a measured answer from the optional real-model check.
5. **Tests scope.** When, if ever, may the `tests/` blacklist be relaxed for a local model, given it guards the checks
   that judge the agent's own work?

### Lead decisions (2026-11-28, under the owner's delegation)

1. **Backend unavailable:** environmental `BLOCKED` raised before dispatch, so no charge (5.1.6). It consumes the single BLOCKED unit (5.5); a second one escalates to `NEEDS_HUMAN`.
2. **Write authorization:** policy mode. Starting the job is the authorization, limited to the docs-only whitelist inside the isolated worktree. No merge, tag or LICENSE change is reachable. Per-write exact-`yes` stays for any path outside the whitelist.
3. **Tier-up:** out of scope for C1-C4. Attempt 2 reuses the same local model.
4. **Prompt budget:** to be measured in the optional real-model check; until then keep edits small (one file per round).
5. **Tests scope:** the `tests/` blacklist is kept. Relax only by a later owner-visible decision.
