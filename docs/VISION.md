# CIPHER: Founding Vision

## Name

**CIPHER** means **Contextual Intelligence Platform for Home, Enterprise & Research**.

It is a platform for a long-lived, local-first assistant that can understand context, help people think and work, operate approved tools, and gradually become more capable without surrendering control of its critical systems to a single provider.

## Long-term direction

CIPHER is intended to grow toward:

- Natural conversation through text and voice.
- Persistent, searchable, user-controlled memory.
- Context-aware assistance across personal computing, home labs, research, and business workflows.
- Optional perception features such as image understanding and, later, authorized sensors.
- Multi-step planning and tool use with clear reporting of actions and results.
- A replaceable AI runtime and a path toward models and training workflows we control.
- Safe diagnostics, testing, recovery, and carefully gated self-improvement.
- Interfaces that can evolve independently of the core.

These are long-term goals, not claims about features that exist today.

## Independence charter

CIPHER shall be locally operable, independently maintainable, and free from mandatory dependence on any single AI provider. Its critical components shall be replaceable by implementations we control.

This means:

- The core must not require a hosted AI API to start or perform its basic duties.
- Provider integrations, if added, must be optional adapters with clearly defined interfaces.
- The user must retain control of CIPHER's source, local data, configuration, and permission system.
- Data formats and interfaces must be documented to reduce lock-in.
- Network access must be explicit and limited to capabilities that need it.
- We should prefer well-understood, portable foundations over unnecessary framework churn.

Independence does not mean pretending that every dependency must be rewritten immediately. It means critical dependencies are identified, bounded, replaceable, and never hidden behind claims of autonomy.

## Safety and authority

CIPHER may become more autonomous over time, but autonomy must be earned through testing and permission boundaries.

- Read-only actions can be low-friction.
- Changes to user projects should be reviewable and recoverable.
- Destructive actions, credentials, purchases, and consequential external actions require explicit authorization.
- Self-modification must happen in an isolated workspace, pass tests, and preserve a rollback option.
- The system must not be able to silently remove or rewrite its own security boundaries.
- It must report what it did, what failed, and what it could not verify.

## How we build

We will use AI tools to accelerate development and learning. AI-generated code is a proposal, not proof of correctness. Core behavior must be understandable, tested, and reviewed.

The project will grow in working increments. A small, dependable capability is more valuable than a large demo that only works when everything goes right.

## First milestone

CIPHER v0.1 establishes a local C++ core, a simple command-line interface, a build process, and repeatable tests. It does not yet include a language model, persistent memory, network access, voice, or computer control.
