/// @file templates_embed.cppm
/// @brief `planar.engine.config.templates_embed` — the compile-time
/// embedded propagation-template set (tech-spec § "Key mechanisms" /
/// "Embedded migrations and templates", D5). This interface unit is
/// hand-authored and stable; the implementation unit that actually defines
/// `embedded_templates()` is generated at configure time by
/// `cmake/generate_templates.cmake` into the build tree (never the source
/// tree) and `#embed`s every `templates/defaults/<system>/<kind>.json`
/// file, enumerated in explicit ascending `(system, kind)` order — never
/// `file(GLOB)`'s underlying directory order. Same public shape as the Zig
/// codegen it replaces (`zig/tools/gen_templates.zig`): an ordered,
/// immutable view over every embedded template's system, kind, and raw
/// JSON body.

module;

export module planar.engine.config.templates_embed;

import std;

namespace planar::engine::config {

/// @brief One embedded template file: an external-system slug, a kind
/// within that system, and its raw JSON body, embedded at compile time via
/// `#embed`.
export struct template_file {
  std::string_view system; ///< External system slug (e.g. "github-issues", "jira").
  std::string_view kind;   ///< Kind within the system (e.g. "issue", "epic").
  std::string_view body;   ///< Verbatim JSON content, embedded at compile time.
};

/// @brief The full embedded template set, in ascending `(system, kind)`
/// order — generated at configure time from
/// `templates/defaults/<system>/<kind>.json`, explicitly sorted.
/// @return An ordered, immutable view over every embedded template.
export auto embedded_templates() -> std::span<template_file const>;

} // namespace planar::engine::config
