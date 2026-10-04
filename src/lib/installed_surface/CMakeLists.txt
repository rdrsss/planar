# planar_installed_surface — layer-1 manifest-owned installed-vendor-
# projection classifier (plan 996, task 6357).
#
# Layer 1 by name (cmake/architecture.cmake classifies anything not `cmd_*` /
# `engine_*` as layer 1), same as `scope_ref` / `json_dom` / `process` /
# `introspection_preview`.
#
# FORCED to layer 1, not chosen: its one consumer is `engine_health` (layer
# 2), and this classifier has no `db` edge of its own — it is manifest JSON
# plus a filesystem walk — so a direct `engine_health -> engine_<this>` edge
# would be the `engine_* -> engine_*` shape D15/D18 FATAL on at configure
# time. Same correction shape as `scope_ref` (D19) and decision 981's
# `introspection_preview`: the leaf composing both halves (`planar health`'s
# handler, layer 3) is where the two buckets legally meet.
#
# DEPENDS json_dom for manifest parsing: the oracle's `std.json.
# parseFromSliceLeaky` rejects duplicate object keys and requires the
# document to be exhausted, and `planar.json_dom` is this tree's ONE parser
# with that exact behaviour (see json_dom.cppm's header) — Glaze's
# `glz::read_json` accepts both. No `std::filesystem` dependency is declared
# because it is a standard library facility, not a Planar module.

planar_module(installed_surface
  INTERFACE installed_surface.cppm
  SOURCES installed_surface.cpp
  DEPENDS json_dom)

if(TARGET planar_installed_surface_tests)
  # The OpenCode derivation parity test runs install.sh's awk over the
  # checkout's agents/ directory.
  target_compile_definitions(planar_installed_surface_tests PRIVATE
    "PLANAR_TARGET_SOURCE_ROOT=\"${CMAKE_SOURCE_DIR}\"")
endif()
