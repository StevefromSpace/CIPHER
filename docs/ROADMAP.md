# Development Roadmap

The dates are planning targets, not promises. Work at a pace that preserves learning and quality.

## October 2026: v0.1 foundation

- [x] Establish repository structure and founding principles.
- [x] Create a C++17 core and basic command interface.
- [x] Add a repeatable build and starter tests.
- [x] Build locally on the owner's computer (Windows, MSVC via VS 2022 Build Tools, Ninja generator).
- [x] Create the GitHub repository and push the initial baseline (commit 5501749, branch main).
- [ ] Review every source file and understand the execution path.
      *(The owner must do this step; it cannot be marked done by the agent.)*

## October 2026: v0.1.1 foundation hardening

- [x] Add GitHub Actions CI (Ubuntu + Windows, build + CTest).
      See `.github/workflows/ci.yml`.
      Status: workflow pushed; first run pending on GitHub.
      The result will appear at:
      https://github.com/StevefromSpace/CIPHER/actions
- [x] Fix MSVC localtime deprecation warning (localtime_s / localtime_r platform helper).
- [x] Expand test coverage from 7 to 18 cases (empty input, whitespace, time,
      echo edge cases, case sensitivity, leading-space stripping).

## Next milestones

### v0.2: Configuration and diagnostics

- Read a simple local configuration file.
- Improve error reporting and command parsing.
- Add structured logs without storing secrets.

### v0.3: Persistent memory foundation

- Define what should and should not be remembered.
- Start with an exportable local format and schema versioning.
- Add tests for saving, loading, and migrating memory.

### v0.4: Replaceable AI runtime interface

- Define a small inference interface independent of a specific provider.
- Prototype with a local model/runtime only as an optional adapter.
- Keep the core useful when no model is available.

### v0.5: Permissioned tools

- Introduce a tool registry, explicit permissions, action logs, and dry-run mode.
- Begin with safe read-only tools before allowing changes.

### Later: voice, interfaces, planning, learning, and self-improvement

Each capability should be introduced behind tests, narrow permissions, observable behavior, and recovery paths. Model training and self-upgrading are advanced milestones, not prerequisites for a useful first version.
