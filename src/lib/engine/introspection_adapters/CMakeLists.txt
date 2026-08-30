# planar_engine_introspection_adapters — engine/introspection_adapters
# bucket (plan 996, task 6102).
#
# Layer-2 `engine_*` module with exactly one dependency, `json_dom`
# (layer 1) — for parsing each JSONL line the way `std.json.parseFromSlice`
# does. No `db` dependency: this bucket ports only the IN-MEMORY collector
# half of zig/src/engine/introspection_adapters.zig (`collectPreview` and
# its extraction/dedup/sort pipeline, plus `discover`); the disk-discovery
# half (`collectPreviewFromPaths`, the `FsSeam`, `TranscriptConfig`,
# `CliLogAdapter`) is deferred alongside `engine/introspect.zig` and the
# `report` handler wiring — see introspection_adapters.cppm's header.

planar_module(engine_introspection_adapters
  INTERFACE introspection_adapters.cppm
  SOURCES introspection_adapters.cpp
  DEPENDS
    json_dom)
