/// @file ranking.cppm
/// @brief `planar.engine.models.ranking` — evidence-backed candidate ranking
/// over declared-experiment terminal samples, behind `planar models evals`'s
/// cohort branch (plan 996, task 6096).
///
/// Behavior-preserving port (D2) of zig/src/engine/routing/ranking.zig.
///
/// ## Why a Wilson lower bound and not a raw rate
///
/// A raw success rate cannot distinguish 1-for-1 from 40-for-40; both are
/// 100%. Ranking on the raw rate therefore hands first place to whichever
/// candidate happened to be tried once and got lucky. The 95% Wilson score
/// interval's LOWER bound answers the question that actually matters — "what
/// is the worst success rate consistent with this evidence?" — so thin
/// evidence is penalized automatically rather than by a hand-tuned prior.
///
/// Oracle-derived, with the exact numbers this port reproduces. A fixture of
/// four candidates in one cohort (`anthropic/coder/medium/engine/standard`,
/// project 1, policies `vp-1` + `rp-1`), seeded through the full
/// experiment -> snapshot -> event -> terminal-sample chain, then:
///
///     $Z models evals --vendor anthropic --project 1 \
///        --validation-policy vp-1 --routing-policy rp-1 --role coder \
///        --tier medium --work-type engine --complexity standard --json
///
/// yields, verbatim:
///
///   | candidate | samples | successes | raw | wilson_lower        | verdict             |
///   |-----------|---------|-----------|-----|---------------------|---------------------|
///   | cand-1    | 20      | 16        | 0.8 | 0.5839825677481064  | rank 1, recommended |
///   | cand-2    | 20      | 12        | 0.6 | 0.38658150076225317 | below_quality_floor |
///   | cand-4    | 20      | 4         | 0.2 | 0.08065766257979808 | below_quality_floor |
///   | cand-3    | 3       | 3         | 1   | 0.4385029682449545  | insufficient_data   |
///
/// Note `cand-3`: a PERFECT raw rate that still ranks nowhere, because three
/// samples is under the minimum and its Wilson bound (0.4385) would not clear
/// the 0.5 floor anyway. That row is the whole argument for this module.
///
/// ## The three outcomes, all oracle-pinned
///
/// 1. **A recommendation.** As above — `"recommended":"cand-1"`,
///    `"no_recommendation_reason":null`. Note the wire field carries the
///    candidate's opaque STRING, not its row index.
/// 2. **Gated out.** Re-running the same cohort with `--quality-floor 0.99`
///    marks every row `below_quality_floor` and yields
///    `"recommended":null,"no_recommendation_reason":"no candidate cleared
///    both the minimum-sample and quality-floor gates"`.
/// 3. **No evidence at all.** The same command with `--complexity bounded`
///    (a cohort with zero samples) yields `"rows":[]`, `"recommended":null`,
///    `"no_recommendation_reason":"no cohort-eligible declared-experiment
///    samples"`.
///
/// "No recommendation" is a first-class, correct answer in both gated cases.
/// It is never softened into "the best of a bad set".
///
/// ## Gate ordering is load-bearing
///
/// The quality floor is applied BEFORE any latency, cost, gate-failure, or
/// iteration ordering. A fast, cheap, wrong candidate must never outrank a
/// slower correct one, so speed is only ever a tie-break among candidates
/// that already cleared quality.
///
/// ## Evidence is never pooled across cohorts
///
/// A candidate that is good at `mechanical/bounded` work has told you
/// NOTHING about `architectural/high-risk` work. Every field of `cohort` is
/// an equality predicate in the aggregation query, and `cohort_eligible = 1`
/// excludes the samples the evidence boundary already set aside.

module;

export module planar.engine.models.ranking;

import std;
import planar.db;
import planar.engine.models.registry;

namespace planar::engine::models::ranking {

/// @brief Bumped when aggregation or ranking semantics change, so results
/// computed under different rules are never presented as comparable.
export inline constexpr std::string_view ranking_version = "routing-ranking-v1";

/// @brief z for a two-sided 95% interval.
///
/// Fixed rather than configurable: the reported number is LABELLED "95%
/// Wilson lower bound", so the constant and the label have to move together.
/// Changing this without changing the label would silently mis-describe every
/// number the verb prints.
export inline constexpr double z_95 = 1.959963984540054;

/// @brief The work-type set, mirroring the schema's CHECK constraint.
export enum class work_type {
  schema,        ///< Migrations and schema contracts.
  engine,        ///< Domain engine logic.
  architectural, ///< Cross-cutting structural change.
  cli,           ///< Command-surface work.
  feature,       ///< Ordinary feature work.
  mechanical     ///< Mechanical sweeps.
};

/// @brief Parse the schema's work-type text.
/// @param text The wire spelling.
/// @return The value, or `std::nullopt` for anything else.
export auto work_type_from_text(std::string_view text) -> std::optional<work_type>;

/// @brief Render a work type as the schema's own text.
/// @param value The value to render.
/// @return The wire spelling.
export auto work_type_to_text(work_type value) -> std::string_view;

/// @brief The complexity set, mirroring the schema's CHECK constraint.
export enum class complexity {
  bounded,  ///< Small, well-fenced.
  standard, ///< Ordinary.
  high_risk ///< Spelled `high-risk` on the wire — see `complexity_to_text`.
};

/// @brief Parse the complexity text, accepting the HYPHENATED wire spelling.
///
/// The wire spells it `high-risk`; the enum cannot. Both `high-risk` and the
/// underscored `high_risk` are accepted here so a caller that already
/// normalized is not punished for it.
/// @param text The wire spelling.
/// @return The value, or `std::nullopt` for anything else.
export auto complexity_from_text(std::string_view text) -> std::optional<complexity>;

/// @brief Render a complexity as the schema's own text.
///
/// Returns the HYPHENATED `high-risk`, matching the schema CHECK and the
/// query predicate — an underscored spelling would silently match zero rows.
/// @param value The value to render.
/// @return The wire spelling.
export auto complexity_to_text(complexity value) -> std::string_view;

/// @brief The exact cohort a candidate is ranked within.
///
/// Every field is an equality predicate. Evidence is never pooled across
/// cohorts — see this module's header.
export struct cohort {
  std::int64_t   project_id = 0;                       ///< The owning project.
  std::string    validation_policy_version;            ///< Frozen validation policy.
  std::string    routing_policy_version;               ///< Frozen routing policy.
  std::string    vendor;                               ///< Opaque vendor string.
  std::string    role;                                 ///< Opaque role string.
  registry::tier tier_       = registry::tier::medium; ///< The routing tier.
  work_type      work_type_  = work_type::engine;      ///< The work type.
  complexity     complexity_ = complexity::standard;   ///< The complexity band.
};

/// @brief Gate configuration. Both gates are REFUSALS, not adjustments.
///
/// Neither gate reweights a score — a candidate that fails one is excluded
/// from ranking entirely and shown with its reason. Softening a gate into a
/// penalty would let a gated candidate still win on a big enough margin
/// elsewhere, which is precisely what the gates exist to prevent.
export struct gates {
  /// Below this sample count a candidate is `insufficient_data`: shown,
  /// never ranked, never recommended. Default 5 (oracle-confirmed:
  /// `"gates":{"minimum_samples":5,...}`).
  std::uint64_t minimum_samples = 5;
  /// A candidate whose Wilson lower bound falls below this is excluded before
  /// any speed ordering. Default 0.5 (oracle-confirmed).
  double quality_floor = 0.5;
};

/// @brief One candidate's aggregated evidence and verdict within a cohort.
export struct row {
  std::int64_t          candidate_id = 0;                   ///< The `routing_candidates.id`.
  std::string           candidate;                          ///< The opaque candidate identifier.
  std::string           vendor;                             ///< The opaque vendor string.
  std::int64_t          fallback_order  = 0;                ///< Deterministic fallback position.
  std::uint64_t         samples         = 0;                ///< Cohort-eligible terminal samples.
  std::uint64_t         successes       = 0;                ///< Samples with `quality_success = 1`.
  std::uint64_t         gate_failures   = 0;                ///< Samples in terminal state `quality_failed`.
  std::uint64_t         excess_attempts = 0;                ///< Attempts beyond the first, summed.
  std::optional<double> mean_latency_ms;                    ///< Unset when NO sample reported latency.
  std::optional<double> mean_cost_micros;                   ///< Unset when NO sample reported cost.
  std::int64_t          measured_samples           = 0;     ///< Samples carrying latency or cost.
  double                raw_rate                   = 0;     ///< `successes / samples`; 0 when unsampled.
  double                wilson_lower               = 0;     ///< 95% Wilson lower bound.
  double                gate_failure_rate          = 0;     ///< `gate_failures / samples`.
  double                expected_excess_iterations = 0;     ///< `excess_attempts / samples`.
  bool                  insufficient_data          = false; ///< Under `gates::minimum_samples`.
  bool                  below_quality_floor        = false; ///< Under `gates::quality_floor`.
  /// 1-based rank among rows that cleared BOTH gates; unset for gated rows.
  std::optional<std::uint64_t> rank;
};

/// @brief The full ranking result for one cohort.
export struct result {
  std::string_view rows_version = ranking_version; ///< The semantics version these rows were computed under.
  std::vector<row> rows;                           ///< Every candidate with evidence, ranked-first then gated.
  /// The recommended candidate's OPAQUE IDENTIFIER (not its index) — the wire
  /// field is a string. Unset when no candidate cleared both gates.
  std::optional<std::string> recommended;
  /// Why there is no recommendation. Always set exactly when `recommended`
  /// is unset, and distinguishes "no evidence" from "all evidence gated".
  std::optional<std::string> no_recommendation_reason;
};

/// @brief Failure surface for this module.
export enum class ranking_error {
  query_failed ///< Any SQLite failure.
};

/// @brief Lower bound of the Wilson score interval for `successes` of `n`.
///
/// Returns 0 for n = 0 — no evidence is NOT weak evidence of success, and a
/// zero keeps an unsampled candidate below every sampled one without needing
/// a special case at the ranking site. Clamped at 0 from below for the same
/// reason: the interval's arithmetic lower end can go negative, and a
/// negative "success rate" is not a thing.
/// @param successes Successful samples.
/// @param n Total samples.
/// @param z The interval's z value; pass `z_95` to match the printed label.
/// @return The lower bound, in [0, 1].
export auto wilson_lower_bound(std::uint64_t successes, std::uint64_t n, double z) -> double;

/// @brief Apply both gates and derive every rate on one aggregated row.
///
/// The sample gate SHORT-CIRCUITS the quality gate: a row flagged
/// `insufficient_data` is NEVER also flagged `below_quality_floor`, even when
/// its bound genuinely is below the floor. "Below the floor" is a judgement
/// about quality, and an under-sampled row is one we have just declined to
/// judge. Oracle-verified — see this module's header and ranking.cpp.
///
/// Exposed (rather than kept private to `rank`) so the gate boundaries can be
/// tested directly at exact sample counts without staging a database.
/// @param aggregate The row, with its raw counts already filled in.
/// @param gate_config The gates to apply.
export auto finalize(row& aggregate, const gates& gate_config) -> void;

/// @brief Rank one exact cohort from declared-experiment evidence.
///
/// Reads `routing_terminal_samples` joined to `routing_candidates`, filtered
/// to `cohort_eligible = 1` and every cohort field. Writes nothing.
/// @param conn An open, migrated database connection.
/// @param target The exact cohort to rank within.
/// @param gate_config The minimum-sample and quality-floor gates.
/// @return The ranked rows plus the recommendation (or the reason there is
/// none).
export auto rank(db::connection& conn, const cohort& target, const gates& gate_config) -> std::expected<result, ranking_error>;

} // namespace planar::engine::models::ranking
