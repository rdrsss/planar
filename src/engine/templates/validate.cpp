/// @file validate.cpp
/// @brief Implementation of `planar.engine.templates.validate` (plan 996,
/// task 6190). See validate.cppm for why the duplicate `(smoke-render)`
/// issue is preserved.

module planar.engine.templates.validate;

import std;
import planar.engine.templates.context;
import planar.json_dom;
import planar.engine.templates.render;

namespace planar::engine::templates {

using json_dom::json_kind;
using json_dom::json_value;

namespace {

/// @brief Walk `v`, rendering every string and recording failures.
/// @param v The subtree.
/// @param path The dotted path to `v`.
/// @param ctx The stub context.
/// @param issues Accumulator.
auto collect_issues(const json_value& v, std::string_view path, const render_context& ctx, std::vector<validation_issue>& issues)
    -> void {
  switch (v.kind) {
  case json_kind::string: {
    auto const rendered = exec_string(v.string, ctx);
    if (!rendered.has_value()) {
      issues.push_back({.json_path = std::string{path}, .message = std::string{error_name(rendered.error())}});
    }
    return;
  }
  case json_kind::object:
    for (auto const& [key, value] : v.object) {
      // A top-level member's path is its BARE key — no leading dot. Only
      // nested members get the `parent.child` join.
      auto const child_path = path.empty() ? std::string{key} : std::format("{}.{}", path, key);
      collect_issues(value, child_path, ctx, issues);
    }
    return;
  case json_kind::array:
    for (std::size_t i = 0; i < v.array.size(); ++i) {
      collect_issues(v.array[i], std::format("{}[{}]", path, i), ctx, issues);
    }
    return;
  default:
    // Numbers, booleans and nulls carry no directives.
    return;
  }
}

} // namespace

auto validate_template(const json_value& fields) -> std::vector<validation_issue> {
  std::vector<validation_issue> issues;
  auto const                    ctx = stub_context();

  collect_issues(fields, "", ctx, issues);

  // Second pass. It overlaps the first on today's renderer and is kept
  // anyway — see validate.cppm's header; the oracle's issue COUNT depends
  // on it.
  if (auto const rendered = render_template(fields, ctx); !rendered.has_value()) {
    issues.push_back(
        {.json_path = "(smoke-render)", .message = std::format("smoke render failed: {}", error_name(rendered.error()))});
  }
  return issues;
}

} // namespace planar::engine::templates
