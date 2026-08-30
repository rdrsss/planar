# planar_engine_introspect — engine/introspect bucket (plan 996, task 6121).
#
# Layer-2 `engine_*` module. Two dependencies: `db` (every query in this
# bucket is a redacted SQL aggregate) and `json_text` (`append_json_string`
# for the `--json` renderer).
#
# Deliberately DOES NOT depend on `engine_introspection_adapters`, unlike
# the oracle's introspect.zig (which imports introspection_adapters.zig
# directly for its `Preview` type). `cmake/architecture.cmake`'s D15
# forbids an `engine_* -> engine_*` edge — configuring this module WITH
# that DEPENDS entry FAILS with exactly that diagnostic
# (`engine_introspect (layer 2) depends on engine_introspection_adapters
# (layer 2), which is not strictly downward`). `bundle` therefore carries
# no `preview` field and `render_text`/`render_json` unconditionally emit
# the "unavailable" / empty-arrays branch. See introspect.cppm's header for
# the two ways forward (a layer-1 extraction, or splicing at the layer-3
# handler) — this is a real finding for whoever wires `report`, not
# something resolved here.
#
# Ports the DB-aggregate half of zig/src/engine/introspect.zig in full:
# `Bundle` (minus `preview`), `build`, `cliPreviewJsonl`, `renderText`,
# `renderJson`. Still unported: the discovery half of
# introspection_adapters.zig (`collectPreviewFromPaths`/`FsSeam`/
# `TranscriptConfig`/`CliLogAdapter`/`collectConfiguredPreview`) and the
# `report` handler itself; see task 6121's tracking row.

planar_module(engine_introspect
  INTERFACE introspect.cppm
  SOURCES introspect.cpp
  DEPENDS
    db
    json_text)
