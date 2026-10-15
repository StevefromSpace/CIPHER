# Development Roadmap

The dates are planning targets, not promises. Work at a pace that preserves learning and quality.

## October 2026: v0.1 foundation

- [x] Establish repository structure and founding principles.
- [x] Create a C++17 core and basic command interface.
- [x] Add a repeatable build and starter tests.
- [x] Build locally on the owner's computer (Windows, MSVC via VS 2022 Build Tools, Ninja generator).
      *(Confirmed: build succeeded locally before the initial push.)*
- [x] Create the GitHub repository and push the initial baseline (commit 5501749, branch main).
      *(Confirmed: repository is at https://github.com/StevefromSpace/CIPHER)*
- [ ] Review every source file and understand the execution path.
      *(The owner must do this step; it cannot be marked done by the agent.)*

## October 2026: v0.1.1 foundation hardening

- [x] Add GitHub Actions CI (Ubuntu + Windows, build + CTest).
      See `.github/workflows/ci.yml`.
      Runners pinned to ubuntu-24.04 and windows-2022.
- [x] Fix MSVC localtime deprecation warning (localtime_s / localtime_r platform helper).
- [x] Expand test coverage from 7 to 18 cases (empty input, whitespace, time,
      echo edge cases, case sensitivity, leading-space stripping).

## October 2026: v0.2 — Configuration and diagnostics

- [x] Add a configuration subsystem: optional `cipher.conf` (key = value format).
      Supports `log_level` and `log_file`. Unknown keys and invalid values are
      rejected with line-number error messages.
- [x] Add a structured logger: four levels (DEBUG, INFO, WARN, ERROR), stderr by default,
      optional file output, graceful degradation when the file cannot be opened.
- [x] Integrate config and logger into the startup sequence in `main.cpp`.
      Configuration errors stop startup with a clear message.
      A missing config file uses sensible defaults — no error.
- [x] Provide `cipher.conf.example` with documentation and a security reminder.
      Exclude `cipher.conf` from Git; keep the example tracked.
- [x] Add config tests (test_config.cpp) and logger tests (test_logger.cpp).
- [ ] Review the v0.2 source files and understand the configuration and logging code.
      *(The owner must do this step.)*

## October 2026: v0.2.1 — Version consistency and test alignment

- [x] Centralize application version in `CMakeLists.txt` via CMake project `VERSION`.
- [x] Generate `include/cipher/version.hpp` using `configure_file()` from `version.hpp.in`.
- [x] Ensure `CipherCore::version()`, `help`, `about`, and `status` dynamically report `0.2.0`.
- [x] Reconcile and expand test coverage across the repository:
      * `cipher_core_tests`: 20 command cases + 1 direct version constant check (21 assertion checks).
      * `cipher_config_tests`: 19 test functions (56 assertion checks).
      * `cipher_logger_tests`: 11 test functions (28 assertion checks).
      * Total: 51 test cases/functions and 105 individual assertions across 3 CTest targets (all passing).
- [x] Document working-directory configuration loading assumption in `docs/ARCHITECTURE.md`.

## Next milestones

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
