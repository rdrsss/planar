/// @file context.cpp
/// @brief Implementation of `planar.engine.templates.context` (plan 996,
/// task 6190). See context.cppm for why every field is a value rather than
/// an optional, and why `children` is empty in the stub.

module planar.engine.templates.context;

import std;

namespace planar::engine::templates {

auto stub_context() -> render_context {
  // Transcribed field-for-field from zig's `stubContext`. The
  // scope_kind/scope_id fields are left at their zero values there too, so
  // `{{if .Task.ScopeKind}}` is false during validation — same reachability
  // caveat as `children`, and same reason for not "improving" it.
  return render_context{
      .feature      = {.id = 1, .slug = "stub-feature", .title = "stub-feature", .body = "stub body", .status = "drafting"},
      .plan         = {.id = 1, .slug = "stub-plan", .title = "stub-plan", .body = "stub body", .status = "drafting"},
      .task         = {.id = 1, .title = "stub-task", .body = "stub body", .status = "todo"},
      .scenario     = {.id = 1, .title = "stub-scenario", .body = "stub body"},
      .touches      = {"stub/repo"},
      .assoc        = {.slug = "stub-assoc", .name = "Stub Assoc"},
      .external_key = "STUB-1",
      .children     = {},
  };
}

} // namespace planar::engine::templates
