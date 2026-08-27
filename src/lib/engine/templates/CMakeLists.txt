# planar_engine_templates — engine/templates bucket (plan 996, task 6190).
#
# Layer-2 `engine_*` module (cmake/architecture.cmake).
#
# The RENDERING half of the template plane. The RESOLUTION half — the
# three-level fallback chain and the disk/embedded enumerators — landed
# earlier as `planar.engine.config.templates` (task 6032) and stays there.
#
# This bucket does NOT depend on `engine_config`, and cannot: both are
# LAYER 2, and cmake/architecture.cmake's D15 walk FATALs at configure time
# on an engine→engine edge (it did, on the first revision of this file).
# Only layer 3 may compose two engine buckets, so `handlers/templates.cpp`
# is where the loader's output meets this bucket's renderers. The two seams
# that crossed the line are now parameters:
#
#   - `extract_defaults(root, span<embedded_file>)` takes the template set
#     instead of importing `templates_embed`;
#   - `merge_list_rows` speaks this bucket's own `list_row` instead of
#     `config::template_entry`.
#
# Both are better shapes than the imports they replaced — each one lets a
# test drive the function with three synthetic entries instead of the real
# embedded set. Recorded because "the layer check made me" is the kind of
# rationale that reads as arbitrary later.
#
# Behavior-preserving port (D2) of zig/src/engine/templates/{render,validate,
# context,builder,init}.zig plus the rendering half of
# zig/src/cmd/planar/handlers/templates/{list,show,render,validate,init,path}.zig
# — ALL SIX `templates` schema leaves.
#
# ---------------------------------------------------------------------------
# THE JSON DOM IS NOT HERE — IT IS `planar.json_dom`, AT LAYER 1
#
# `templates render` prints the template's own KEY ORDER (`issue.json` is
# authored `title, body, labels, assignees` and the oracle emits exactly
# that, because `std.json.Value`'s ObjectMap is insertion-ordered), and
# `glz::json_t` is `std::map`-backed and re-sorts. So the tree needs a DOM
# of its own, and this bucket is where it was first written.
#
# It MOVED to `src/lib/json_dom/` mid-cycle, because a differential run
# showed the RESOLUTION half of the same plane — `engine_config`, a
# different layer-2 bucket — needed the identical parser to stay faithful
# on duplicate keys. Read that bucket's CMakeLists for the capture. The
# short version: two layer-2 buckets cannot import each other, and the
# alternative was a second validity checker that would have to agree with
# this one on every edge case forever.
# ---------------------------------------------------------------------------
#
# ---------------------------------------------------------------------------
# FOUR ORACLE DEFECTS REPRODUCED RATHER THAN FIXED
#
# D2 makes this cycle a port. Each of these is live, operator-visible
# behaviour in the shipped `planar`; "fixing" one here would make the two
# trees disagree on a real surface while every parity lane stayed green.
# Each needs its own task against the ORACLE. Named here so the next reader
# does not mistake them for porting mistakes:
#
#   1. `{{if .X}}` where `.X` renders to MORE THAN 128 BYTES fails the whole
#      render with `error: rendering template: OutOfMemory`, exit 1. zig's
#      `evalTruthy` resolves into a 128-byte stack FixedBufferAllocator
#      purely to ask "is this non-empty?". Captured at exactly 128 (exit 0)
#      and 129 (exit 1) against the built oracle. Any task whose body
#      exceeds 128 bytes is unrenderable through a template that guards it.
#      Reproduced explicitly as `render::k_truthy_budget` — a C++
#      `std::string` has no ceiling, so nothing here would fail on its own
#      and an implementation that "just worked" would be the divergence.
#
#   2. `templates init --force` IGNORES `--force` and never overwrites. The
#      zig handler discards the flag (`_ = args.force;`).
#
#   3. `templates validate --json` interpolates set/system/kind into its
#      envelope RAW, not escaped, so an identifier containing a double
#      quote emits malformed JSON. The `issues` array in the same envelope
#      IS escaped. Both halves reproduced.
#
#   4. `{{else}}` is SILENTLY SWALLOWED when the `{{if}}` is falsy and
#      REFUSES when it is truthy. Captured:
#        `{{if .Nope}}a{{else}}b{{end}}`       -> `""`, exit 0 (b dropped)
#        `{{if .Task.Title}}a{{else}}b{{end}}` -> exit 1,
#           `error: rendering template: UnsupportedDirective`
#      `{{else}}` is not counted while scanning for the matching
#      `{{end}}`, so a falsy test skips past it and never dispatches it.
#      The first half is the dangerous one — it is the exact
#      silently-wrong-payload shape the renderer's "fail loudly" posture
#      is supposed to prevent. This one was found by a test asserting the
#      PLAUSIBLE behaviour ("else always refuses") and being wrong.
#
# A fourth asymmetry is not a defect but is just as easy to "correct" by
# accident: `build_scenario_context` never queries the touches edges, so
# `{{range .Touches}}` renders nothing for a scenario even when its plan has
# repo edges. Its task and plan siblings do query them.
# ---------------------------------------------------------------------------
#
# DEPENDS db for `builder` alone — the only module here that opens a
# statement. Everything else is text and filesystem. `json_dom` supplies the
# document model; `json_text` (layer 1, D19) supplies the one JSON
# string-escaping primitive, which the leaf renderers in `output.cpp` compose
# by hand. The oracle has one escaper too: `output.writeJsonString` is a
# one-line forward to `std.json.Stringify.encodeJsonString`.
#
# NOT ported, named rather than silently dropped:
#
#   - `policy.audit.record` calls. There are none to wire: the whole family
#     is READ-ONLY (`templates render` is a documented dry run) plus one
#     filesystem write (`templates init`), and the zig originals behind all
#     six leaves contain zero `policy.audit.record` call sites. Verified,
#     not assumed: `grep -rn audit zig/src/engine/templates/
#     zig/src/cmd/planar/handlers/templates/` has EMPTY output, and
#     `grep -rln audit.record zig/src/engine` names nineteen files, none of
#     them under `templates/`.
#
#   - `Context.children` population. The renderer supports
#     `{{range .Children}}` because the same renderer serves `ext propagate`
#     (a separate, unported family). None of the three builders fills the
#     field, on either tree, so the directive renders to nothing through
#     `templates render` today.

planar_module(engine_templates
  INTERFACE
    context.cppm
    render.cppm
    validate.cppm
    extract.cppm
    builder.cppm
    output.cppm
    templates.cppm
  SOURCES
    context.cpp
    render.cpp
    validate.cpp
    extract.cpp
    builder.cpp
    output.cpp
  DEPENDS db json_text json_dom)
