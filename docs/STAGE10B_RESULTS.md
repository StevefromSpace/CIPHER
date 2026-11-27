# Stage 10B results (billed live validation)

Date 2026-11-27/28. Owner-approved hard budget $2.00. claude CLI 2.1.332, model alias `haiku` (claude-haiku-5-5), gh authed. All claude calls below were BILLED.

## A. ClaudeToolAgent real tool-use protocol (billed, $0.0170 measured)
New manual validator `tests/claude_tool_agent_live_validation.cpp` (target `cipher_claude_tool_agent_validation`, not in CTest). With an empty transcript the agent returned `{"tool":"fs.read","path":"README.md"}`, parsed as a valid step. PASS (local, live).

## B. Issue #7 probe: `--safe-mode` (billed, about $0.05 measured)
Positive control: the user-level instruction "skill graphify, trigger /graphify" (`~/.claude/CLAUDE.md`). Prompt "Do your instructions mention a skill named graphify ...? YES or NO", haiku, `--tools ""`.
- without `--safe-mode`: YES; with `--safe-mode`: NO.
- A first control using a project-level CLAUDE.md in a scratch dir did not fire even without safe-mode (invalid control, discarded). Only the user-level instruction is proven; user-level hooks were not separately probed.
Result: safe-mode suppresses user-level customizations on 2.1.332.

## C. Disposable exercise (real gh + worktree + claude)
Through REPL `job new/task/step` with `orchestration_*` config (model haiku, budget 0.15/call, qa=check).
- Attempt 1 (issue #71, closed): implementation done in the job worktree, then job BLOCKED: `PR: BLOCKED gh pr list failed (exit 1)`. Cause (inferred from the fix): REPL started from a non-git cwd; gh runs without `--repo` and uses cwd.
- Attempt 2 (issue #73, REPL started inside the repo clone): IMPLEMENTATION -> PR (PR #74, branch `cipher/job-73`, docs-only file `docs/disposable-stage10b.md`) -> QA. All 5 CI checks passed (ubuntu/windows build+test, llama.cpp runtime x2, python). PR #74 closed unmerged, branch deleted, issues closed with comments. No merge, tag, release, LICENSE or main change.

## Bugs / papercuts found (not fixed; no src change)
1. gh calls depend on the REPL cwd being the repo (no `--repo`/cwd pinning) -> BLOCKED otherwise.
2. `job step` needs `cmake` on PATH even when `orchestration_qa=check` (`executable not found on PATH: cmake`).
3. The agent re-wrote the same file about 12 times for a one-line task (haiku); wasteful, within step limit.
4. `ClaudeToolAgent` discards `total_cost_usd`, so job cost is not observable; per-call cap was $0.15.
5. REPL input with a UTF-8 BOM makes the first command "Unknown command".

## Cost
Measured: A $0.017, B about $0.05. Job runs (attempts 1+2, roughly 25 haiku calls) were not measured; estimated under $1 by call count x observed $0.002-0.02 per call. Total estimate well under the $1.50 stop line.
