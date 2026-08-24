# planar_adapter — the external-plane adapter boundary (plan 996, task 6041).
#
# Layer-1 base library (cmake/architecture.cmake) that depends on NOTHING —
# not even planar_http. An adapter needs a transport; the boundary VOCABULARY
# does not, and keeping the dependency out is what lets
# `planar.engine.external.sync` be unit-tested against a stub adapter with no
# HTTP anywhere in the picture.
#
# It is layer 1 for the reason `scope_ref` and `json_text` are (D19): two
# layer-2 buckets need it — `engine_extsync` implements adapters,
# `engine_external` drives one — and D18 forbids the edge between them.
#
# `adapter_error_name` is the one behavior here with real content, and it is a
# PARITY SURFACE: those PascalCase strings are Zig error tags that reach the
# operator through `sync_events.detail` and `audit trail --link --json`. See
# adapter.cppm's header.
#
# STDOUT TERMINATOR CONTRACT: not applicable — this module renders nothing.

planar_module(adapter
  INTERFACE adapter.cppm
  SOURCES adapter.cpp)
