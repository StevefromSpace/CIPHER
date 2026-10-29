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

## Limits
- The final natural-language answer is the model's. CIPHER does not check its content; the footer says so.
- The verification gate checks only that a file named in the question was read (whole-word, ASCII case-insensitive). It does not check the answer against the file.
- Tool output above 4096 bytes is withheld, not truncated, so the model never sees a partial result.
- A directory listing shows at most 100 entries. Entries beyond that cap are not tracked for verification.
- Non-ASCII file names are not matched by the verification gate (ASCII-only). Such a question fails closed rather than passing.
- Names from a directory listing are not stripped of control characters before they enter the prompt.
- A small local model (qwen2.5:3b in development runs) sometimes sends two `TOOL_CALL` lines or a malformed request. CIPHER uses only the first request in a reply.
- Early-exit messages after a tool ran name the calls that ran but do not add the footer.
- `memory.list` is not offered to the model. `memory.search` is offered only when `memory_deny` allows it.
- The llama.cpp tool path is covered by scripted tests and CI only. The Ollama path was run end to end with a real model during development.
- Issues #6, #7 and #8 stay open and are deferred past v1.0.0.

## Not done (honest)
- No code was adapted from OpenClaw/OVOS/Leon/Dodaios; the existing `integrations/` bridges were not changed. Decision for now: REJECT as runtime dependency, keep as research references. (Clean-room decision: DECISIONS.md ADR 13 and owner decision m00849.)
