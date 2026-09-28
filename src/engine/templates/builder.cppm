/// @file builder.cppm
/// @brief `planar.engine.templates.builder` — assemble a `render_context`
/// from the database (plan 996, task 6190).
///
/// Behavior-preserving port (D2) of
/// `zig/src/engine/templates/builder.zig`. This is the ONLY module in the
/// bucket that touches SQLite; everything else is pure text and
/// filesystem, which is why `DEPENDS db` sits on this bucket for this file
/// alone.
///
/// ## Reads only. Always.
///
/// `templates render` is documented as a dry run — no writes, no external
/// calls — and every statement below is a `select`. There is no
/// `policy::audit::record` call either, matching the oracle: rendering a
/// template is not an auditable mutation because it mutates nothing.
///
/// ## The anchor walk, and the fallback that is easy to miss
///
/// `{{.Feature.*}}` is the ANCHOR plan — the root of the feature tree —
/// and finding it from an arbitrary entity is a two-stage walk:
///
///   1. Follow a `derives-from` edge in `entity_links` to some plan
///      (lowest `id` wins when there are several).
///   2. From there, climb `parent_plan_id` until it is NULL.
///
/// Stage 1 has a fallback that only fires when the edge is ABSENT, and it
/// differs per kind:
///
///   - a **plan** starts the climb from itself;
///   - a **task** falls back to its `tasks.plan_id` FOREIGN KEY, because
///     tasks are attached to plans by column rather than by edge;
///   - a **test_scenario** has neither, so it FAILS with
///     `anchor_plan_not_found`.
///
/// That third case is a real refusal an operator can hit: `templates render
/// … --entity scenario:<id>` on a scenario with no `derives-from` edge
/// exits non-zero rather than rendering against an empty feature.
///
/// ## `plans` has no `body` column
///
/// `{{.Plan.Body}}` and `{{.Feature.Body}}` read `plans.summary`. Reaching
/// for a `plans.body` column would not fail loudly — it would fail at
/// prepare time and take the whole leaf down — but the subtler trap is
/// rendering the empty string from a column that does not mean what the
/// template says. Named here because the field name and the column name
/// genuinely disagree.
///
/// ## `children` is never populated
///
/// The oracle's three builders leave `Context.children` empty; only the
/// `ext propagate` path (unported, a separate family) fills it. So
/// `{{range .Children}}` renders to NOTHING through `templates render`
/// today, on both trees. The renderer supports the directive because the
/// same renderer serves propagation; this builder simply has nothing to
/// put there. Not a gap in this port — a gap in the oracle's `templates
/// render`, reproduced.

module;

export module planar.engine.templates.builder;

import std;
import planar.db;
import planar.engine.templates.context;

namespace planar::engine::templates {

/// @brief Why building a context failed.
export enum class builder_error : std::uint8_t {
  not_found,             ///< The named entity does not exist.
  anchor_plan_not_found, ///< No anchor plan is reachable from the entity.
  query_failed,          ///< A statement failed to prepare or step.
};

/// @brief Build the context for `--entity task:<id>`.
///
/// `Plan` and `Feature` are both set to the ANCHOR plan — the immediate
/// plan is NOT exposed separately for a task, which is the oracle's shape
/// and means `{{.Plan.Title}}` on a task template names the feature, not
/// the task's own milestone plan.
/// @param conn An open connection.
/// @param task_id The task's row id.
/// @return The context, or the failure.
export auto build_task_context(db::connection& conn, std::int64_t task_id) -> std::expected<render_context, builder_error>;

/// @brief Build the context for `--entity plan:<id>`.
///
/// Here `Plan` and `Feature` DO differ: `Plan` is the requested row and
/// `Feature` is the root of its tree. A top-level plan is its own anchor,
/// and that case short-circuits the `derives-from` lookup entirely.
/// @param conn An open connection.
/// @param plan_id The plan's row id.
/// @return The context, or the failure.
export auto build_plan_context(db::connection& conn, std::int64_t plan_id) -> std::expected<render_context, builder_error>;

/// @brief Build the context for `--entity scenario:<id>` (also spelled
/// `test_scenario:<id>`).
///
/// `Touches` is left EMPTY — the oracle's scenario builder does not query
/// the touches edges at all, unlike its task and plan siblings. So
/// `{{range .Touches}}` renders nothing for a scenario even when the
/// scenario's plan has repo edges. Asymmetry reproduced, not corrected.
/// @param conn An open connection.
/// @param scenario_id The scenario's row id.
/// @return The context, or the failure.
export auto build_scenario_context(db::connection& conn, std::int64_t scenario_id)
    -> std::expected<render_context, builder_error>;

} // namespace planar::engine::templates
