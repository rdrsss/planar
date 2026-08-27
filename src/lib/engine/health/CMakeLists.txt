# planar_engine_health — engine/health bucket (plan 996, task 6090).
#
# Layer-2 `engine_*` module; two layer-1 dependencies (`db`, `scope_ref`)
# and no sideways ones. As with `engine_search`, the scope-slug resolution
# the Zig original takes from `identity.scope.resolveSlug` comes from
# layer-1 `planar.scope_ref` here — D19's extraction (task 6089) is what
# keeps this bucket free of the `engine_* -> engine_*` edge D15/D18 forbid.
#
# ONLY the `hygiene` half of zig/src/engine/health.zig is ported. The
# `check` / `withProjectionFreshness` half behind the bare `planar health`
# verb is deferred WITH its real blocker: that leaf's handler folds
# `engine.installedsurface.status` into every run, and
# zig/src/engine/installedsurface.zig (548 lines of manifest-driven
# filesystem classification) has no counterpart in this tree. A `check`
# ported without it would report a permanently-stubbed
# `projection_freshness` and would get the `overall` rollup — the field the
# leaf's exit-1-on-degraded contract reads — wrong on any machine with a
# managed install. See health.cppm's header.

planar_module(engine_health
  INTERFACE health.cppm
  SOURCES health.cpp
  DEPENDS
    db
    scope_ref)
