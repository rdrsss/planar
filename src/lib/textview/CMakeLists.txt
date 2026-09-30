# planar.textview — layer-1 terminal-safe text primitives (plan 1080, task
# hq-view-escape-fields, 7082).
#
# Layer 1 by name (cmake/architecture.cmake classifies anything not `cmd_*` /
# `engine_*` as layer 1), same as `json_text`. DEPENDS nothing: pure functions
# over `std::string_view`.
#
# WHY IT EXISTS. `planar-watch queue`, `planar-watch queue history` and
# `planar-agent queue status` all print submitter-controlled text (vendor,
# role, label, directory, argv) on a terminal. D18 forbids a `cmd_* -> cmd_*`
# edge, so the escaping, truncation and display-width rules they must agree on
# could only be duplicated per binary or extracted down to layer 1. They are
# extracted here so the two binaries cannot drift apart.
planar_module(textview
  INTERFACE textview.cppm
  SOURCES textview.cpp)
