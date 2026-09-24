/// @file strategy.cppm
/// @brief `planar.engine.planning.strategy` — the parallelizability-rules
/// engine behind `plan recommend-strategy` AND the declared-vs-derived
/// overlap measure behind `plan divergence` (plan 996, task 6310).
///
/// Port target: `zig/src/engine/planning/strategy.zig` (1520 lines), plus the
/// two handlers `zig/src/cmd/planar/handlers/plan/recommend_strategy.zig` and
/// `.../divergence.zig`.
///
/// ## WHY BOTH LEAVES LAND IN ONE MODULE
///
/// Task 6298 measured it: the two verbs share roughly 300 lines of loader
/// substrate — `loadOpenTasks`, `loadTouches`, `loadClosureTouches`,
/// `ensurePlanExists`, and the `Touch`/`WorkTask` representation with its
/// conflict rule. `divergence` needs one entry point and `recommendWith`
/// another, but neither can be written without that substrate. Porting them
/// in separate cycles means either writing the loader twice or leaving one
/// leaf reaching into the other's internals, so they are ONE cycle.
///
/// The substrate is `namespace {}`-private in strategy.cpp; only the two
/// entry points and their renderers are exported. Nothing outside this module
/// gets to re-derive the six rules — the oracle's own header is emphatic that
/// they "live here, once", because the fan-out gate and the orchestrator skill
/// both consume `recommend-strategy`'s verdict.
///
/// ## THE TWO VERBS SHARE A LOADER AND AGREE ON ALMOST NOTHING ELSE
///
/// Every line below is oracle-captured (see strategy.t.cpp for the arena
/// recipe). This list exists because "they share a loader, so they must share
/// their filters/empty-cases/exit-codes" is exactly the assumption that would
/// produce a plausible, wrong port.
///
/// **`divergence` APPLIES ONLY RULE 2'S PAIRWISE OVERLAP TEST.** It runs no
/// unilateral rule at all — not 1, 3, 4, 5, or 6 — and does no drop-both
/// partitioning. Measured on a 7-task fixture where `recommend-strategy`
/// serialized five tasks (one per unilateral rule), `plan divergence` reported
/// `open_tasks:7 pairs:21 declared_overlaps:0 derived_overlaps:0 flips:0`. It
/// is a pure measurement of the signal D4 swaps, not a filtered view of the
/// recommendation.
///
/// **THE EMPTY-TOUCH SET MEANS OPPOSITE THINGS TO THE TWO VERBS.** Under
/// `recommend-strategy` a task with no declared touches is "touches
/// everything" and is dropped by rule 2. Under `divergence` an empty touch set
/// simply never conflicts, so it contributes NO overlap and cannot flip. Two
/// bare tasks in one plan: `recommend-strategy` serializes both with a rule-2
/// exclusion; `divergence` reports `pairs:1` and all-zero overlaps.
///
/// **`--closure-source` EXISTS ONLY ON `recommend-strategy`.** Passing it to
/// `divergence` is `error: UnknownFlag` at exit 2 — `divergence` always reads
/// BOTH sources by construction. Correspondingly `recommend-strategy`'s JSON
/// carries a `closure_source` field and `divergence`'s does not.
///
/// **`jaccard` IS RENDERED TWO DIFFERENT WAYS.** JSON emits the shortest
/// round-trip form (`0`, `1`, `0.5`, `0.3333333333333333`); text emits fixed
/// four-decimal (`0.0000`, `1.0000`, `0.5000`, `0.3333`). A single formatter
/// for both would be wrong in one of the two arms.
///
/// The two verbs DO agree on the candidate set (`status = 'todo'`, ordered by
/// `priority, id`) and on both refusal arms: a missing plan is exit 1
/// `plan {id} not found`, and a non-integer id is exit 2 `plan id must be an
/// integer, got '{raw}'`.
///
/// ## SUBSTRATE NOTES THAT COST A CYCLE IF ASSUMED
///
/// **RULE 1 READS `relationship = 'depends-on'`, NOT `'blocks'`.** The oracle's
/// own comments still say `blocks` (migration 00033 renamed it and the prose
/// was not updated); the SQL is authoritative.
///
/// **THE `excluded_by` ARRAY ORDER IS THE RULE-APPLICATION ORDER, AND IT IS
/// NOT SORTED.** The oracle applies 1, 5, 6, 2-empty, 3, 4, and only then the
/// rule-2 pairwise overlap. A task tripping five rules emitted them in exactly
/// that sequence, so `[1, 5, 6, 3, 4]` — not `[1, 3, 4, 5, 6]`.
///
/// **THE RULE-2 OVERLAP PASS RUNS ONLY OVER SURVIVORS, AND IT IS
/// ORDER-DEPENDENT.** A task already dropped by a unilateral rule is skipped
/// as both `i` and `j`, so it cannot cascade a rule-2 exclusion onto a peer
/// whose only conflict was with it. Within the pass, `j` is re-checked for
/// `dropped` on every iteration but `i` is checked only once, so `i` can
/// accumulate several rule-2 exclusions while a `j` dropped earlier in the
/// same pass is skipped later. Both halves are reproduced verbatim.
///
/// **BOTH EXCLUSIONS OF AN OVERLAPPING PAIR NAME THE SAME TOUCH.**
/// `shared_touch(a, b)` returns the touch from its FIRST argument, and the
/// caller formats one description for both sides. A path-vs-whole-repo
/// conflict therefore reports the PATH on both tasks, never `repo:N (whole
/// repo)`.
///
/// READ-ONLY: this module only SELECTs. No writes, no migration.
module;

export module planar.engine.planning.strategy;

import std;
import planar.db;

namespace planar::engine::planning::strategy {

/// @brief Why a strategy computation failed.
export enum class strategy_error : std::uint8_t {
  not_found,    ///< The plan does not exist.
  query_failed, ///< SQL failure.
};

/// @brief Which signal rule 2's pairwise overlap test reads (decision D4).
///
/// Only rule 2's overlap branch switches. Rules 1/3/4/5/6 — and rule 2's
/// empty-touches branch — always read the DECLARED set, because a migration
/// touch, a singleton touch and the touches-everything guard are all
/// declared-path properties rather than closure properties.
export enum class closure_source : std::uint8_t {
  declared, ///< Baseline: `task_touch_paths` + whole-repo `entity_links` edges.
  derived,  ///< The computed symbol-level closure in `closures`.
};

/// @brief Parse the `--closure-source` flag value.
/// @param text The operator-supplied token.
/// @return The source, or `nullopt` for an unknown token so the caller can
/// emit the oracle's precise refusal.
export auto parse_closure_source(std::string_view text) -> std::optional<closure_source>;

/// @brief The flag spelling for a source, as it appears in both renderings.
/// @param source The source.
/// @return `"declared"` or `"derived"`.
export auto closure_source_name(closure_source source) -> std::string_view;

/// @brief Canonical coordination files (rule 4).
///
/// A task touching any of these serializes. Exported so the classifier can be
/// tested directly and so the list stays greppable — the oracle's tech-spec
/// notes it may grow.
/// @return The four canonical singleton paths, in the oracle's own order.
export auto singleton_files() -> std::span<const std::string_view>;

/// @brief True when `path` is one of the canonical singleton files (rule 4).
/// @param path A repo-relative declared path.
/// @return Whether rule 4 fires for it.
export auto is_singleton_file(std::string_view path) -> bool;

/// @brief True when `path` is a schema migration (rule 3).
///
/// Matches a `migrations/` PREFIX and a `.sql` SUFFIX — so
/// `src/migrations/x.sql` does NOT match, and `migrations/README.md` does not
/// either.
/// @param path A repo-relative declared path.
/// @return Whether rule 3 fires for it.
export auto is_migration_path(std::string_view path) -> bool;

/// @brief One exclusion reason: the rule number (1..6) and its operator-facing
/// text.
export struct exclusion {
  std::uint8_t rule = 0; ///< The rule number that fired.
  std::string  reason;   ///< The oracle's exact reason string.
};

/// @brief One task in the recommendation.
export struct task_ref {
  std::int64_t               id = 0;      ///< `tasks.id`.
  std::optional<std::string> slug;        ///< `tasks.slug`, renders `null` when absent.
  std::string                title;       ///< `tasks.title`.
  std::vector<exclusion>     excluded_by; ///< Empty for an eligible task.
};

/// @brief The full recommendation for a plan.
export struct recommendation {
  std::int64_t          plan_id = 0;               ///< The plan asked about.
  std::vector<task_ref> parallel_eligible;         ///< Survivors of all six rules.
  std::vector<task_ref> serialized;                ///< Dropped tasks, with reasons.
  std::size_t           open_tasks        = 0;     ///< Candidates considered.
  bool                  fan_out_available = false; ///< `parallel_eligible.size() >= 2`.
};

/// @brief The declared-vs-derived rule-2 overlap divergence for a plan.
///
/// Note it carries NO `plan_id`: the oracle's `Divergence` struct does not,
/// and its handler supplies the id to the renderer. Kept split so the
/// renderer signature matches the oracle's own division of labour.
export struct divergence_result {
  std::size_t open_tasks        = 0;   ///< Open tasks considered.
  std::size_t pairs             = 0;   ///< Unordered pairs (`open_tasks choose 2`).
  std::size_t declared_overlaps = 0;   ///< Pairs overlapping under DECLARED.
  std::size_t derived_overlaps  = 0;   ///< Pairs overlapping under DERIVED.
  std::size_t flips             = 0;   ///< Pairs whose verdict differs.
  double      jaccard           = 0.0; ///< `flips / |declared ∪ derived|`; 0.0 when the union is empty.
};

/// @brief Compute the parallel-eligibility recommendation under an explicit
/// closure source.
/// @param conn An open database connection.
/// @param plan_id The plan to analyse.
/// @param source Which signal rule 2's overlap branch reads.
/// @return The recommendation, or `not_found` when the plan does not exist.
export auto recommend_with(db::connection& conn, std::int64_t plan_id, closure_source source)
    -> std::expected<recommendation, strategy_error>;

/// @brief Compute the recommendation under the baseline DECLARED source.
///
/// Behaviour-preserving wrapper over `recommend_with(..., declared)`.
/// @param conn An open database connection.
/// @param plan_id The plan to analyse.
/// @return The recommendation, or `not_found` when the plan does not exist.
export auto recommend(db::connection& conn, std::int64_t plan_id) -> std::expected<recommendation, strategy_error>;

/// @brief Compute the rule-2 overlap divergence between the two sources.
///
/// Applies ONLY rule 2's pairwise overlap test — no unilateral rules, no
/// partitioning — so the number isolates exactly the signal D4 swaps.
/// @param conn An open database connection.
/// @param plan_id The plan to analyse.
/// @return The measure, or `not_found` when the plan does not exist.
export auto compute_divergence(db::connection& conn, std::int64_t plan_id) -> std::expected<divergence_result, strategy_error>;

/// @brief Render `plan recommend-strategy --json`.
/// @param rec The recommendation.
/// @param source The source that produced it (echoed as `closure_source`).
/// @return A COMPLETE stdout payload including its trailing newline.
export auto render_recommendation_json(const recommendation& rec, closure_source source) -> std::string;

/// @brief Render `plan recommend-strategy` in text mode.
/// @param rec The recommendation.
/// @param source The source that produced it.
/// @return A COMPLETE stdout payload including its trailing newline.
export auto render_recommendation_text(const recommendation& rec, closure_source source) -> std::string;

/// @brief Render `plan divergence --json`.
/// @param plan_id The plan asked about (not carried by `divergence_result`).
/// @param div The measure.
/// @return A COMPLETE stdout payload including its trailing newline.
export auto render_divergence_json(std::int64_t plan_id, const divergence_result& div) -> std::string;

/// @brief Render `plan divergence` in text mode.
/// @param plan_id The plan asked about.
/// @param div The measure.
/// @return A COMPLETE stdout payload including its trailing newline.
export auto render_divergence_text(std::int64_t plan_id, const divergence_result& div) -> std::string;

/// @brief The one-line strategy hint both renderings carry.
///
/// Three distinct strings, all oracle-captured: the fan-out arm names the
/// count, and the two no-fan-out arms differ on whether exactly one task was
/// eligible.
/// @param rec The recommendation.
/// @return The note text, without a terminator.
export auto recommended_note(const recommendation& rec) -> std::string;

} // namespace planar::engine::planning::strategy
