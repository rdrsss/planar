# planar_engine_local — engine/local bucket (plan 996, task 6109).
#
# Layer-2 `engine_*` module (cmake/architecture.cmake).
#
# DEPENDS json_text ONLY. There is no `DEPENDS db`, and that is not an
# oversight: the operator-local sandbox is pure filesystem state. Nothing under
# `planar local *` reads or writes a single SQLite row — the sources live under
# `<home>/.planar/local/`, the install record lives in a per-kind
# `.link-manifest.json` beside them, and the vendor installs are symlinks. A
# `DEPENDS db` here would be an edge that exists only to look conventional, the
# same call `engine_workflows` made for the same reason.
#
# `json_text` (layer 1, D19) supplies the one JSON string-escaping primitive.
# Before it existed this bucket would have been the EIGHTH byte-identical copy
# of that function in the tree; see src/lib/json_text/CMakeLists.txt.
#
# Behavior-preserving port (D2) of zig/src/engine/local/{manifest,link,import}.zig
# plus zig/src/cmd/planar/handlers/local/{common,list,link,unlink,import,migrate}.zig
# — ALL FIVE `local` schema leaves (list, link, unlink, import, migrate),
# including `link --reconcile`, which is a distinct code path rather than a flag.
#
# ---------------------------------------------------------------------------
# HOME SAFETY. This bucket is the most dangerous one ported so far, because its
# whole job is writing symlinks into the operator's REAL vendor directories:
# `~/.claude/commands`, `~/.codex/skills`, `~/.copilot/skills`. A test that
# resolves the wrong root does not fail — it edits the developer's machine.
#
# The Zig original resolves that root as PLANAR_LOCAL_HOME, else HOME
# (handlers/local/common.zig:11). It NEVER consults PLANAR_HOME, unlike every
# other Planar surface, so redirecting only PLANAR_HOME — the obvious move —
# leaves `local link` writing into the real `~/.claude/commands`.
#
# The protection here is structural, not a test convention:
# `manifest::resolve_home_and_root` takes an explicit env-lookup callable and
# every other entry point takes explicit absolute paths. No function in this
# bucket calls `std::getenv`, so it has no way to find a real home at all. Same
# mechanism `engine_workflows` uses, applied to a surface where the stakes are
# an order of magnitude higher.
#
# Oracle-verified while establishing that: `CODEX_HOME` is IRRELEVANT to this
# bucket. With `CODEX_HOME` pointed elsewhere, the codex install still landed
# under `$PLANAR_LOCAL_HOME/.codex/skills`. All three vendor directories are
# derived from the one sandbox root and nothing else.
# ---------------------------------------------------------------------------
#
# NOT ported, named rather than silently dropped:
#
#   - `policy.audit.record` calls. There is still no `policy.audit` module in
#     the C++ tree; engine_planning, engine_identity, engine_promotion,
#     engine_runtime, engine_runs, engine_models and engine_workflows all
#     document the same omission (task 6100, pending an operator decision). No
#     `local` leaf reads or emits audit rows.
#
#   - The handlers' `exit.die` paths themselves. This bucket returns typed
#     failures; mapping a failure onto an exit code and a stderr line is the
#     cmd layer's job, and there is no `cmd_*` layer yet. The MESSAGES are here
#     (see render.cppm) so that when the cmd layer lands it is a wiring change
#     with nothing left to re-derive.
#
# STDOUT TERMINATOR CONTRACT (task 6114, resolving the M4 boundary review's
# F1). A renderer in this tree that returns a COMPLETE stdout payload returns
# the oracle's exact bytes INCLUDING the trailing newline; the layer-3 caller
# writes the string verbatim and appends nothing. A renderer that returns a
# FRAGMENT the caller composes into a larger line -- an error-message body
# that becomes `error: <body>` on stderr -- returns no terminator and its
# @return says so. The alternative shape ("renderer returns the body, caller
# appends the terminator") cannot express two payloads this tree actually has:
# a leaf whose empty output is ZERO BYTES rather than a bare newline
# (`workflow list --json`, `local list --json`), and NDJSON, where the newline
# is per-RECORD rather than per-document. Both are pinned. See
# `json_text/CMakeLists.txt` for the escaping half of the same contract.

planar_module(engine_local
  INTERFACE
    manifest.cppm
    link.cppm
    importer.cppm
    render.cppm
    local.cppm
  SOURCES
    manifest.cpp
    link.cpp
    importer.cpp
    render.cpp
  DEPENDS json_text)

# Glaze (D8) reads the on-disk `.link-manifest.json`. PRIVATE: the exported
# interface speaks in this bucket's own structs, never in Glaze types, so no
# consumer needs Glaze's headers. The WRITER is hand-rolled instead — the
# target is not "valid JSON" but the exact bytes zig's std.json.Stringify with
# indent_2 produces, which no general-purpose serializer reproduces by accident.
target_link_libraries(planar_engine_local PRIVATE glaze::glaze)
if(TARGET planar_engine_local_tests)
  target_link_libraries(planar_engine_local_tests PRIVATE glaze::glaze)
endif()
