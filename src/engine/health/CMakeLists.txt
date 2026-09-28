# planar_engine_health — engine/health bucket (plan 996, tasks 6090 and
# 6357).
#
# Layer-2 `engine_*` module; three layer-1 dependencies (`db`, `scope_ref`,
# `installed_surface`) and no sideways ones. As with `engine_search`, the
# scope-slug resolution the Zig original takes from
# `identity.scope.resolveSlug` comes from layer-1 `planar.scope_ref` here —
# D19's extraction (task 6089) is what keeps this bucket free of the
# `engine_* -> engine_*` edge D15/D18 forbid.
#
# Task 6090 landed only the `hygiene` half of zig/src/engine/health.zig. The
# `check` / `withProjectionFreshness` half behind the bare `planar health`
# verb was deferred WITH its real blocker: that leaf's handler folds
# `engine.installedsurface.status` into every run, and
# zig/src/engine/installedsurface.zig (548 lines of manifest-driven
# filesystem classification) had no counterpart in this tree.
#
# Task 6357 closed that gap by porting the classifier straight to LAYER 1 —
# `planar.installed_surface` (src/lib/installed_surface) — rather than
# adding a same-layer `engine_health -> engine_<classifier>` edge, the same
# decision-981 shape task 6352 used for `report`'s two engine buckets. The
# classifier holds no `db` edge (manifest JSON + a filesystem walk only), so
# there was nothing pulling it toward `engine_health` in the first place.
# `check` still returns a permanently-stubbed `projection_freshness` on its
# own — that mirrors the oracle's `check`, which never touches
# `installedsurface` either — and `with_projection_freshness` is the
# separate fold-in step the `planar health` CLI handler (layer 3) composes.
# See health.cppm's header.

planar_module(engine_health
  INTERFACE health.cppm
  SOURCES health.cpp
  DEPENDS
    db
    scope_ref
    installed_surface)
