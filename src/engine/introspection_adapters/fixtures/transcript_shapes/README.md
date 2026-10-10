# Transcript record shapes (structure only)

Provenance: structure captured 2026-10-08 (plan 1132, task 7366) from the newest
few local transcript files of each vendor, read-only, with a script that emitted
only key names, nesting, JSON value types and fixed enum-like discriminator
tokens. No raw record, message text, command, path or identifier was copied.
Every value in these files is a synthetic placeholder (`"<string>"`, `0`,
`false`, a made-up `planar task show 0`, a fixed timestamp). Keys, nesting and
types are the observed ones.

Vendor versions seen: Claude Code 2.1.289 and 2.1.292 (`version` on each
record), Codex CLI 0.161.0 (`payload.cli_version` on `session_meta`), Copilot
CLI: no version field in `workspace.yaml`.

| File | Shapes |
|------|--------|
| `claude_current.jsonl` | assistant `tool_use` (Bash, bare and `cd … && planar …` commands), user `tool_result` (`is_error` true, string and array content), user text, thinking/text, `system`/`turn_duration`, `attachment`, `queue-operation`, `ai-title` |
| `codex_current.jsonl` | `session_meta`, `turn_context`, `event_msg` (`task_started`, `item_completed` `CommandExecution` failed and completed, `task_complete`), `response_item` (`function_call`, `function_call_output`, `custom_tool_call`, `custom_tool_call_output`) |
| `copilot_session_state/session-id/` | the files the installed Copilot CLI writes under `session-state/<id>/`: `workspace.yaml`, `rewind-file-snapshots/tracking.json`, `checkpoints/index.md`. No JSONL event file exists there. |

Tests that read them (`src/engine/introspection_adapters/introspection_adapters.t.cpp`):

- `claude_current.jsonl`: "the Claude fixture's bare and cd-prefixed planar failures both normalize".
- `codex_current.jsonl`: "the Codex fixture parses clean and its current records are ignored without malformed lines".
- `copilot_session_state/`: "a Copilot session-state directory with no JSONL is unsupported, not malformed".
