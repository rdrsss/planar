# planar_engine_introspection_adapters — engine/introspection_adapters
# bucket (plan 996, tasks 6102 and 6352).
#
# Layer-2 `engine_*` module. Two dependencies: `json_dom` (layer 1) — for
# parsing each JSONL line the way `std.json.parseFromSlice` does — and
# `introspection_preview` (layer 1, task 6352, decision 981) — the shared
# `vendor`/`category`/`coverage_state`/`warning_kind`/`preview` vocabulary
# this bucket re-exports under its own names so its task-6102 callers keep
# compiling unchanged. No `db` dependency: this bucket touches no table —
# filesystem only.
#
# Task 6102 landed the IN-MEMORY collector half
# (`collectPreview`/`collect_preview`, its extraction/dedup/sort pipeline,
# and `discover`). Task 6352 landed the disk-discovery half
# (`collectPreviewFromPaths`/`collect_preview_from_paths`,
# `collectVendorPath`/`collect_vendor_path`, the fault-injection seam,
# `TranscriptConfig`/`transcript_config`, `CliLogAdapter`/`cli_log_adapter`)
# and, per decision 981, the layer-1 extraction that lets
# `engine_introspect`'s `bundle` carry a `preview` field again — see
# introspection_adapters.cppm's header for both accounts.

planar_module(engine_introspection_adapters
  INTERFACE introspection_adapters.cppm
  SOURCES introspection_adapters.cpp
  DEPENDS
    core
    json_dom
    introspection_preview)
