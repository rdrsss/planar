/// @file ranking.cpp
/// @brief Implementation of `planar.engine.models.ranking` (plan 996, task
/// 6096). See ranking.cppm for scope and the oracle-derived numeric fixtures.

module planar.engine.models.ranking;

import std;
import planar.db;
import planar.engine.models.registry;

namespace planar::engine::models::ranking {

// Exported (see ranking.cppm). Defined ahead of the anonymous namespace so
// the private helpers below can call it.
auto less_than(const row& lhs, const row& rhs) -> bool;

namespace {

/// @brief Guarantee a non-null `data()` pointer for an empty view.
///
/// `db::statement::bind_text` binds SQL **NULL** for a default-constructed
/// `std::string_view` (src/lib/db/db.cpp:98; task 6097 tracks the root fix).
/// Same guard as engine/runs/lifecycle.cpp's and registry.cpp's `nn`.
///
/// UNLIKE those two, this one is currently NOT observable, and saying so is
/// more useful than implying otherwise. A break-probe replacing it with the
/// identity function passed every test, and that is correct rather than a
/// gap in the suite: every free-text cohort column on
/// `routing_terminal_samples` carries a `check (length(...) > 0)` constraint
/// (`role`, `vendor`, `validation_policy_version`, `routing_policy_version`),
/// and `tier` / `work_type` / `complexity` are rendered from closed enums. So
/// no stored row can hold an empty string in a cohort column, which means
/// binding `''` and binding `NULL` BOTH match zero rows — the two spellings
/// are indistinguishable through this query.
///
/// It is kept as defence in depth rather than deleted because the failure it
/// guards against is the silent kind: were a length CHECK ever relaxed, the
/// NULL bind would report "no cohort-eligible declared-experiment samples"
/// for a cohort that has plenty — a confident, well-formatted, wrong answer
/// rather than an error. The cost of the guard is one comparison; the cost of
/// its absence is unbounded.
auto nn(std::string_view s) -> std::string_view {
  return s.data() == nullptr ? std::string_view{""} : s;
}

/// @brief Compare an optional metric, lower-is-better, only when BOTH sides
/// carry one.
///
/// Returns unset when either is unmeasured, so the caller falls through to the
/// next key rather than inventing an ordering. Treating a missing value as 0
/// would promote the candidate we know LEAST about; treating it as infinity
/// would bury a candidate merely for not being instrumented. Neither is a
/// judgement the evidence supports, so the metric simply does not participate.
auto compare_optional(std::optional<double> lhs, std::optional<double> rhs) -> std::optional<bool> {
  if (!lhs.has_value() || !rhs.has_value()) {
    return std::nullopt;
  }
  if (*lhs == *rhs) {
    return std::nullopt;
  }
  return *lhs < *rhs;
}

/// @brief Eligible rows first (ranked among themselves), then gated-out rows
/// in a stable, explainable order rather than whatever the query returned.
auto sort_eligible_first(const row& lhs, const row& rhs) -> bool {
  const bool lhs_gated = lhs.insufficient_data || lhs.below_quality_floor;
  const bool rhs_gated = rhs.insufficient_data || rhs.below_quality_floor;
  if (lhs_gated != rhs_gated) {
    return !lhs_gated;
  }
  if (lhs_gated) {
    // Among gated rows: more evidence first, then configured order.
    if (lhs.samples != rhs.samples) {
      return lhs.samples > rhs.samples;
    }
    return lhs.fallback_order < rhs.fallback_order;
  }
  return less_than(lhs, rhs);
}

/// The aggregation query, transcribed from zig/src/engine/routing/ranking.zig.
///
/// The `excess_attempts` sub-select is deliberately kept verbatim, including
/// its `or e.dispatch_id = (...)` arm: it sums `max(attempt_number) - 1` over
/// every event belonging to the dispatch the terminal event came from, which
/// is what makes a retried dispatch contribute its retries rather than just
/// its terminal row.
constexpr std::string_view k_rank_sql =
    "select "
    "  c.id, "
    "  c.candidate_id, "
    "  c.vendor, "
    "  c.fallback_order, "
    "  count(*) as samples, "
    "  sum(s.quality_success) as successes, "
    "  sum(case when s.terminal_state = 'quality_failed' then 1 else 0 end) as gate_failures, "
    "  coalesce(sum( "
    "    (select max(e.attempt_number) - 1 "
    "     from routing_dispatch_events as e "
    "     where e.event_id = s.terminal_event_id "
    "        or e.dispatch_id = ( "
    "          select d2.dispatch_id from routing_dispatch_events as d2 "
    "          where d2.event_id = s.terminal_event_id "
    "        )) "
    "  ), 0) as excess_attempts, "
    "  avg(s.latency_ms) as mean_latency_ms, "
    "  avg(s.cost_micros) as mean_cost_micros, "
    "  sum(case when s.latency_ms is not null or s.cost_micros is not null "
    "      then 1 else 0 end) as measured_samples "
    "from routing_terminal_samples as s "
    "join routing_candidates as c on c.id = s.candidate_id "
    "where s.cohort_eligible = 1 "
    "  and s.project_id = ? "
    "  and s.validation_policy_version = ? "
    "  and s.routing_policy_version = ? "
    "  and s.vendor = ? "
    "  and s.role = ? "
    "  and s.tier = ? "
    "  and s.work_type = ? "
    "  and s.complexity = ? "
    "group by c.id, c.candidate_id, c.vendor, c.fallback_order";

auto non_negative(std::int64_t value) -> std::uint64_t {
  return value < 0 ? 0U : static_cast<std::uint64_t>(value);
}

} // namespace

// Exported. The key order, the descending primary, and the abstention rule for
// keys 4a/4b are documented on the declaration in ranking.cppm; ranking.t.cpp
// pins every one of the five keys against THIS function rather than a local
// copy of its rules (task 6114, review finding F4).
auto less_than(const row& lhs, const row& rhs) -> bool {
  if (lhs.wilson_lower != rhs.wilson_lower) {
    return lhs.wilson_lower > rhs.wilson_lower;
  }
  if (lhs.expected_excess_iterations != rhs.expected_excess_iterations) {
    return lhs.expected_excess_iterations < rhs.expected_excess_iterations;
  }
  if (lhs.gate_failure_rate != rhs.gate_failure_rate) {
    return lhs.gate_failure_rate < rhs.gate_failure_rate;
  }
  if (const auto latency = compare_optional(lhs.mean_latency_ms, rhs.mean_latency_ms)) {
    return *latency;
  }
  if (const auto cost = compare_optional(lhs.mean_cost_micros, rhs.mean_cost_micros)) {
    return *cost;
  }
  return lhs.fallback_order < rhs.fallback_order;
}

auto work_type_from_text(std::string_view text) -> std::optional<work_type> {
  if (text == "schema") {
    return work_type::schema;
  }
  if (text == "engine") {
    return work_type::engine;
  }
  if (text == "architectural") {
    return work_type::architectural;
  }
  if (text == "cli") {
    return work_type::cli;
  }
  if (text == "feature") {
    return work_type::feature;
  }
  if (text == "mechanical") {
    return work_type::mechanical;
  }
  return std::nullopt;
}

auto work_type_to_text(work_type value) -> std::string_view {
  switch (value) {
  case work_type::schema:
    return "schema";
  case work_type::engine:
    return "engine";
  case work_type::architectural:
    return "architectural";
  case work_type::cli:
    return "cli";
  case work_type::feature:
    return "feature";
  case work_type::mechanical:
    return "mechanical";
  }
  return "engine";
}

auto complexity_from_text(std::string_view text) -> std::optional<complexity> {
  if (text == "bounded") {
    return complexity::bounded;
  }
  if (text == "standard") {
    return complexity::standard;
  }
  // The wire spelling is hyphenated; the underscored form is accepted too so
  // a caller that already normalized is not punished for it.
  if (text == "high-risk" || text == "high_risk") {
    return complexity::high_risk;
  }
  return std::nullopt;
}

auto complexity_to_text(complexity value) -> std::string_view {
  switch (value) {
  case complexity::bounded:
    return "bounded";
  case complexity::standard:
    return "standard";
  case complexity::high_risk:
    // HYPHENATED, matching the schema CHECK and the query predicate. An
    // underscored spelling here would match zero rows and be reported as
    // "no evidence" rather than as an error.
    return "high-risk";
  }
  return "standard";
}

/// FLOATING-POINT CONTRACTION MUST STAY OFF FOR THIS FUNCTION.
///
/// `p * (1.0 - p) + z2 / (4.0 * nf)` is exactly the multiply-add shape clang
/// fuses into a single `fma` under its DEFAULT `-ffp-contract=on`. The fused
/// form skips one intermediate rounding, and for `wilson_lower_bound(12, 20)`
/// that shifts the result by one ULP:
///
///     contracted (clang default) : 0.38658150076225312
///     uncontracted / the oracle  : 0.38658150076225317
///
/// One ULP is not a rounding curiosity here — `render.cpp` prints the
/// shortest round-trippable form of this double, so the last digit lands in
/// the JSON and parity breaks on a byte diff. This was NOT predicted; it was
/// caught by ranking.t.cpp's exact-equality fixture failing against the
/// transcribed oracle value, and diagnosed by rebuilding the same expression
/// with and without `-ffp-contract=off`.
///
/// `-ffp-contract=off` is set on THIS SOURCE FILE (and on ranking.t.cpp) by
/// engine/models/CMakeLists.txt — a source-level property, deliberately not a
/// target-level one, because a target-level compile option perturbs CMake's
/// `import std;` module-settings grouping and drags `-Werror` onto libc++'s
/// own std.cppm. See that file's comment for the full account.
///
/// The standing guard against the flag being dropped is the test itself:
/// `models.ranking: the wilson bound reproduces the oracle's exact doubles`
/// fails by name the moment contraction returns.
auto wilson_lower_bound(std::uint64_t successes, std::uint64_t n, double z) -> double {
  if (n == 0) {
    return 0;
  }
  const double nf     = static_cast<double>(n);
  const double p      = static_cast<double>(successes) / nf;
  const double z2     = z * z;
  const double denom  = 1.0 + z2 / nf;
  const double center = p + z2 / (2.0 * nf);
  const double margin = z * std::sqrt((p * (1.0 - p) + z2 / (4.0 * nf)) / nf);
  const double lower  = (center - margin) / denom;
  return lower < 0 ? 0.0 : lower;
}

auto finalize(row& aggregate, const gates& gate_config) -> void {
  if (aggregate.samples > 0) {
    const double n                       = static_cast<double>(aggregate.samples);
    aggregate.raw_rate                   = static_cast<double>(aggregate.successes) / n;
    aggregate.gate_failure_rate          = static_cast<double>(aggregate.gate_failures) / n;
    aggregate.expected_excess_iterations = static_cast<double>(aggregate.excess_attempts) / n;
  }
  aggregate.wilson_lower = wilson_lower_bound(aggregate.successes, aggregate.samples, z_95);

  // The sample gate SHORT-CIRCUITS: an under-sampled row is never ALSO
  // labelled below-the-floor, even when its bound is in fact below the floor.
  // This is not an accident of the original's control flow — it is the right
  // reading. "Below the quality floor" is a JUDGEMENT about quality, and we
  // have just said we do not have enough evidence to judge this candidate at
  // all; asserting both would claim a finding the evidence does not support.
  //
  // Oracle-verified rather than assumed, because the two flags look
  // independent from the outside. `cand-3` (3 samples, 3 successes, Wilson
  // 0.4385) under the DEFAULT floor of 0.5 reports:
  //     "insufficient_data":true,"below_quality_floor":false
  // even though 0.4385 < 0.5. An implementation evaluating both gates
  // independently would emit `true,true` here and break parity.
  if (aggregate.samples < gate_config.minimum_samples) {
    aggregate.insufficient_data = true;
    return;
  }
  if (aggregate.wilson_lower < gate_config.quality_floor) {
    aggregate.below_quality_floor = true;
  }
}

auto rank(db::connection& conn, const cohort& target, const gates& gate_config) -> std::expected<result, ranking_error> {
  auto stmt = conn.prepare(k_rank_sql);
  if (!stmt) {
    return std::unexpected(ranking_error::query_failed);
  }
  if (!stmt->bind_int64(1, target.project_id) || !stmt->bind_text(2, nn(target.validation_policy_version)) ||
      !stmt->bind_text(3, nn(target.routing_policy_version)) || !stmt->bind_text(4, nn(target.vendor)) ||
      !stmt->bind_text(5, nn(target.role)) || !stmt->bind_text(6, registry::tier_to_text(target.tier_)) ||
      !stmt->bind_text(7, work_type_to_text(target.work_type_)) || !stmt->bind_text(8, complexity_to_text(target.complexity_))) {
    return std::unexpected(ranking_error::query_failed);
  }

  result out;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(ranking_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    row aggregate{
        .candidate_id     = stmt->column_int64(0),
        .candidate        = stmt->column_text(1),
        .vendor           = stmt->column_text(2),
        .fallback_order   = stmt->column_int64(3),
        .samples          = non_negative(stmt->column_int64(4)),
        .successes        = non_negative(stmt->column_int64(5)),
        .gate_failures    = non_negative(stmt->column_int64(6)),
        .excess_attempts  = non_negative(stmt->column_int64(7)),
        .mean_latency_ms  = stmt->is_null(8) ? std::optional<double>{} : std::optional<double>{stmt->column_double(8)},
        .mean_cost_micros = stmt->is_null(9) ? std::optional<double>{} : std::optional<double>{stmt->column_double(9)},
        .measured_samples = stmt->column_int64(10),
    };
    finalize(aggregate, gate_config);
    out.rows.push_back(std::move(aggregate));
  }

  // Stable, matching zig's `std.mem.sort`: two rows the comparator calls
  // equivalent keep the query's `group by` order rather than swapping between
  // runs.
  std::stable_sort(out.rows.begin(), out.rows.end(), sort_eligible_first);

  std::uint64_t next_rank = 1;
  for (auto& ranked : out.rows) {
    // Rank ONLY rows that cleared both gates. Numbering a gated row would put
    // a rank next to a candidate we just said we cannot judge.
    if (ranked.insufficient_data || ranked.below_quality_floor) {
      continue;
    }
    ranked.rank = next_rank;
    ++next_rank;
  }

  if (next_rank == 1) {
    out.recommended              = std::nullopt;
    out.no_recommendation_reason = out.rows.empty()
                                       ? std::string{"no cohort-eligible declared-experiment samples"}
                                       : std::string{"no candidate cleared both the minimum-sample and quality-floor gates"};
  } else {
    // The sort put the single best row first, so index 0 is the
    // recommendation. The WIRE field is the opaque candidate string, not the
    // index — oracle-confirmed (`"recommended":"cand-1"`).
    out.recommended = out.rows.front().candidate;
  }
  return out;
}

} // namespace planar::engine::models::ranking
