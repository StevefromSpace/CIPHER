# CIPHER Milestone and Release Procedure

A milestone is complete only when every item below holds. This is preparation: it does not authorize any release by itself.

## Completion checklist

- [ ] **Clean state.** `git status` shows no uncommitted work on the milestone branch. Generated files are in the ignored build directory.
- [ ] **Version and milestone.** `CMakeLists.txt` `project(... VERSION ...)`, `README.md`, and `docs/PROJECT_STATE.md` name the same version and milestone.
- [ ] **Build.** `scripts\verify.ps1` completes with no errors on Windows. CI builds on ubuntu-24.04 and windows-2022.
- [ ] **Tests.** All CTest tests pass (27/27 on `main` `659454e`, Windows Release with Ninja, no `CIPHER_WITH_LLAMA`, 2026-11-27). The `integrations` Python suite passes. The `tools/pipeline` suite passes only after an in-tree `build\` exists (see `RESET_RECOVERY.md` section 3).
- [ ] **CI on the exact SHA.** The PR head SHA has a green CI run. A green run on an earlier commit does not count.
- [ ] **Documentation.** `PROJECT_STATE.md` describes what exists. `ROADMAP.md` marks the milestone status. `DECISIONS.md` has an ADR for each architectural decision in the milestone.
- [ ] **Architecture consistency.** Core does not depend on memory or assistant layers, per `ARCHITECTURE.md`.
- [ ] **No unresolved release blocker.** `PROJECT_STATE.md` section 9 lists no blocker for this milestone.
- [ ] **Owner approval.** The owner has reviewed and approved the merge. Agents do not merge.

## Tag and release procedure

Run only after the owner approves:

1. Merge the PR into `main` with the owner's approval.
2. On the merged `main` commit, run the completion checklist again, including CI.
3. Create an annotated tag: `git tag -a vX.Y.Z -m "CIPHER vX.Y.Z"`, then push it with `git push origin vX.Y.Z`.
4. Record the tag and SHA in `docs/PROJECT_STATE.md`.

Existing rollback tags (`rollback-hardening-gate-*-baseline`) are historical baselines. Do not move or delete them.

## Not covered

- Binary packaging and installers. CIPHER has no release artifacts yet.
- Open-source licensing. No license has been chosen; this is an intentional deferral (DECISIONS ADR 23). Do not add one.
