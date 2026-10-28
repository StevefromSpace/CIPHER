# Third-Party Notices and Intellectual Property Provenance

CIPHER (Contextual Intelligence Platform for Home, Enterprise & Research) is an independent, local-first platform built with zero mandatory external runtime dependencies. One header-only library is vendored at build time (section 5).

During **Operation Source Intelligence** (Milestone v0.4), research clones of four open-source personal assistant projects were audited for architectural mechanisms, security invariants, and design patterns. The research clones reside strictly outside the CIPHER repository in external research directories.

No upstream source code from any of these projects is copied, vendored, or compiled into CIPHER's C++17 core libraries (`cipher_core`, `cipher_config`, `cipher_logger`, `cipher_memory`, `cipher_assistant`). All native C++ components (`RiskTier`, `PermissionPolicy`, `ContextBudget`, `EventMessage`) are original, clean-room reimplementations of proven architectural patterns.

This document records the provenance, upstream commit SHAs, license terms, and copyright notices of the researched projects in accordance with open-source license compliance.

---

## 1. OpenClaw

* **Repository:** [https://github.com/openclaw/openclaw](https://github.com/openclaw/openclaw)
* **Researched Commit:** `e2a63bc4ccb74927ad9ef64e99ff20ffe60cb6ed` (branch `main`)
* **Upstream License:** MIT License
* **Copyright:** Copyright (c) 2025-2026 OpenClaw Contributors
* **Architectural Influence in CIPHER:**
  - Concept of byte-bounded context budgets to prevent token window overflow, cleanly reimplemented as C++17 `ContextBudget` (`max_prompt_bytes`).
  - Analysis of tool profile boundaries (demonstrating that `minimal` profiles must still be guarded against default presence/gateway tools).
  - Decoupled, optional loopback adapter in `integrations/openclaw/`.
* **License Notice (MIT):**
```text
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## 2. Dodaios (formerly Personal Jarvis)

* **Repository:** [https://github.com/Dodaios/Dodaios](https://github.com/Dodaios/Dodaios)
* **Researched Commit:** `ae629b4b13c1b625b311eb905488cf9599fbe236` (branch `main`)
* **Upstream License:** Apache License 2.0 (releases >= 2.0.0; historical releases <= 1.6.0 were MIT)
* **Copyright:** Copyright (c) 2024-2026 Dodaios Contributors
* **Upstream Sub-Components & Provenance:**
  - `NousResearch/hermes-agent`: MIT License
  - `T3 Code`: MIT License
  - `Pierre Computer Company`: Apache License 2.0
* **Architectural Influence in CIPHER:**
  - Clean-room C++17 reimplementation of the 4-tier risk classification (`RiskTier`: `Safe`, `Monitor`, `Ask`, `Block`).
  - Strict priority evaluation order (`Blacklist` > `Whitelist` > `Default Tier`) cleanly reimplemented in `PermissionPolicy`.
  - Rejection of autonomous desktop control, computer use, and OS automation.
  - Decoupled, optional loopback REST adapter in `integrations/dodaios/`.
* **License Notice (Apache 2.0):**
```text
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

---

## 3. Leon

* **Repository:** [https://github.com/leon-ai/leon](https://github.com/leon-ai/leon)
* **Researched Commit:** `54611cb27cc3da785738e0219da2cb9926b4b847` (branch `develop`)
* **Upstream License:** MIT License
* **Copyright:** Copyright (c) 2019-2026 Louis Grenard / Leon AI
* **Architectural Influence in CIPHER:**
  - 4-tier functional decomposition hierarchy (`Skills` -> `Actions` -> `Tools` -> `Functions`).
  - Progressive tool-schema loading architecture (reserved for future reference in Milestone v0.5).
  - Decoupled, optional loopback HTTP query adapter in `integrations/leon/` (pinned to stable 1.x REST contract).
* **License Notice (MIT):**
```text
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## 4. OpenVoiceOS (OVOS)

* **Repositories:**
  - `ovos-core`: `8ca3cda026a9e018cf10bac3e6cafe26fca29302` (branch `dev`)
  - `ovos-messagebus`: `cb429394c608e593bf30f73f32f5f57f997a3afd` (branch `dev`)
  - `ovos-architecture`: `d3df840e6a1b41558dc9e35a5a61827e36bf00d5` (branch `dev`)
* **Upstream License:** Apache License 2.0
* **Copyright:** Copyright (c) 2021-2026 OpenVoiceOS Contributors
* **Architectural Influence in CIPHER:**
  - Clean-room C++17 reimplementation of standardized event wire message envelopes (`EventMessage`, modeled after `OVOS-MSG-1`).
  - Session correlation tracking across asynchronous boundaries.
  - Broadcast bus confidentiality warnings and prompt containment.
  - Decoupled, optional loopback messagebus adapter in `integrations/ovos/`.
* **License Notice (Apache 2.0):**
```text
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

---

## 5. nlohmann/json (vendored)

* **Repository:** [https://github.com/nlohmann/json](https://github.com/nlohmann/json)
* **Version:** 3.11.3 release asset `json.hpp`, vendored unmodified at `third_party/nlohmann/json.hpp`
* **SHA-256:** `9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6`
* **Upstream License:** MIT License
* **Copyright:** Copyright (c) 2013-2023 Niels Lohmann
* **Use in CIPHER:** JSON encoding and decoding in the Ollama backend (`src/ollama_backend.cpp`) only. Compiled into `cipher_ollama`; not exposed in public headers.
* **License Notice (MIT):**
```text
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
