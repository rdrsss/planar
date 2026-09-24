/// @file views.cpp
/// @brief Implementation of `planar.engine.models.views` (plan 996, task
/// 6096). See views.cppm for scope and the oracle-derived output shapes.

module planar.engine.models.views;

import std;
import planar.db;

namespace planar::engine::models::views {

namespace {

/// The experiments query, transcribed from zig/src/engine/routing/views.zig.
///
/// `population_size` and `candidate_count` come from `json_each` over the
/// FROZEN manifests, not from a live eligibility re-derivation: the whole
/// point of freezing a manifest is that its population cannot drift after
/// approval, so counting anything else would report a number the experiment
/// is not actually running against.
constexpr std::string_view k_experiments_sql =
    "select e.id, e.experiment_key, e.status, e.vendor, e.role, e.tier, "
    "       e.work_type, e.complexity, e.validation_policy_version, "
    "       e.routing_policy_version, e.manifest_digest, e.operator_approved_at, "
    "       (select count(*) from json_each(e.eligible_population_json)), "
    "       (select count(*) from json_each(e.candidate_set_json)), "
    "       (select count(*) from routing_terminal_samples s where s.experiment_id = e.id), "
    "       (select count(*) from routing_terminal_samples s "
    "          where s.experiment_id = e.id and s.cohort_eligible = 1) "
    "from routing_experiments as e "
    "order by e.id asc";

/// The outcomes query, transcribed from the same file.
///
/// `coalesce(s.exclusion_reason, '')` then mapping the empty string back to
/// "unset" mirrors the Zig original exactly. It matters because the schema
/// permits NULL only when the sample counts, so an empty-string reason and a
/// NULL reason are the same state and must render identically.
constexpr std::string_view k_outcomes_sql = "select s.id, s.experiment_id, s.logical_work_item_id, s.role, s.vendor, "
                                            "       c.candidate_id, s.tier, s.work_type, s.complexity, s.terminal_state, "
                                            "       s.quality_success, s.cohort_eligible, coalesce(s.exclusion_reason,''), "
                                            "       s.finalized_at "
                                            "from routing_terminal_samples as s "
                                            "join routing_candidates as c on c.id = s.candidate_id "
                                            "order by s.id desc "
                                            "limit ?";

} // namespace

auto list_experiments(db::connection& conn) -> std::expected<std::vector<experiment>, views_error> {
  auto stmt = conn.prepare(k_experiments_sql);
  if (!stmt) {
    return std::unexpected(views_error::query_failed);
  }
  std::vector<experiment> out;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(views_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    out.push_back(experiment{
        .id                        = stmt->column_int64(0),
        .experiment_key            = stmt->column_text(1),
        .status                    = stmt->column_text(2),
        .vendor                    = stmt->column_text(3),
        .role                      = stmt->column_text(4),
        .tier                      = stmt->column_text(5),
        .work_type                 = stmt->column_text(6),
        .complexity                = stmt->column_text(7),
        .validation_policy_version = stmt->column_text(8),
        .routing_policy_version    = stmt->column_text(9),
        .manifest_digest           = stmt->column_text(10),
        .operator_approved_at      = stmt->column_text(11),
        .population_size           = stmt->column_int64(12),
        .candidate_count           = stmt->column_int64(13),
        .samples                   = stmt->column_int64(14),
        .eligible_samples          = stmt->column_int64(15),
    });
  }
  return out;
}

auto list_outcomes(db::connection& conn, std::int64_t limit) -> std::expected<std::vector<outcome>, views_error> {
  auto stmt = conn.prepare(k_outcomes_sql);
  if (!stmt) {
    return std::unexpected(views_error::query_failed);
  }
  if (!stmt->bind_int64(1, limit)) {
    return std::unexpected(views_error::query_failed);
  }
  std::vector<outcome> out;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(views_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    auto reason_text = stmt->column_text(12);
    out.push_back(outcome{
        .id                           = stmt->column_int64(0),
        .experiment_id                = stmt->column_int64(1),
        .logical_work_item_id         = stmt->column_text(2),
        .role                         = stmt->column_text(3),
        .vendor                       = stmt->column_text(4),
        .candidate                    = stmt->column_text(5),
        .tier                         = stmt->column_text(6),
        .work_type                    = stmt->column_text(7),
        .complexity                   = stmt->column_text(8),
        .terminal_state               = stmt->column_text(9),
        .quality_success              = stmt->column_int64(10) != 0,
        .counts_toward_recommendation = stmt->column_int64(11) != 0,
        .exclusion_reason = reason_text.empty() ? std::optional<std::string>{} : std::optional<std::string>{reason_text},
        .finalized_at     = stmt->column_text(13),
    });
  }
  return out;
}

} // namespace planar::engine::models::views
