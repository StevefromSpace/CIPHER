# Orchestration Status (Stages 1-9 and 10A, non-live)

Status of the `cipher_orchestration` library against [AGENT_ROLE_CONTRACT.md](AGENT_ROLE_CONTRACT.md), which is the governance reference.
This document records what is implemented and tested, what is real versus fake, and what is missing.
It does not change the contract.

## Stage status

| Stage | Scope | Status |
|---|---|---|
| 1-4 | State machine, orchestrator, file job store, GitHub facts, scheduler, worktree allocator | Implemented and tested |
| 5 | Typed GitHub argv adapter, governed merge gates, merge lock | Implemented and tested |
| 6-8 | Claude executor and role executor, process runner, recovery, event log | Implemented and tested |
| 9 | Executable resolver, `gh` process runner | Implemented and tested with fakes |
| 10A | Real git worktree backend, strict JSON GitHub read, Claude executor hardening | Implemented and tested (this PR) |

## Components

| Component | Real or fake | Tested with | Notes |
|---|---|---|---|
| State machine, orchestrator, file job store | Real | Unit, flow, store, recovery tests | Two executions max, one BLOCKED diagnostic, security escalation before Review. `recover()` never re-runs an executor. |
| Scheduler, merge gates, merge lock, event log | Real | Concurrency and flow tests | Max 3 jobs. A stale `merge.lock` blocks all merges and is never auto-deleted. |
| Worktree allocator | Real | Worktree and concurrency tests | Job-id validation, strict root containment, owner records. |
| `GitWorktreeBackend` | **Real** (runs `git`) | Worktree tests against temporary repositories | See below. Deterministic fake backend is kept for other tests. |
| GitHub adapter, TSV read | Real argv builder | Adapter tests with fake runner | Lenient legacy read. Does not populate checks or reviews. |
| GitHub adapter, JSON read (`read_pull_request_detail`) | **Real** parser | Tests plus the PR #49 fixture | See below. |
| `gh` process runner, executable resolver | Real | Fake process runner | `argv[0]` must be an absolute path. Nothing resolves `gh` from PATH implicitly. |
| Process runner | Real, Windows and POSIX | Child-process tests | Timeout, output cap, no shell. |
| Claude executor, role executor | Real argv, parse and mapping | Fake runner only | No paid call is made by any test. |

## Worktree capability

`GitWorktreeBackend` creates detached worktrees under a managed root, one per validated job id, and removes them again.

- Job ids are validated before any path is built. Traversal (`..`, separators, drive prefixes) and root escape are rejected.
- Access to the owner's main checkout is refused. `CIPHER-wt-release` and `tools/pipeline/` are protected and never touched.
- Removal verifies the worktree is managed (owner record present, path under the root, registered with git) before deleting. Unmanaged paths are rejected.
- Stale worktrees are recovered, dirty worktrees are not silently destroyed, and repeated cleanup is safe.
- Covered by `cipher_orchestration_worktree_tests`: creation, invalid ids, traversal, root escape, protected roots, remove, unmanaged rejection, dirty and stale recovery.

## GitHub read capability

`GhAdapter::read_pull_request_detail` runs `gh pr view N --json number,state,headRefOid,baseRefOid,mergeable,statusCheckRollup,reviews` and parses it strictly. It fails closed (throws) on a non-zero exit, empty or malformed JSON, missing or mistyped fields, non-full or uppercase SHAs, unknown enum values, and a PR number that does not match the request. Missing values are never invented.

- Mergeable is true only for `MERGEABLE`. `UNKNOWN` is not mergeable.
- Checks: failure beats pending beats passed. An empty rollup is pending. Unknown check types or conclusions are rejected.
- Reviews: only `OWNER`, `MEMBER` and `COLLABORATOR` reviewers count. The latest decisive review per author wins. Any outstanding change request wins. An approval must name a full commit SHA.
- Review text, PR bodies, comments and commit messages are never read, so untrusted text cannot authorize anything.
- The one authorized live read of PR #49 was saved, sanitized (URLs and timestamps removed), as `tests/fixtures/github_pr49.json`. Automated tests use that fixture only.
- Write operations (comment, merge) still exist as typed argv builders from Stage 5. Nothing calls them from tests.

## Claude execution boundary

The Claude call is tool-less. It can answer Research and Architecture roles but cannot edit files, push, or produce a head SHA.

- Model, budget and prompt are validated before launch. Invalid input throws and the runner is never invoked.
- A runner exception becomes `RUNNER_ERROR`, then `BLOCKED` at the role executor. Timeouts, non-zero exits, malformed results and Claude `is_error` map to failure.
- Exactly one runner invocation per attempt. There is no hidden retry. The two-execution orchestration limit is unchanged.

## REPL integration (Stage 11)

The `cipher` executable links `cipher_orchestration`. With `orchestration_dir=<folder>` in `cipher.conf`, the REPL accepts `job new <issue> [research] [design] [security]`, `job status|step|recover <issue>`, `job list`, `job log`. Job state persists in `FileJobStore` and survives restart (tested). `events.log` is best-effort diagnostic history written after the record is saved: if an append fails the command prints a WARNING and that line is not back-filled. Verified locally: unit tests plus a manual run of the real executable.

**Placeholder:** the default executor escalates every role to `NEEDS_HUMAN` (`RoleNotSupported`). No role does real work from the REPL until the tool-enabled implementation executor exists.

## Implementation executor (Milestone A)

- `WorktreeTools` (fs.read/list/search/write/edit, process.run by pre-registered command id, git.status/diff/log): typed calls, `PermissionPolicy` check on every call, path containment (no absolute, `..`, `.git`), protected write paths (`.git`, `.github`, `tools/pipeline`, role contract), size/output limits, audit log. The model never gets a shell; `process.run` only runs argv registered by CIPHER.
- `ImplementationExecutor`: allocates the job worktree, loops `ToolAgent` steps (step limit, no internal retry), runs the verify command, commits with `git add -A` and returns `ImplementationComplete` with the full HEAD sha. Two-execution limit comes from the existing Orchestrator policy. Other roles, and the PullRequest step, escalate to `NEEDS_HUMAN`.
- REPL: `job task <issue> <text>` stores the task; with `orchestration_repo`, `orchestration_base_sha`, `orchestration_worktree_root` set, `job step` runs `ImplementationRig`.
- **Verified:** automated tests with a scripted agent and real git (real worktree, real file edit, real commit, persistence and restart reload, failure and exhaustion paths). Windows ctest 29/29.
- **Not verified:** `ClaudeToolAgent` has only been tested with a fake runner; no live Claude call has been made. It is paid per step.
- **Not implemented:** independent review, autonomous merge. Security-sensitive jobs stay `NEEDS_HUMAN`.

## Pipeline roles (Milestone C) - fake-tested only

`PipelineExecutor` (src/orchestration/pipeline_roles.cpp) plugs into the existing state machine as the fallback of `ImplementationExecutor`; there is no second engine.

- **PR creation** (Implementation role at the PullRequest state): verifies worktree HEAD == job head sha, refuses if more than one open PR exists for the job branch `cipher/job-<n>`, pushes with a plain (never forced) `git push`, creates the PR only if none exists (`GhAdapter::create_pull_request`, validated branch names, head != base), re-reads the PR with the strict JSON reader and requires open + head sha match. Never merges. PR number/head/base go to the evidence file and the `PullRequestOpened` event.
- **QA**: independent of implementation (separate argv registered by CIPHER, no file writes). Empty diff, source without tests, non-zero exit => `ChangesRequested`; timeout, unknown command id, no configured commands, or HEAD moving during the run => `Blocked`. Out of the box the only approved command is `git diff --check`; real test commands must be added by the owner.
- **Reviewer (mechanical)**: family `cipher-mechanical`. Checks protected paths (tools/pipeline, .github, third_party, .claude, AGENTS.md, role contract, CMakeLists.txt, env/secret/credential names), diff size, secret markers, source-without-tests. Security-sensitive jobs always get `NEEDS_HUMAN`. **This is not an independent or semantic review** and must not be counted as one.
- **CI observer** (`observe_ci`): reads PR checks, no event while pending, `CiResult` when decided, `Blocked` on closed PR / head mismatch / malformed output. Driven from the REPL by `job ci <issue> <pr>` (PR #53; fake-tested, needs `gh` on PATH).
- **Verified:** automated tests with fake git and fake gh (PR created / reused / duplicate / mismatch / push failure / ambiguous output, QA and review outcomes, CI states). No live GitHub mutation was performed.
- **Externally verified (one authorized live run):** `GhAdapter::create_pull_request` via `cipher_gh_pr_validation` (manual, `tests/gh_pr_live_validation.cpp`) created PR #55 (docs-only branch `validation/docs-pr-creation`, head `f05b22243270dda676ba6f70ad86dbd0710fba04`, base `main` `659454eed0407607180a313f0e5936280d23454f`). The PR number parsed from real `gh` output, and the strict JSON read returned the same head SHA; it refuses to create if an open PR exists for the branch. PR #55 is validation-only and unmerged.

## Configurable QA (Milestone D)
- Config key `orchestration_qa` (comma list, default `check`) selects approved profiles: `check` (git diff --check), `build` (cmake configure+build, out-of-tree in `<worktree_root>/qa-build-<n>`), `ctest`, `python` (unittest discovery of `integrations/` and `tools/pipeline/tests`; fails on zero tests or skips). `src/orchestration/qa_profile.cpp`.
- Commands are fixed argv with absolute executables (no shell), bounded by the 600 s command timeout and the runner output cap. Unknown/duplicate profiles, `ctest` without `build`, and missing tools throw, so a check is never silently skipped.
- QA verdicts: PASS (Approve), FAIL (ChangesRequested), BLOCKED. Blocked if HEAD differs from the job head before or after, a command times out/overflows, or `git status --untracked-files=no` shows tracked files modified by QA.
- Evidence lines record command id, exact head SHA and exit code in the job evidence log.
- Fake-tested: `tests/test_orchestration_pipeline.cpp`. **Locally verified (Windows, real processes):** `build-stage10a\cipher_qa_real_validation.exe <git> <cmake> <ctest> <empty dir>` (manual, not in CTest; needs MSVC env) runs `check,build,ctest` through `SystemProcessRunner` in a disposable git repo: PASS on exact head with per-command SHA evidence and out-of-tree build; failing ctest -> FAIL; 1.5 s timeout kills a 30 s sleep -> BLOCKED in ~1 s; 64-byte output cap -> BLOCKED; QA overwriting a tracked file -> BLOCKED; head mismatch -> BLOCKED (13/13 checks). `qa_timeout_ms` is now a `PipelineSettings` field. Not externally verified: Linux real run, `python` profile. Locally on Windows, `tools/pipeline/tests` shows 6 failures (FAILED_VERIFICATION in orchestrator tests) while CI on Linux passes; the `python` profile is therefore unproven on Windows.
- Pilot readiness: still not ready. Independent security review is unavailable, #51/#52/#53 are unmerged, Claude tool use is unverified live.

## Antigravity / Gemini investigation

Checked on the owner machine: `antigravity`, `agy` and `gemini` are not on PATH. No documented, verified way to invoke Gemini programmatically through Antigravity, capture structured output, or verify the reviewing model's identity has been established. No CLI was invented and none was installed. Consequence: no independent reviewer exists; security-sensitive tasks stay `NEEDS_HUMAN`. Re-open this item only when the owner supplies a concrete, documented invocation surface.

## Pilot prerequisites (not started)

1. Owner authorizes one small paid Claude validation of `ClaudeToolAgent` (budget stated up front); until then the real tool-use protocol is unverified.
2. Owner defines real QA test commands (the default only runs `git diff --check`).
3. A disposable docs-only or test-only issue, disposable branch, isolated worktree, real PR, CI, no autonomous merge.
4. (done in PR #53) `job ci` drives `observe_ci` from the REPL.

## Governance boundaries

- Security-sensitive jobs remain `NEEDS_HUMAN`. No independent security-review backend exists, and none is faked.
- The implementation agent cannot approve its own work.
- Gemini is available to the owner through Antigravity, but no verified executable integration exists. Gemini CLI is not installed and no Antigravity CLI is assumed.
- Autonomous merging is not implemented. `evaluate_merge_gates` and `MergeLock` are tested in isolation, and nothing re-reads job B's PR after job A merges.
- A live pilot is intentionally deferred.

## Known limitations

- No tool-enabled implementation executor, so no end-to-end run is possible.
- The orchestration library is not wired into the `cipher` executable.
- `GitWorktreeBackend` is tested against local temporary repositories, not against the real project repository.
- The JSON read has not been run against open PRs with live checks, requested changes or `StatusContext` entries. Those paths are covered by synthetic tests only.
- `gh` and `claude` paths must be supplied by the caller as absolute paths.
- The budget flag is not a hard ceiling: a call capped at $0.05 once cost $0.138.

## Contract tensions

- Round 3 of review is unreachable under the 2-execution limit (sections 5.1 and 5.2 item 3).
- Section 5.3 asks for a one-shot higher-tier re-evaluation on the same head, which section 5.2 item 3 forbids. It is treated as a one-shot exception that does not count as a round.
- Section 8.2 defines 7 merge gates. The implementation has those 7 plus 3 adapter preconditions (PR open, head unchanged, mergeable).

## Next checkpoint

Stage 10B, only after the owner approves it: a tool-enabled implementation executor and a disposable-branch exercise of the real `gh` and worktree backends. Pilot requirements stay as before: owner-approved paid-call budget, docs-only and non-security path.
