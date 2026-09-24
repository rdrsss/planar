/// @file context.cppm
/// @brief `planar.engine.templates.context` — the data a template renders
/// against (plan 996, task 6190).
///
/// Behavior-preserving port (D2) of `zig/src/engine/templates/context.zig`.
///
/// Every field defaults to empty/zero and NOT to an optional. That is
/// deliberate and it is the oracle's: a plan-level template rendered
/// against a plan context still resolves `{{.Task.Title}}`, and it resolves
/// it to the EMPTY STRING rather than failing. Go's `text/template` prints
/// a zero-valued struct field as empty, the Zig port kept that, and a
/// `std::optional` here would turn "field is not meaningful for this
/// entity" into a distinct third state the renderer would then have to
/// invent behaviour for.
///
/// The FIELD NAMES on the wire are Go's exported PascalCase spellings
/// (`{{.Task.Title}}`, `{{.Feature.ScopeID}}`) because templates authored
/// against the original Go binary must keep working. The C++ member names
/// are snake_case; the mapping between the two lives in exactly one place,
/// `render.cpp`'s `resolve_reference`, and nowhere else.

module;

export module planar.engine.templates.context;

import std;

namespace planar::engine::templates {

/// @brief The fields of a `plans` row the renderer can reach. Used for BOTH
/// the anchor feature plan (`{{.Feature.*}}`) and the immediate plan
/// (`{{.Plan.*}}`), which are frequently the same row.
export struct plan_info {
  std::int64_t id = 0; ///< `{{.X.ID}}`.
  std::string  slug;   ///< `{{.X.Slug}}`.
  std::string  title;  ///< `{{.X.Title}}`.
  /// `{{.X.Body}}` — sourced from the `plans.summary` COLUMN, not a
  /// `plans.body` column (there is none). Named `body` to match the
  /// template-visible spelling rather than the column.
  std::string  body;
  std::string  scope_kind;   ///< `{{.X.ScopeKind}}`.
  std::int64_t scope_id = 0; ///< `{{.X.ScopeID}}`.
  std::string  status;       ///< `{{.X.Status}}`.
};

/// @brief The fields of a `tasks` row the renderer can reach.
export struct task_info {
  std::int64_t id = 0;       ///< `{{.Task.ID}}`.
  std::string  title;        ///< `{{.Task.Title}}`.
  std::string  body;         ///< `{{.Task.Body}}`.
  std::string  status;       ///< `{{.Task.Status}}`.
  std::int64_t priority = 0; ///< `{{.Task.Priority}}`.
  std::string  scope_kind;   ///< `{{.Task.ScopeKind}}`.
  std::int64_t scope_id = 0; ///< `{{.Task.ScopeID}}`.
};

/// @brief The fields of a `test_scenarios` row the renderer can reach.
export struct scenario_info {
  std::int64_t id = 0; ///< `{{.Scenario.ID}}`.
  std::string  title;  ///< `{{.Scenario.Title}}`.
  std::string  body;   ///< `{{.Scenario.Body}}`.
};

/// @brief The association identity the renderer can reach.
export struct assoc_info {
  std::string slug; ///< `{{.Assoc.Slug}}`.
  std::string name; ///< `{{.Assoc.Name}}`.
};

/// @brief A child plan or task inside the feature tree, as seen from inside
/// `{{range .Children}}`.
export struct child_ref {
  std::string title;        ///< `{{.Title}}` (and `{{.}}`) inside the range body.
  std::string external_key; ///< `{{.ExternalKey}}` inside the range body — SHADOWS the top-level one.
};

/// @brief Everything one template execution can reach.
export struct render_context {
  plan_info                feature;      ///< `{{.Feature.*}}` — the anchor plan.
  plan_info                plan;         ///< `{{.Plan.*}}` — the immediate plan.
  task_info                task;         ///< `{{.Task.*}}`.
  scenario_info            scenario;     ///< `{{.Scenario.*}}`.
  std::vector<std::string> touches;      ///< `{{range .Touches}}` — repo slugs.
  assoc_info               assoc;        ///< `{{.Assoc.*}}`.
  std::string              external_key; ///< `{{.ExternalKey}}` at top level.
  std::vector<child_ref>   children;     ///< `{{range .Children}}`.
};

/// @brief A context whose every field carries a small fixed sentinel.
///
/// This is what `templates validate` smoke-renders against, so it must
/// populate every reachable field: a field left empty here would make
/// `{{if .Assoc.Slug}}`'s body unreachable and hide any directive error
/// inside it. Values transcribed from zig's `stubContext`.
///
/// `children` is deliberately EMPTY, matching the oracle. The consequence
/// is real and is not corrected here: a `{{range .Children}}` body is never
/// entered during validation, so directive errors inside one are invisible
/// to `templates validate`. Named rather than fixed because changing it
/// would change which templates validate clean.
/// @return The stub context.
export auto stub_context() -> render_context;

} // namespace planar::engine::templates
