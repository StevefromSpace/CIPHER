# CIPHER Development Workflow

This document describes how CIPHER is developed by a human owner with AI assistance. It describes the process only; `docs/PROJECT_STATE.md` is the source of truth for what exists now.

## Development lifecycle

Every task follows this cycle:

```
PLAN → IMPLEMENT → TEST → REVIEW → CLEANUP → COMMIT / PR → MERGE → NEXT TASK
```

1. **Plan.** Read `docs/PROJECT_STATE.md` and `docs/ROADMAP.md`, then the milestone's design doc (for v0.5, `docs/V05_TOOL_ARCHITECTURE.md` section 8). Write the plan in a doc or PR description. Architectural changes need an ADR in `docs/DECISIONS.md` first.
2. **Branch.** Start from current `main`: `git switch -c feat/<topic>` (or `fix/`, `docs/`). One milestone phase per branch.
   - **Stacking exception:** when a phase depends on an unmerged PR, branch from that PR's head SHA and name the dependency in the PR body.
   - Use a sibling worktree (`git worktree add -b <branch> <path> <base>`) when the main tree has uncommitted work.
3. **Implement.** Keep the change bounded. Follow `AGENTS.md`: C++17, local-first, provider-independent, no speculative architecture.
4. **Test.** Run `powershell -ExecutionPolicy Bypass -File scripts\verify.ps1` on Windows. It configures, builds, and runs CTest out of source, the same steps CI runs. For a worktree, pass its path as the source or run `cmake -S <worktree>` directly, so the build matches the branch. The Python suites are in `RESET_RECOVERY.md` section 3.
5. **Review.** Run `git diff --check`, read the full diff, and address findings before the PR.
6. **Cleanup.** Apply the task-level cleanup rules below before committing.
7. **Commit / PR.** Commit on the feature branch only. Push with `-u`. Open a PR against its base (usually `main`) with a summary, the dependency, and a test plan. CI must be green on the exact head SHA; a green run on an earlier commit does not count.
8. **Merge.** The owner reviews and merges. Agents do not merge, and do not close PRs that another person owns.
9. **Next task.** After merge: delete the merged remote branch, remove its worktree, and delete the local branch only if it has no unique commits.

## Development hygiene

**Task-level cleanup (agents do this themselves).** Each agent removes temporary state its own task created, without asking the owner:
- temporary files and test output
- temporary worktrees and branches the task created
- pipeline job records, branches and worktrees created by the task's own test runs
- generated build output outside the repository

If the agent clearly created an item and no longer needs it, it may delete it. Check the item first (for example, `git status` clean, no unique commits), and record it in the final report.

**Scope.** Task cleanup stays scoped to the task. An agent does not start cleaning historical or unrelated state because it noticed it. If the state is ambiguous, preserve it and report it.

**Maintenance cleanup.** Stale branches, obsolete docs, and accumulated pipeline state are handled periodically, as separate maintenance work, not inside a feature task.

Owner-approval rules for destructive or outward-facing actions are in "Agent autonomy" below.

**Definition of done.** Every task finishes with:
- *Code:* implementation complete; relevant tests pass; no test weakened, skipped, or removed to make it pass.
- *Repository:* only the intended files changed; task-created temporary state removed; the working tree is understood; no unexplained artifacts.
- *Documentation:* updated where the change requires it, and not rewritten beyond that.
- *Verification:* build and test results recorded; known environmental failures and intentional leftovers documented.

Clean as you go, but do not stop a feature task to clean the whole repository.

## AI-assisted development

- **Antigravity** is the primary implementation environment.
- **Claude** is used for planning, reasoning, review, and bounded implementation.
- **Other AI tools** (Codex, Gemini, local models) are optional development helpers. Their configuration is not required to build or test CIPHER.
- **CIPHER itself must stay provider-independent.** No AI provider, paid API, or model download is a required runtime component. Assistant code lives behind `IAssistantBackend` and the `integrations/` adapters.
- AI tool state is not a single point of failure: project knowledge lives in Git (`AGENTS.md`, `docs/`), and the tool-specific settings are recreatable (see `RESET_RECOVERY.md` section 8).

## Agent autonomy

Agents may, without asking:

- inspect the repository, docs, and build output
- implement bounded work on a feature branch
- run the build and tests, and fix routine failures
- update documentation that matches the code
- remove temporary state their own task created (see "Development hygiene"), including generated artifacts under `%LOCALAPPDATA%\cipher-build-ninja` and finished pipeline worktrees
- commit on a feature branch and open a PR
- push a feature branch they created

Owner approval is required for:

- major architecture changes and new ADRs
- merging PRs, and closing PRs or branches with unmerged work
- releases and tags
- destructive history operations (history rewrite, force-push, `git gc --prune=now`)
- deleting project history, branches, or worktrees that have unique commits, uncommitted work, or uncertain ownership
- security-policy changes
- major dependency or provider changes

The aim is maximum safe autonomy with minimum routine approval friction.

## Local pipeline (`tools/pipeline`)

The developer pipeline runs two AI agents against a task inside an isolated Git worktree.

Lifecycle, as implemented in `tools/pipeline/orchestrator.py` and `tools/pipeline/git_manager.py`:

1. **Create.** `submit_task` creates a job record in the job store (`.agents/pipeline/jobs/*.json`).
2. **Worktree.** `GitManager.create_worktree` runs `git worktree add -b pipeline/job-<8 chars> .agents/pipeline/worktrees/job-<8 chars> HEAD`.
3. **Implement and review.** The implementation agent edits the worktree. The reviewer agent checks the diff. The verifier runs the build and tests.
4. **Repair loop.** Failed review or verification sends feedback back to the implementation agent.
5. **Approve and commit.** `approve_commit` commits the worktree changes after a fingerprint check confirms nothing changed since approval.
6. **Finish.** Successful jobs leave their worktree and branch in place. **This is the known gap:** nothing removes them after success.
7. **Cancel.** `cancel_job` removes the worktree and branch.

Known problems:

- Completed and failed jobs keep their worktrees, so about 30 worktrees and branches accumulated. Most were clean and merged; they were removed in the 2026-10-27 cleanup.
- Some implementation commits were created with the message `Agent commit` and a placeholder file (`docs/committed_file.md`). These are meaningless, and 34 are now unreachable. They are not a valid pattern for new work.

Lifecycle rule to follow until the pipeline is changed:

- When a job's changes are merged or abandoned, remove its worktree (`git worktree remove <path>`) and delete its branch if it has no unique commits. Do this with the owner's approval if the job has uncommitted work.
- Do not create commits only to record pipeline state.

Owner decision (not implemented): whether successful jobs should clean up their worktree automatically. That changes pipeline behavior and needs an owner decision.

## Pipeline design notes

Two earlier pipeline documents were recovered from job worktrees that were never committed. Copies are in `.recovery/pipeline-docs/` (ignored by Git). They are historical evidence, not authoritative. Their useful content was reviewed for this document. Do not commit them unchanged.
