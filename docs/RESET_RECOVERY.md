# CIPHER Reset Recovery Guide

Purpose: after a clean Windows reinstall, rebuild the CIPHER development environment and resume work from GitHub alone. No secret values are stored here. Environment variables are named, never valued.

Verified state at the time of writing: `origin/main` = `5b2b583` (includes PR #17, the v0.5 design). Re-check the head with `git ls-remote origin main` before relying on it.

---

## 1. Prerequisites (install on a fresh machine)

| Tool | Requirement | Verified version / notes |
|------|-------------|--------------------------|
| Git | Any recent Git for Windows | Required to clone. |
| GitHub CLI (`gh`) | Authenticated to the `StevefromSpace/CIPHER` repo | Required for PR and CI checks. Run `gh auth login` once. |
| Visual Studio 2022 Build Tools | MSVC x64 toolchain (C++ workload) | The local build used MSVC 2022 with the Ninja generator. |
| CMake | 3.16 or newer | Local copy came with the VS Build Tools (`Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`). Not on `PATH` by default. |
| Ninja | Optional | Used with `-G Ninja` by the local scripts. |
| Python | 3.11 (CI). Local: 3.14 also used. | Needed only for the Python test suites and the developer pipeline. |
| Python package | `mcp>=1.0.0` (from `requirements-dev.txt`) | Only dependency. The C++ core has no third-party libraries. |
| Node.js / npm | Not required by CIPHER | Only if you use Claude Code (it is installed via npm). |
| Claude Code CLI | Optional, for `ask claude` (PR #3 only) | Version verified: 2.1.310. Authenticate interactively with `claude` login; never paste tokens into files. |
| Hermes | Not installed. Deferred (Issue #8). | Not needed for any merged code. |

## 2. Clone

```powershell
git clone https://github.com/StevefromSpace/CIPHER.git
cd CIPHER
git checkout main
git log --oneline -1   # expect 5b2b583 or a later main commit
```

## 3. Build and test (verified commands)

Use a Developer shell (or run `vcvars64.bat` first). With Ninja:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --clean-first
ctest --test-dir build --output-on-failure
```

The README documents the Visual Studio generator as well:
```bat
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Python suites (the same ones CI runs):
```bat
python -m pip install -r requirements-dev.txt
python -m unittest discover -s integrations
python -m unittest discover -s tools/pipeline/tests
```

Whitespace check before any commit:
```bat
git diff --check
```

Known limits: `cmake` and `ctest` are not on `PATH` on this machine by default. Use the full path to the VS-bundled `cmake.exe`. Do not hardcode the user name or drive paths in scripts; derive them from the installed Visual Studio location.

## 4. Configuration

- `cipher.conf` is user-local and gitignored. Create it by copying `cipher.conf.example`.
- CIPHER reads no environment variables itself.
- Persistent memory file: `memory_file` in `cipher.conf` (see the example). The memory file is user data. It is not in Git.

## 5. Optional AI/provider setup (not required for CIPHER to build or run)

- `ask claude` (PR #3 only, not on `main` yet): requires the Claude Code CLI on `PATH` and an interactive login. Run the login with the `!` prefix in the Claude Code prompt so output stays in the session. Environment variable names involved: `ANTHROPIC_BASE_URL` (set on the current machine; value not recorded here, investigate before reuse). Do not put keys in any file.
- Each `ask claude` call is billed. Automated tests never make live calls.
- `ask hermes` is unavailable (Hermes not installed; fails closed).

## 6. Claude Code and project instructions

Restore these from your user profile, not from Git:
- `~/.claude/CLAUDE.md` and `~/.claude/rules/ecc/**` (global rules). Their key content is summarized in `AGENTS.md` and `docs/PROJECT_STATE.md` section 9.
- `~/.claude/settings.json` (keep `includeCoAuthoredBy` as your own choice).
- Plugins: ECC, Superpowers, Context7. Reinstall through Claude Code's plugin manager. Optional for building CIPHER.

Repository instructions that must remain in Git: `AGENTS.md`, `GEMINI.md`, `docs/`.

## 7. Repository state: branches, PRs, issues

Merged to `main`: PR #1, #2, #13, #14, #15, #16, #17 (v0.2, v0.3, v0.4 and the v0.5 design).

Open PRs (all Draft; none merged):

| PR | Head branch | Base | Head SHA | Purpose | Dependency |
|----|-------------|------|----------|---------|-----------|
| #3 | `feat/assistant-cli-providers` | `feat/repl-memory-commands` (merged) | `5b72fcf` | `ask claude`, `ai list`, process runner. | Needs retarget to `main` before merge. Resolve the Windows `bInheritHandles` change (Issue #5 fix already in head). |
| #9 | `docs/autonomous-operating-policy` | `research/assistant-source-intelligence` (merged) | `b65a918` | AGENTS.md autonomous operation policy. | Owner review pending (m00828). Retarget to `main` first. |
| #11 | `fix/memory-inspection-regression` | `research/assistant-source-intelligence` (merged) | `f9b44f9` | Issue #4 test seam for memory inspection failure. | Retarget to `main`; CI green on `f9b44f9`. |
| #12 | `feat/readonly-tool-registry` | `feat/assistant-cli-providers` (PR #3) | `ee498ea` | Parked prototype. | Do not merge. Superseded by the v0.5 design (rewrite on `main`). |

Open issues: #6 (POSIX setsid descendants, document only), #7 (verify `claude --safe-mode` suppression; no billed calls), #8 (Hermes, deferred). Closed: #4, #5.

Tags on origin: `rollback-hardening-gate-baseline`, `rollback-hardening-gate-2-baseline`, `rollback-hardening-gate-2.1-baseline`.

Worktrees: about 35 local worktrees (`CIPHER-wt-*` and `.agents/pipeline/worktrees/job-*`). Their branches are all pushed. They are disposable after reset.

## 8. Local-only items (not in Git) and recovery

| Item | Location | Class | Action after reset |
|------|----------|-------|--------------------|
| Persistent memory store | `.agents/memory/cipher_memory.json` (JSON-lines, 14 lines at audit time) | Must restore if you want your memories | Copy from the old disk before reset, or accept loss. It is gitignored and is user data. Its content was not inspected by the audit. |
| MCP configuration | `.agents/mcp_config.json` (keys: `graphify`, `cipher-memory`, `cipher-memory.env.MEMORY_FILE_PATH`, `cipher-pipeline`) | Must restore | Recreate from the key list. Set `MEMORY_FILE_PATH` to the memory file path. |
| Build output | `build/`, `build-win/` | Disposable | Regenerate with the commands in section 3. |
| Graph output | `graphify-out/` | Useful | Regenerate with graphify. |
| Scratch | `scratch/` | Disposable | Not needed. |
| Pipeline jobs | `.agents/pipeline/jobs`, `.agents/pipeline/worktrees` (job worktrees are created by the pipeline test run; `job-bc904cf3` was created by the audit run and sits at `5b2b583`, no unique commits) | Disposable | Every `pipeline/job-*` branch (32) and `experiment/openclaw-bridge` have **zero** commits missing from GitHub. Their commits are reachable from remote refs, so they can be dropped. |
| Research reports | `C:/Users/Johan Joseph/Documents/CIPHER-upstream-research/reports/` (outside the repo) | Must restore before reset | Conclusions are carried into section 9 below. Copy the folder to a backup location if you want the full reports. |
| Stashes | none | — | — |
| Uncommitted changes in main worktree | none at audit time | — | — |

## 9. Research conclusions (durable)

Each entry: source, idea, decision, CIPHER implication. Full detail is in `docs/V05_TOOL_ARCHITECTURE.md` section 6.

- **OpenClaw:** its `minimal` tool profile still exposes gateway/presence tools. Adopt an explicit deny-all policy; never rely on a profile name. Adapt its multi-layer policy as one `PermissionPolicy` evaluation.
- **Dodaios:** precedence Blacklist > Whitelist (downgrade to Safe) > tool default > policy default; fnmatch on `"<tool> <args>"`; Monitor tier goes to an audit stream. Adopt the precedence and the monitor audit idea.
- **Leon:** progressive schema loading and a dynamic tool manager are Reject for v0.5 (preview, breaking changes). Adopt per-profile secret isolation and no tokens in tool-call logs.
- **OpenVoiceOS:** the shared broadcast bus is a security limitation, so reject it for tool invocation. Python skills are incompatible with CIPHER's C++ core: Reject.
- **Licensing and security notes:** see the report folder; not re-summarized here.

## 10. Known limitations and open items

- No Ollama or llama.cpp report or clone exists. Not blocking.
- Issue #6: POSIX process-tree termination misses descendants that call `setsid`. Document only.
- Issue #7: `claude --safe-mode` suppression of user instructions is unverified. No billed calls are permitted until the owner approves.
- Issue #8: Hermes not installed; adapter fails closed by design.
- Obsidian MCP is unavailable (connection refused). Notes are not synced.
- Pipeline Python tests failed on a scratch branch earlier and did not reproduce on the fix head; not investigated.

## 11. Post-reset verification checklist

1. `git clone` completes; `git log -1` on `main` shows the expected SHA.
2. `gh auth status` shows the correct account.
3. `cmake --version` works (from full path if not on `PATH`).
   Note: the pipeline test adds one job worktree per run; remove them only after confirming they have no unique commits.
4. Configure, build (`--clean-first`), and `ctest` pass with the Ninja commands in section 3.
5. `python -m unittest discover -s integrations` and `-s tools/pipeline/tests` pass.
6. `git diff --check` is clean.
7. Restore `.agents/memory/cipher_memory.json` and `.agents/mcp_config.json` from backup if needed.
8. `cipher.conf` created from `cipher.conf.example`.
9. Claude Code signed in, if you need `ask claude`.
