# planar_engine_introspect — engine/introspect bucket (plan 996, tasks
# 6121 and 6352).
#
# Layer-2 `engine_*` module. Three dependencies: `db` (every query in this
# bucket is a redacted SQL aggregate), `json_text` (`append_json_string`
# for the `--json` renderer), and `introspection_preview` (layer 1, task
# 6352, decision 981) — the shared `preview` type `bundle` now carries.
#
# Still DELIBERATELY DOES NOT depend on `engine_introspection_adapters`,
# unlike the oracle's introspect.zig (which imports
# introspection_adapters.zig directly for its `Preview` type).
# `cmake/architecture.cmake`'s D15 forbids an `engine_* -> engine_*` edge —
# configuring this module WITH that DEPENDS entry FAILS with exactly that
# diagnostic (`engine_introspect (layer 2) depends on
# engine_introspection_adapters (layer 2), which is not strictly
# downward`), verified experimentally by a reviewer on task 6121. Decision
# 981 is the resolution: `preview` moved to the new layer-1
# `introspection_preview` module both this bucket and
# `engine_introspection_adapters` depend on downward, so `bundle` carries
# a real `preview` field again without ever naming the other engine
# bucket. See introspect.cppm's header for the full account.
#
# Ports the DB-aggregate half of zig/src/engine/introspect.zig in full:
# `Bundle` (including `preview`, as of task 6352), `build`,
# `cliPreviewJsonl`, `renderText`, `renderJson`. `build` itself still never
# populates `preview` — that is the `report` handler's job, calling
# `engine_introspection_adapters::collect_preview_from_paths` after `build`
# returns, mirroring the oracle's own two-step assembly in
# `handlers/report.zig`.

planar_module(engine_introspect
  INTERFACE introspect.cppm
  SOURCES introspect.cpp
  DEPENDS
    core
    db
    json_text
    introspection_preview)
