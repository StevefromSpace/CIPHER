# V1 Assistant Loop

Builds on PR #34 (`filesystem.read`). CIPHER stays the authority: the model only proposes `TOOL_CALL:` lines.

## What changed
- `kMaxModelRounds` 3 -> 6; at most 5 tool calls per question.
- Model-offered tools: `time.now`, `system.info`, `filesystem.read`, `filesystem.list`, `memory.search`.
- `filesystem.list [dir]`: relative directories only, same traversal/symlink/reparse checks as read, private CIPHER files hidden, max 100 entries.
- `memory.search <keyword>`: max 5 hits, 200-byte snippets, entries denied by the `memory_deny` policy are skipped. `memory.list` is still not offered.
- Every call still needs its own exact `yes`. There is no plan-wide approval.
- Repeat guard: an identical call that already ran is not re-confirmed or re-run.
- Recovery: a refused or failed read-only call is shown to the model as an observation, at most `kMaxToolFailures` (2) times, then the loop stops with `Denied: ...`. Policy block, owner "no", malformed, unoffered and non-local route stay terminal. After an earlier call in the same turn, these messages name the calls that ran; "Nothing was run." appears only when none ran.
- Observation vs verification: empty output is reported as "confirms nothing"; any answer after tool use gets a footer listing the calls CIPHER ran and stating the answer is the model's, not verified.

## Not done (honest)
- No code was adapted from OpenClaw/OVOS/Leon/Dodaios; the existing `integrations/` bridges were not changed. Decision for now: REJECT as runtime dependency, keep as research references.
- No end-to-end run against a real Ollama/llama.cpp model; tests use scripted backends.
