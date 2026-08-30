# planar_introspection_preview — layer-1 redacted-preview vocabulary
# (decision 981, plan 996 task 6352).
#
# Extracted out of `engine_introspection_adapters` (task 6102) so that BOTH
# `engine_introspection_adapters` AND `engine_introspect` can depend on it
# downward — D15/D18's same-layer prohibition forbids the direct
# `engine_introspect -> engine_introspection_adapters` edge the oracle's own
# `Bundle.preview: ?adapters.Preview` shape would otherwise require (FATALs
# at configure time, verified experimentally on task 6121). Same shape as
# `scope_ref` (D19), `json_text`, `adapter`, `http`, `process`, `policy`,
# `cliapp`. See introspection_preview.cppm's header for the full account.
#
# Pure data, no behavior: `vendor`/`category`/`coverage_state`/
# `warning_kind` and the `warning_row`/`signal_row`/`coverage_row`/`preview`
# structs it composes. No SOURCES — nothing here has a definition to put in
# one (same shape as `core`'s INTERFACE-only version.cppm). DEPENDS nothing.

planar_module(introspection_preview
  INTERFACE introspection_preview.cppm)
