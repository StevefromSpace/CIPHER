# v0.5 Permissioned Tools: Architecture Proposal

Status: APPROVED by owner (design gate closed). Implementation proceeds in the phases in section 8. No implementation exists in this document's PR.
Scope: the smallest tool architecture that lets CIPHER run owner-requested, permission-gated actions, without giving the assistant unrestricted control.

Baseline verified on `main` at `206b8b7` (PRs #1, #2, #14, #15, #16 merged). `main` CI run 40278596961: Windows, Ubuntu and Python all success.

## 1. Current baseline (what already exists)

| Concern | Existing code | Notes |
|---|---|---|
| Risk tiers | `RiskTier { Safe, Monitor, Ask, Block }` in `include/cipher/assistant_backend.hpp` | Reused unchanged. |
| Action descriptor | `ActionDescriptor { name, target, category, default_tier }` | `name` is the action verb, `target` is the object. |
| Policy | `PermissionPolicy`: blacklist, whitelist, default tier. Priority Blacklist > Whitelist (downgrades to Safe) > default. | Pattern matched against `"<name> <target>"`. Category is not part of the match string. Used by the memory gate on `remember`/`forget` (default `Monitor`). |
| Provider path | `IAssistantBackend`, `AssistantManager::submit` (single backend, no fallback) | Model reasoning only. No tool use. `request_tools = false` is enforced. |
| Memory gate | `MemoryCommands` checks `check_permission` before any mutation | The pattern to copy for tool gating. |

`PermissionPolicy` is documented as a classifier, not an execution engine. This proposal keeps that split: `PermissionPolicy` decides, and a new registry executes only after an allowed decision.

## 2. Answers to the design questions

### 2.1 Tool identity

A tool is identified by a **stable dotted ID** (`time.now`, `system.info`, later `filesystem.read`). The ID is the descriptor `name`. Each tool has:

- `id` (unique, lowercase, `[a-z0-9.]`), `description` (one line for `tool list`)
- `category` (`time`, `system`, `filesystem`, `memory`, ...) used for grouping only
- `default_tier` (feeds `ActionDescriptor`)
- `side_effects`: `None`, `ReadLocal`, `WriteLocal`, `Process`, `Network` (declared, enforced by registration rules, see 2.2)
- `handler`: `std::string(const std::string& arguments)`

Not in v0.5: JSON schemas, output schemas, plugin manifests. Arguments are a single UTF-8 string, validated by the handler. This matches the existing `echo`/`remember` command shape and avoids a schema dependency.

Timeouts are **not** part of the in-process interface. A `std::function` cannot be killed safely. Tools with a time bound must run as subprocesses, which is a later milestone (see 5).

### 2.2 Registry

- **v0.5: static, built-in only.** Built-ins are registered explicitly in one function (`register_builtin_tools`) called at startup. No dynamic registration.
- **Rejected for v0.5:** provider-supplied tools (a backend must not add tools to CIPHER), plugin loading, and any registration from configuration files.
- Registration rules (enforced in code, tested):
  - Duplicate or empty IDs are rejected.
  - A tool declaring `Process` or `Network` side effects is **refused at registration** in v0.5. Those classes are not implemented.
  - A tool with `WriteLocal` side effects must have default tier `Ask` or `Block`.

### 2.3 Permission integration

Permission is decided by the **existing** `PermissionPolicy` and `RiskTier`, with no second system:

1. Caller passes `ActionDescriptor{ name = tool id, target = resolved target, category, default_tier }`.
2. `PermissionPolicy::evaluate(descriptor)` returns the tier.
3. `Safe` and `Monitor` run. `Ask` runs only with owner confirmation (section 2.4). `Block` never runs.

Mapping of the requested capability classes onto the existing model:

| Capability | Action name | Target | Default tier (v0.5) |
|---|---|---|---|
| time read | `time.now` | none | Safe |
| system info read | `system.info` | none | Safe |
| memory inspect | `memory.list` | none | Monitor |
| filesystem read | `filesystem.read` | canonical path under allowlisted root | Monitor (deferred to phase 4) |
| filesystem write | `filesystem.write` | canonical path | Ask (not in v0.5) |
| process execute | `process.execute` | command name | Block (not in v0.5) |
| network request | `network.request` | host | Block (not in v0.5) |
| memory write | `memory.write` | key | Monitor for `remember`/`forget` (existing) |

Target matching requires evaluating `name + " " + target`. PR #12 evaluates `name` only (see section 9), which cannot express `filesystem.write C:\secret*`.

### 2.4 Execution lifecycle

Use a **single outcome enum**, not a stored state machine. States are recorded only as the final outcome plus the decision:

```
requested -> decision { Denied | Allowed(tier) } -> { Completed | Failed | DryRun | NotFound }
```

- `Denied` and `NotFound`: the handler was never called. Zero execution.
- `Completed`: handler returned normally.
- `Failed`: handler threw or reported an error. The tool *did* run.
- `DryRun`: decision was Allowed, handler skipped (owner preview).
- `TimedOut`: reserved for subprocess tools (later milestone). Not produced in v0.5.

`Ask` confirmation (phase 5, optional for v0.5): prompt the owner on the REPL. EOF or any answer other than an explicit `yes` is a denial. No timeout-based approval.

### 2.5 Results

```
struct ToolResult {
    ToolOutcome outcome;   // Completed | Failed | Denied | NotFound | DryRun
    RiskTier tier;         // decision tier, always set
    bool executed;         // true only when the handler was called
    std::string output;    // handler output, or diagnostic text
};
```

`executed` makes "ran and failed" (`executed = true, outcome = Failed`) distinguishable from "never allowed" (`executed = false, outcome = Denied`). The assistant layer must show `executed` to the owner.

### 2.6 Audit model

One append-only record per attempt, including denials:

| Field | Stored | Reason |
|---|---|---|
| sequence number | yes | ordering within a session |
| correlation ID | yes | ties one owner command to its record |
| tool ID | yes | |
| decision tier | yes | |
| outcome, executed flag | yes | |
| argument length (bytes) | yes | size only |
| arguments themselves | **no** | may contain secrets or personal data |
| output | **no** | same reason; shown to owner, not stored |
| wall-clock timestamp | yes, local time, to seconds | |

v0.5 keeps the log in memory for the session. Persisting it to disk is deferred, because it needs its own retention and redaction rules. The existing `Monitor` tier means every `Monitor` action appears in this log.

## 3. Security boundary (non-negotiable)

- **Owner-typed calls and opt-in model-proposed calls.** An owner-typed REPL command (`tool <id> [args]`) can invoke any registered tool. A model may propose a call only when `model_tool_calls = true` and `ollama_port` is set (owner-directed amendment to the original "no model-initiated calls" rule). Each proposal needs the owner's exact `yes`, even for Safe tools, and must be on a fixed offered list (`time.now`, `system.info`). `memory.list` and `filesystem.read` are never offered to the model. `request_tools = false` stays in `AssistantRequest`.
- Providers receive tool ids and descriptions only as prompt text built by the loop. They never receive a registry, never register tools, and never execute them. The CIPHER-side loop parses a strict `TOOL_CALL: <id>` reply and runs the call itself.
- Model-proposed calls use a separate registry, so `tool log` shows owner calls only. The audit record has no origin field yet; that is a known gap.
- Every invocation passes through the policy **before** the handler is called.
- Denied calls: zero handler calls, no partial mutation, a clear diagnostic. Tested by byte-identical file checks, as with the memory gate.
- No shell, no arbitrary process execution, no network, no filesystem writes in v0.5. These are refused at registration, not just by policy.
- Blacklist defaults shipped in code (not config): `process.*`, `network.*`, `filesystem.write*`.
- Failure does not fall back to another tool or provider.

## 4. Initial safe-tool list (v0.5 phases 2–3)

1. `time.now`: local date and time. `Safe`. No input.
2. `system.info`: CIPHER version, build type, platform name. `Safe`. Deliberately excludes hostname, user name, environment variables and paths.
3. `memory.list`: lists memory entry IDs only, not contents. `Monitor`.

Deferred to phase 4, with stronger controls:
- `filesystem.read` under one allowlisted root (default: none configured, so the tool is unavailable until the owner sets it), canonicalized path, symlink and junction rejection, size bound.

Not included: any write, process, network or model tool.

## 5. PR #12 recommendation

PR #12 (`feat/readonly-tool-registry`, head `ee498ea`, base PR #3, Draft) implemented a read-only `ToolRegistry`. Its CI is green for `ee498ea`.

What is useful and should be kept (in a fresh branch on `main`):
- `ToolOutcome` and `ToolResult` shapes, the fail-closed denial before the handler, dry-run, the handler-exception-to-`Failed` mapping, and the 18 tests.

What conflicts with the design and must change:
- Policy evaluates `descriptor.name` only. It needs `name + " " + target`.
- `ToolResult` lacks `executed`. Without it, "ran and failed" and "never allowed" look the same to the caller.
- The log has no sequence/correlation ID and no timestamp.
- Its base is PR #3, which is still an unmerged Draft and carries the provider process runner. A registry needs none of that, so it should rebase onto `main`.

**Recommendation:** do not merge PR #12 and do not resurrect its branch as-is. Rewrite the registry on a new branch from `main`, reusing its header and tests where they match, and keep PR #12 parked until the owner approves this design. Then close PR #12 as superseded, or keep it as history, at the owner's choice.

## 6. External research (adopt / adapt / reject)

Source: reports in `C:/Users/Johan Joseph/Documents/CIPHER-upstream-research/reports` (`openclaw.md`, `leon.md`, `openvoiceos.md`, `dodaios.md`).

| Idea | Source | Decision | Reason |
|---|---|---|---|
| Blacklist > whitelist > tool default > policy default, fnmatch on `"<tool> <args>"` | Dodaios | **Adopt** | Already in `PermissionPolicy`. |
| `monitor` tier logged to an audit stream | Dodaios | **Adopt** | The audit model in 2.6. |
| Explicit deny-all for a tool-free run; never rely on a profile name | OpenClaw | **Adopt** | `request_tools = false` plus an explicit empty allowlist. The `minimal` profile still exposes gateway tools. |
| Multi-layer policy intersection (sandbox, caller, provider, profile) | OpenClaw | **Adapt** | Collapse to one `PermissionPolicy` evaluation. Three layers would be speculative for one owner. |
| Tokens and secrets never written to tool-call logs; isolation per profile | Leon | **Adopt** | Arguments are not logged (2.6). |
| Progressive tool-schema loading and dynamic tool-manager | Leon 2.0 | **Reject** | Leon 2.0 is a preview and warns of breaking changes. CIPHER has three tools. |
| Shared broadcast message bus for invocation | OpenVoiceOS | **Reject** | The report calls out the broadcast property as a security limitation. Tools are called directly. |
| OVOS Python skill packaging | OpenVoiceOS | **Reject** | Incompatible with CIPHER's C++ core. |

## 7. Files, issues and ADRs proposed

Proposed on this branch (docs only):
- `docs/V05_TOOL_ARCHITECTURE.md` (this file)
- `docs/DECISIONS.md`: ADR 18, "Permissioned tool registry on existing PermissionPolicy", status PROPOSED
- `docs/ROADMAP.md`: v0.5 row to "DESIGN IN REVIEW"
- `docs/PROJECT_STATE.md`: section 10 next task

Proposed issues, opened only after owner approval of this design:
- Tool registry (built-ins only, lifecycle, audit) with tests for each denial path
- `time.now`, `system.info`, `memory.list` built-in tools
- Owner-typed `tool` REPL command
- Read-only filesystem tool with allowlisted root (phase 4)
- Ask-tier owner confirmation (phase 5)

## 8. Implementation phases

1. **Design review** (this document). Owner decisions required.
2. **Registry and lifecycle.** New library on `main`. Registration rules, lifecycle, audit, dry-run. Built-in tools `time.now`, `system.info`. Tests: each denial path, byte-identical file where a file is involved, duplicate and empty ID rejection, handler exception becomes `Failed` with `executed = true`.
3. **Owner command.** `tool list`, `tool <id> [args]`, `tool log`. Tests cover the REPL path and existing commands unchanged.
4. **Read-only filesystem.** `filesystem.read` under an allowlisted root, canonical path, symlink and junction rejection on Windows, size bound. Tests use a temp directory and a traversal attempt. Kept in v0.5 (owner decision 4).
5. **Owner confirmation for Ask tier.** Kept in v0.5 (owner decision 5). Only explicit owner `yes` runs an `Ask` tool; no write tool is added.

Each phase is its own PR, stacked as needed, with CI verified on the exact SHA.

## 9. Risks and unresolved decisions

- **In-process handlers cannot be time-bounded.** Mitigation: v0.5 tools are instant. Any slow tool waits for a subprocess runner.
- **Windows path canonicalization** (junctions, 8.3 short names, case) is hard to get right. Phase 4 needs tests on Windows specifically, and this is the largest security risk in the milestone.
- **Pattern matching on arguments** (`filesystem.read C:\*`) can be bypassed by `..` or alternate spellings. Matching must run on the canonical target, not the raw argument.
- **Audit persistence** is deferred. A crash loses the session log.
- **Single backend pointer** remains; tool results are not sent to a provider in v0.5. Feeding results back into model context is a later decision that needs its own privacy review.
- **Owner confirmation** (phase 5) may be needed sooner if the owner wants `Ask` tools in v0.5.

## 10. Owner decisions (all APPROVED)

1. **APPROVED:** architecture in sections 2–3 (tool ID, built-in-only registry, lifecycle, result shape, audit fields).
2. **APPROVED:** initial safe-tool list `time.now`, `system.info`, `memory.list`.
3. **APPROVED:** PR #12 is not resurrected as-is. Rewrite the useful concepts on current `main`, reusing outcome types, dry-run and useful tests. PR #12 is not merged.
4. **APPROVED:** `filesystem.read` stays in v0.5 as a later phase (phase 4).
5. **APPROVED:** owner confirmation for `Ask` tools stays in v0.5 (phase 5).
6. **APPROVED:** audit is session-only. Record attempts including denials. Do not persist arguments or tool outputs.
7. **APPROVED:** open the implementation issues listed in section 7.

No implementation, no billed probes, and no runtime expansion are part of this package. Hermes remains deferred. Obsidian was not synced: the MCP connection is refused (ECONNREFUSED).
