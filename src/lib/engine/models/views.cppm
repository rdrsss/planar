/// @file views.cppm
/// @brief `planar.engine.models.views` — read-only inspection of the routing
/// evidence plane behind `planar models experiments` and
/// `planar models outcomes` (plan 996, task 6096).
///
/// Behavior-preserving port (D2) of zig/src/engine/routing/views.zig.
/// Everything here answers "what does Planar believe, and on what basis?".
/// Nothing writes.
///
/// ## An exclusion must always be NAMED
///
/// This is the organizing rule of both views. A sample that does NOT count
/// toward a recommendation is more informative than one that does: it tells
/// an operator that a run happened, was recorded, and was deliberately set
/// aside — and why. So:
///
///   - `list_outcomes` returns excluded samples alongside counted ones.
///     Omitting them would make the evidence look thinner than it is and
///     leave no way to audit where the boundary was drawn.
///   - `outcome::exclusion_reason` is always present when
///     `counts_toward_recommendation` is false. An unexplained exclusion is
///     indistinguishable from a bug.
///   - `experiment::samples` and `::eligible_samples` are reported
///     separately, so the excluded count is visible as their difference
///     rather than hidden inside a single total.
///
/// Oracle-captured, against a fixture with 64 recorded samples of which one
/// was a `candidate_mismatch` excluded as `actual_candidate_differs`:
///
///     $Z models experiments
///       -> samples: 64 recorded, 63 counted (1 excluded)
///     $Z models outcomes --limit 2
///       -> [64] cand-1  candidate_mismatch  w-1-0
///                EXCLUDED from recommendations: actual_candidate_differs
///          [63] cand-4  quality_failed  w-4-19
///                counts toward recommendation (quality_success=no)
///
/// Note the second row: `quality_failed` still COUNTS. A failure is evidence,
/// and suppressing it would bias every rate upward. Only an evidence-boundary
/// problem (the wrong model actually ran, the record is incomplete) excludes.
///
/// ## Ordering, oracle-confirmed
///
/// - Experiments: `id` ASCENDING — declaration order, which is the order an
///   operator declared them in.
/// - Outcomes: `id` DESCENDING — newest first, because the recent tail is
///   what an operator is almost always asking about.

module;

export module planar.engine.models.views;

import std;
import planar.db;

namespace planar::engine::models::views {

/// @brief Bumped when either view's shape changes, so two captures taken
/// under different rules are never compared as though they agreed.
export inline constexpr std::string_view views_version = "routing-views-v1";

/// @brief One declared experiment with the manifest identity frozen at
/// approval.
///
/// The manifest BODY is deliberately not expanded: what an operator needs at
/// a glance is which cohort it governs, whether it is still running, and how
/// much evidence it has produced. `population_size` and `candidate_count` are
/// derived by counting the frozen JSON manifests rather than by re-deriving
/// eligibility, so they report what was actually frozen.
export struct experiment {
  std::int64_t id = 0;                    ///< The `routing_experiments.id`.
  std::string  experiment_key;            ///< The operator-facing unique key.
  std::string  status;                    ///< declared / running / stopped / completed / cancelled.
  std::string  vendor;                    ///< Cohort vendor.
  std::string  role;                      ///< Cohort role.
  std::string  tier;                      ///< Cohort tier.
  std::string  work_type;                 ///< Cohort work type.
  std::string  complexity;                ///< Cohort complexity band.
  std::string  validation_policy_version; ///< Frozen validation policy.
  std::string  routing_policy_version;    ///< Frozen routing policy.
  std::string  manifest_digest;           ///< Digest of the frozen manifest.
  std::string  operator_approved_at;      ///< When an operator approved it.
  std::int64_t population_size  = 0;      ///< Work items the frozen manifest admits.
  std::int64_t candidate_count  = 0;      ///< Candidates the frozen manifest admits.
  std::int64_t samples          = 0;      ///< Terminal samples recorded so far.
  std::int64_t eligible_samples = 0;      ///< How many of those count toward a recommendation.
};

/// @brief One recorded terminal sample.
///
/// `counts_toward_recommendation` is stated DIRECTLY rather than left for the
/// reader to infer from a raw `cohort_eligible` column — the inference is
/// exactly the step a reader gets wrong.
export struct outcome {
  std::int64_t id            = 0;                    ///< The `routing_terminal_samples.id`.
  std::int64_t experiment_id = 0;                    ///< The declaring experiment.
  std::string  logical_work_item_id;                 ///< The work item this sample measured.
  std::string  role;                                 ///< Cohort role.
  std::string  vendor;                               ///< Cohort vendor.
  std::string  candidate;                            ///< The opaque candidate identifier.
  std::string  tier;                                 ///< Cohort tier.
  std::string  work_type;                            ///< Cohort work type.
  std::string  complexity;                           ///< Cohort complexity band.
  std::string  terminal_state;                       ///< completed / quality_failed / ... .
  bool         quality_success              = false; ///< Whether validation passed.
  bool         counts_toward_recommendation = false; ///< `cohort_eligible`.
  /// Why this sample was excluded. Set exactly when
  /// `counts_toward_recommendation` is false — the schema enforces the
  /// biconditional with a CHECK.
  std::optional<std::string> exclusion_reason;
  std::string                finalized_at; ///< When the sample was derived.
};

/// @brief Failure surface for this module.
export enum class views_error {
  query_failed ///< Any SQLite failure.
};

/// @brief List every declared experiment, oldest first, with its evidence
/// counts.
/// @param conn An open, migrated database connection.
/// @return Every experiment, ordered by `id` ascending.
export auto list_experiments(db::connection& conn) -> std::expected<std::vector<experiment>, views_error>;

/// @brief List terminal outcomes, newest first, INCLUDING excluded ones.
///
/// Excluded samples are returned deliberately — see this module's header.
/// @param conn An open, migrated database connection.
/// @param limit Maximum rows to return. The CLI defaults this to 50 and
/// refuses a non-positive value before calling in
/// (`error: --limit must be positive`, exit 2, oracle-captured).
/// @return Up to `limit` outcomes, ordered by `id` descending.
export auto list_outcomes(db::connection& conn, std::int64_t limit) -> std::expected<std::vector<outcome>, views_error>;

} // namespace planar::engine::models::views
