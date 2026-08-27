# planar_json_dom — the tree's ONE JSON DOM (plan 996, task 6190).
#
# Layer-1 base library (cmake/architecture.cmake), beside `json_text`.
#
# An insertion-ordered JSON document model whose parser and writer both
# reproduce `std.json`'s exact behaviour. See json_dom.cppm's header for
# the four `std.json` behaviours a plausible parser gets wrong — every one
# of them WAS wrong here until a differential run against the built oracle
# disagreed.
#
# ---------------------------------------------------------------------------
# WHY THIS IS LAYER 1 RATHER THAN PART OF engine/templates
#
# It was written inside `engine/templates`, where the only consumer was the
# `{{...}}` substituter. Then a differential run showed the RESOLUTION half
# of the same plane — `planar.engine.config.templates`, a DIFFERENT layer-2
# bucket — needed the same parser to stay faithful to the oracle:
#
#   `templates show probe/px/dup` on a template with a REPEATED KEY
#     oracle: exit 1, `error: template probe/px/dup not found`
#     before:  exit 0, rendered the last value
#
# The oracle refuses because `std.json`'s `duplicate_field_behavior`
# defaults to `.@"error"`, so the candidate fails to parse and falls
# through the whole resolution chain. `engine_config` was asking
# `glz::validate_json`, which ACCEPTS duplicate keys.
#
# Two layer-2 buckets cannot import each other (D15 FATALs on an
# engine→engine edge), so the choice was a second validity check inside
# `engine_config` or one shared DOM at layer 1. A second checker is exactly
# the duplication D19 exists to prevent — and it would have been a checker
# that had to agree with this parser on every edge case forever. So the DOM
# moved down a layer and `engine_config::is_valid_json` now routes through
# `parse_json`.
#
# The side benefit is that Glaze leaves this path entirely. Glaze remains
# right where the question is "is this valid JSON at all" for a document
# nobody re-emits; it was never right for "hand me back what I gave you",
# because `glz::json_t` is `std::map`-backed and re-sorts object keys.
# ---------------------------------------------------------------------------
#
# DEPENDS json_text for the string escaper. The two modules are the same
# escape table transcribed from the same zig function, and a first draft of
# json_dom.cpp carried its own byte-identical copy — which is the eighth
# copy D19 was written about. There is one.
#
# Layer-1 modules may depend on each other (D17, decision 943); the cycle
# check still applies and this edge is one-way.

planar_module(json_dom
  INTERFACE
    json_dom.cppm
  SOURCES
    json_dom.cpp
  DEPENDS json_text)
