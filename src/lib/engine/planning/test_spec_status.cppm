/// @file test_spec_status.cppm
/// @brief `planar.engine.planning.test_spec_status` — the compute and render
/// halves of the `planar test-spec status` leaf (plan 996, task 6095).
///
/// Behavior-preserving port (D2) of
/// zig/src/engine/planning/test_spec_status.zig plus the anchor-resolution
/// and rendering halves of zig/src/cmd/planar/handlers/test_spec/status.zig.
///
/// The verb is the human-facing complement to `spec ingest --strict`'s
/// coverage gate: ingest checks the parsed diff, this walks the LIVE database
/// and surfaces per-milestone coverage after ingest has run. It is read-only.
///
/// ## What a "milestone" is here
///
/// For an anchor plan `A`, the milestone set is `A` itself plus every plan `P`
/// carrying a `plan -> plan derives-from` edge to `A`. Note this is the
/// entity_links edge, NOT `plans.parent_plan_id` — a child plan without the
/// edge does not appear, and a plan with the edge does even if its
/// `parent_plan_id` points elsewhere. Verified: the fixture's three
/// milestones each carry an explicit `derives-from` row.
///
/// ## ORACLE PROVENANCE
///
/// Every count, ordering, and byte below was captured by RUNNING the Zig
/// binary against a seeded scratch database. test_spec_status.t.cpp's header
/// carries the fixture SQL and the verbatim captures. Two findings worth
/// naming here because they are easy to get wrong:
///
/// 1. **Bucket counts are per-SCENARIO, not per-task.** A task verified by
///    five scenarios contributes five to the bucket totals but only one to
///    `tasks_covered`. The fixture's M1 milestone reports
///    `tasks_covered:1` alongside `happy:1,empty:2,error:1,edge:0,other:1`
///    — six scenario-hits against a single covered task.
///
/// 2. **The text table prints an explicit `+` sign on every count.** Not a
///    typo and not a diff marker: the oracle emits `+1`, `+0`, `+12345`,
///    right-aligned in a five-wide field that OVERFLOWS rather than
///    truncating. Captured directly:
///
///      '  M3                               +12345 +12345    +0      +0 ...'
///
///    A plain right-aligned integer would be wrong by a character on every
///    row.
///
/// ## Cut list
///
/// - `policy.audit.record` — nothing to cut. The module exists as of task
///   6100 and the rest of this bucket writes through it, but this leaf is
///   read-only and the Zig original has no call site.

module;

export module planar.engine.planning.test_spec_status;

import std;
import planar.db;

namespace planar::engine::planning::test_spec_status {

/// @brief The five return-path buckets a scenario title classifies into.
export enum class bucket {
  happy,      ///< Title starts with `happy path`.
  empty,      ///< Title starts with `empty / null`, `empty`, or `null`.
  error_case, ///< Title starts with `error`. Named `error_case` because
              ///< `error` is the field name on the wire but an awkward
              ///< enumerator here.
  edge,       ///< Title starts with `edge`.
  other       ///< Anything else.
};

/// @brief One per-milestone row of the coverage report.
///
/// Field order matches the Zig struct, which in turn matches the Go original,
/// so all three emit compatible NDJSON.
export struct milestone_status {
  std::int64_t plan_id = 0;         ///< The milestone plan's id.
  std::string  title;               ///< The milestone plan's title, untruncated.
  std::int64_t total_tasks     = 0; ///< Tasks with `plan_id` = this milestone.
  std::int64_t tasks_with_slug = 0; ///< ...of those, how many carry a non-empty slug.
  std::int64_t tasks_covered   = 0; ///< ...of those, how many some scenario `verifies`.
  std::int64_t happy           = 0; ///< Scenario-hits, NOT task counts.
  std::int64_t empty           = 0; ///< Scenario-hits.
  std::int64_t error_count     = 0; ///< Scenario-hits. Serializes as `"error"`.
  std::int64_t edge            = 0; ///< Scenario-hits.
  std::int64_t other           = 0; ///< Scenario-hits.
};

/// @brief The trailing roll-up across every milestone.
export struct plan_summary {
  std::int64_t anchor_plan_id  = 0; ///< The anchor the report was run for.
  std::int64_t total_tasks     = 0; ///< Sum across milestones.
  std::int64_t tasks_with_slug = 0; ///< Sum across milestones.
  std::int64_t tasks_covered   = 0; ///< Sum across milestones.
  /// Scenarios attached to the ANCHOR by a `derives-from` edge. This counts
  /// attached scenarios, not verifying ones — the fixture's detached seventh
  /// scenario is excluded, but an attached scenario that verifies nothing is
  /// still counted.
  std::int64_t total_scenarios = 0;
};

/// @brief The full report: per-milestone rows plus the roll-up.
export struct status {
  std::vector<milestone_status> milestones; ///< Ordered `(plan_id asc, title asc)`.
  plan_summary                  summary;    ///< The trailing roll-up.
};

/// @brief Failure surface for this module.
export enum class test_spec_error {
  not_found,   ///< No ANCHOR plan matched the argument.
  query_failed ///< Any SQLite failure.
};

/// @brief An anchor plan resolved from the leaf's positional argument.
export struct anchor {
  std::int64_t id = 0; ///< The resolved plan id.
  std::string  slug;   ///< Its slug, used in the text header.
};

/// @brief Classify a scenario title into one of the five buckets.
///
/// ASCII-case-insensitive prefix match after trimming spaces and tabs (NOT
/// newlines — the Zig original trims exactly `" \t"`). The `empty / null`
/// bucket is reached by three distinct prefixes.
/// @param title The scenario title.
/// @return The bucket it classifies into; `other` when nothing matches.
export auto classify_bucket(std::string_view title) -> bucket;

/// @brief Resolve the leaf's positional argument to an ANCHOR plan.
///
/// Tries a numeric id first, then a slug. BOTH lookups additionally require
/// `parent_plan_id is null`, which is the surprising half: a perfectly valid
/// milestone plan is reported as "not found" rather than as "not an anchor".
/// Oracle-captured — `test-spec status 2` and `test-spec status m1`, where
/// plan 2 / slug `m1` is a real milestone, both exit 1 with
/// `error: plan '2' not found` / `error: plan 'm1' not found`.
///
/// The slug branch is `order by p.id limit 1`, so the lowest-id anchor wins
/// if slugs ever collide across anchors.
/// @param conn An open, migrated database connection.
/// @param argument The raw positional (numeric id or plan slug).
/// @return The anchor, or `test_spec_error::not_found`.
export auto fetch_anchor(db::connection& conn, std::string_view argument) -> std::expected<anchor, test_spec_error>;

/// @brief Walk the anchor's milestones, scenarios, and `verifies` edges.
/// @param conn An open, migrated database connection.
/// @param anchor_plan_id The anchor plan resolved by `fetch_anchor`.
/// @return The report. An anchor with no milestones still yields one
/// milestone row (itself); an id matching no plan at all yields zero rows and
/// an all-zero summary rather than an error.
export auto compute(db::connection& conn, std::int64_t anchor_plan_id) -> std::expected<status, test_spec_error>;

/// @brief Render `test-spec status --json`.
///
/// NDJSON, not a single document: one object per milestone, each on its own
/// line, followed by the summary object on its own line. Every line including
/// the last is newline-terminated.
/// @param report The computed report.
/// @return The NDJSON block, WITH a trailing newline.
export auto render_json(const status& report) -> std::string;

/// @brief Render `test-spec status`'s text table.
///
/// Titles longer than 32 characters are truncated to 29 plus a literal
/// `...`; counts carry an explicit `+` sign right-aligned in a five-wide
/// field that overflows rather than truncating (see this module's header).
/// @param anchor_slug The anchor's slug, for the header line.
/// @param report The computed report.
/// @return The text block, WITH a trailing newline.
export auto render_text(std::string_view anchor_slug, const status& report) -> std::string;

/// @brief The `plan '<arg>' not found` message this leaf emits at exit 1.
/// @param argument The unresolved positional, echoed verbatim.
/// @return The message body (no `error: ` prefix, no trailing newline).
export auto render_not_found(std::string_view argument) -> std::string;

} // namespace planar::engine::planning::test_spec_status
