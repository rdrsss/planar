# planar_engine_workflows — engine/workflows bucket (plan 996, task 6096).
#
# Layer-2 `engine_*` module (cmake/architecture.cmake). It DEPENDS on
# NOTHING: workflow discovery is pure filesystem work and touches no SQLite
# handle at all (the Zig handlers say so explicitly -- "Read-only; no SQLite
# handle"). A `DEPENDS db` here would be an edge that exists only to look
# conventional, and architecture.cmake walks real edges.
#
# Behavior-preserving port (D2) of
# zig/src/cmd/planar/handlers/workflow/{dirs,meta,scan,list,show}.zig -- TWO
# of the three `workflow` schema leaves (`workflow list`, `workflow show`).
#
# NOT ported, named explicitly rather than silently dropped:
#
#   - `workflow run`, the third leaf. The boundary was established
#     EMPIRICALLY, not assumed: with `planar-execute` on PATH,
#     `workflow run finalize-closeout --phase closeout` exits 0 printing `{}`,
#     and `workflow run mmm-sandbox --phase build` exits 1 with
#     `planar-execute: phase function not found: build` on stderr -- output
#     from planar-execute itself. So `run` = this bucket's `catalog::find`
#     (ported) followed by a PROCESS SPAWN of a separate binary (not ported).
#     There is no process-spawn seam anywhere in this tree; building one
#     inside a port task would be a much larger change than the leaf it
#     serves, and it is the same call `bench harvest` (cycle 5) and
#     `capture commits` (cycle 4) made. Deferred WITH its dependency.
#
#     Its PRE-SPAWN refusals are reachable and ARE covered here:
#     `workflow run nope --phase x` fails `error: workflow 'nope' not found`
#     (exit 1) before any spawn -- the same text `workflow show` emits, which
#     is why `render::not_found_error` is one shared function pinned against
#     both leaves' captures.
#
#   - `policy.audit.record` calls. There is still no `policy.audit` module in
#     the C++ tree; engine_planning, engine_identity, engine_promotion,
#     engine_runtime, engine_runs and engine_models all document the same
#     omission. No `workflow` leaf reads or emits audit rows.
#
# HOME SAFETY. `catalog::resolve_dirs` takes an explicit env-lookup callable
# instead of reading `std::getenv`, so this module has no way to find the
# developer's real `~/.planar` on its own. That is a structural property, not
# a test convention -- see catalog.cppm's header.
#
# `workflows.cppm` is the umbrella re-export.
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

planar_module(engine_workflows
  INTERFACE
    catalog.cppm
    render.cppm
    workflows.cppm
  SOURCES
    catalog.cpp
    render.cpp
  DEPENDS
    json_text)
