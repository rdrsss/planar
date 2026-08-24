/// @file legacy.cpp
/// @brief Implementation of `planar.engine.models.legacy` (plan 996, task
/// 6111). See legacy.cppm for the note convention, the three signal sources,
/// and the two dead branches this port keeps for wire compatibility.

module;

#include <glaze/glaze.hpp>

module planar.engine.models.legacy;

import std;
import planar.db;
import planar.json_text;

namespace planar::engine::models::legacy {

using planar::json_text::append_json_string;

namespace {

constexpr std::string_view k_choice_key = "model_choice:";

auto trim(std::string_view text, std::string_view chars) -> std::string_view {
  const auto first = text.find_first_not_of(chars);
  if (first == std::string_view::npos) {
    return {};
  }
  return text.substr(first, text.find_last_not_of(chars) - first + 1);
}

/// @brief Render a double the way `std.json.Stringify` does: shortest
/// round-trippable, with no forced fractional part.
///
/// The consequence is visible in the oracle capture: an approval rate of 1.0
/// serialises as `1`, not `1.0`, while 0.5 stays `0.5`. Note that the
/// RATIONALE string uses `{:.2}` and therefore always shows two decimals, so
/// the same value appears as `1` in the JSON number and `1.00` in the prose
/// beside it. That disagreement is the oracle's and is deliberate.
auto json_number(double value) -> std::string {
  return std::format("{}", value);
}

/// @brief Left-pad to `width`; never truncates.
auto pad(std::string_view value, std::size_t width) -> std::string {
  std::string out{value};
  if (out.size() < width) {
    out.append(width - out.size(), ' ');
  }
  return out;
}

enum class claim_outcome { approved, aborted, other, none };
enum class test_coder_outcome { ok, other };

/// @brief The most recent claim's terminal disposition for a task.
///
/// `completed` is the recoverable proxy for a reviewer approval; `request-
/// changes` is NOT a distinct terminal state anywhere in the schema (the claim
/// persists across loop-back iterations), which is exactly why iteration count
/// is tracked separately.
auto resolve_claim_outcome(db::connection& conn, std::int64_t task_id) -> std::optional<claim_outcome> {
  auto stmt = conn.prepare("select status from agent_work_claims "
                           "where entity_kind = 'task' and entity_id = ? "
                           "order by claimed_at desc, id desc limit 1");
  if (!stmt || !stmt->bind_int64(1, task_id)) {
    return std::nullopt;
  }
  const auto step = stmt->step();
  if (!step) {
    return std::nullopt;
  }
  if (*step == db::step_result::done) {
    return claim_outcome::none;
  }
  const auto status = stmt->column_text(0);
  if (status == "completed") {
    return claim_outcome::approved;
  }
  if (status == "aborted") {
    return claim_outcome::aborted;
  }
  return claim_outcome::other;
}

/// @brief The most recent test-coder action's outcome for a task.
///
/// Returns nullopt for BOTH "no row" and "row with a NULL outcome", and in
/// either case the task contributes to NEITHER counter — which is why
/// `test_coder_ok_count + test_coder_other_count` need not equal
/// `dispatch_count`.
auto resolve_test_coder_outcome(db::connection& conn, std::int64_t task_id)
    -> std::expected<std::optional<test_coder_outcome>, std::monostate> {
  auto stmt = conn.prepare("select outcome from agent_actions "
                           "where action_kind = 'test_coder' and entity_kind = 'task' and entity_id = ? "
                           "order by started_at desc, id desc limit 1");
  if (!stmt || !stmt->bind_int64(1, task_id)) {
    return std::unexpected(std::monostate{});
  }
  const auto step = stmt->step();
  if (!step) {
    return std::unexpected(std::monostate{});
  }
  if (*step == db::step_result::done || stmt->is_null(0)) {
    return std::optional<test_coder_outcome>{};
  }
  return stmt->column_text(0) == "ok" ? std::optional{test_coder_outcome::ok} : std::optional{test_coder_outcome::other};
}

/// @brief The vendor an agent RECORDED for this candidate string.
///
/// Never inferred from a catalog: plan 950 removed Planar's model catalog
/// precisely so it would stop making support claims about which vendor serves
/// which model. Null when no claim carries the string.
auto vendor_for_candidate(db::connection& conn, std::string_view candidate) -> std::optional<std::string> {
  auto stmt = conn.prepare("select vendor from agent_work_claims "
                           "where model = ? order by claimed_at desc, id desc limit 1");
  if (!stmt || !stmt->bind_text(1, candidate)) {
    return std::nullopt;
  }
  const auto step = stmt->step();
  if (!step || *step == db::step_result::done || stmt->is_null(0)) {
    return std::nullopt;
  }
  return stmt->column_text(0);
}

/// @brief One accumulating (work_type, candidate) group.
struct group_accum {
  std::string work_type;
  std::string candidate;
  std::string tier; ///< From the FIRST task folded in; later tasks do not change it.
  std::size_t dispatch_count   = 0;
  std::size_t approved_count   = 0;
  std::size_t aborted_count    = 0;
  std::size_t other_count      = 0;
  std::size_t total_iterations = 0;
  std::size_t test_coder_ok    = 0;
  std::size_t test_coder_other = 0;
};

} // namespace

auto process_dispatch_body(std::string_view body, std::map<std::int64_t, task_info>& tasks) -> void {
  std::size_t pos = 0;
  while (pos <= body.size()) {
    const auto next = body.find('\n', pos);
    const auto seg  = body.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos);
    const auto line = trim(seg, " \t\r");
    pos             = next == std::string_view::npos ? body.size() + 1 : next + 1;

    if (!line.starts_with(k_choice_key)) {
      continue;
    }
    const auto json_part = trim(line.substr(k_choice_key.size()), " \t");
    if (json_part.empty()) {
      return;
    }

    glz::generic parsed{};
    if (glz::read_json(parsed, json_part)) {
      return; // Malformed JSON is a silent skip, never an error.
    }
    if (!parsed.is_object()) {
      return;
    }

    for (const auto& [key, value] : parsed.get_object()) {
      std::int64_t task_id = 0;
      const auto*  first   = key.data();
      const auto*  last    = key.data() + key.size();
      const auto   conv    = std::from_chars(first, last, task_id);
      if (conv.ec != std::errc{} || conv.ptr != last) {
        continue;
      }

      // Incremented BEFORE the triple is validated: the dispatch happened even
      // when its classification is unusable, so it still counts as an
      // iteration. This is what makes a two-note task report avg 1.5 rather
      // than 1.0 when only one note was complete.
      auto& info = tasks[task_id];
      ++info.iterations;

      if (!value.is_object()) {
        continue;
      }
      const auto& obj       = value.get_object();
      const auto  as_string = [&obj](std::string_view name) -> std::optional<std::string> {
        const auto it = obj.find(std::string{name});
        if (it == obj.end() || !it->second.is_string()) {
          return std::nullopt;
        }
        return it->second.get_string();
      };
      const auto work_type = as_string("work_type");
      const auto candidate = as_string("candidate");
      const auto tier      = as_string("tier");
      if (!work_type || !candidate || !tier) {
        continue;
      }
      if (work_type->empty() || candidate->empty() || tier->empty()) {
        continue;
      }

      // Last complete note wins.
      info.work_type = *work_type;
      info.candidate = *candidate;
      info.tier      = *tier;
      info.has_info  = true;
    }
    return; // Only the FIRST model_choice line in a body is ever read.
  }
}

auto aggregate(db::connection& conn) -> std::optional<result> {
  std::map<std::int64_t, task_info> tasks;
  {
    auto stmt = conn.prepare("select body from session_entries "
                             "where prefix = 'note' and body like '%dispatch_shape:%' "
                             "order by id");
    if (!stmt) {
      return std::nullopt;
    }
    while (true) {
      const auto step = stmt->step();
      if (!step) {
        return std::nullopt;
      }
      if (*step == db::step_result::done) {
        break;
      }
      process_dispatch_body(stmt->column_text(0), tasks);
    }
  }

  std::map<std::string, group_accum> groups;
  result                             out;

  for (const auto& [task_id, info] : tasks) {
    if (!info.has_info) {
      // Counts TASKS, not notes, despite the field's name.
      ++out.legacy_dispatch_notes_skipped;
      continue;
    }
    const auto claim = resolve_claim_outcome(conn, task_id);
    if (!claim) {
      return std::nullopt;
    }
    const auto test_coder = resolve_test_coder_outcome(conn, task_id);
    if (!test_coder) {
      return std::nullopt;
    }

    // The NUL separator is the Zig original's, and it matters: a candidate
    // containing the separator could otherwise collide with a different
    // (work_type, candidate) pair, and candidate ids are opaque operator
    // strings.
    std::string key = info.work_type;
    key.push_back('\0');
    key.append(info.candidate);
    auto [it, inserted] = groups.try_emplace(key, group_accum{info.work_type, info.candidate, info.tier});
    auto& group         = it->second;

    ++group.dispatch_count;
    group.total_iterations += info.iterations;
    switch (*claim) {
    case claim_outcome::approved:
      ++group.approved_count;
      break;
    case claim_outcome::aborted:
      ++group.aborted_count;
      break;
    case claim_outcome::other:
    case claim_outcome::none:
      // NO CLAIM folds into `other`, not into its own bucket. An undispatched
      // task is not a success and must not raise the approval rate.
      ++group.other_count;
      break;
    }
    if (test_coder->has_value()) {
      if (**test_coder == test_coder_outcome::ok) {
        ++group.test_coder_ok;
      } else {
        ++group.test_coder_other;
      }
    }
  }

  for (const auto& [key, group] : groups) {
    const auto denominator = static_cast<double>(group.dispatch_count);
    out.scorecard.push_back({group.work_type, group.candidate, vendor_for_candidate(conn, group.candidate), group.tier,
                             group.dispatch_count, group.approved_count, group.aborted_count, group.other_count,
                             static_cast<double>(group.approved_count) / denominator,
                             static_cast<double>(group.total_iterations) / denominator, group.test_coder_ok,
                             group.test_coder_other, false, std::nullopt});
  }

  std::ranges::sort(out.scorecard, [](const score_row& lhs, const score_row& rhs) {
    if (lhs.work_type != rhs.work_type) {
      return lhs.work_type < rhs.work_type;
    }
    if (lhs.insufficient_data != rhs.insufficient_data) {
      return !lhs.insufficient_data;
    }
    if (!lhs.insufficient_data) {
      // Approval rate DESCENDING, then iterations ASCENDING. Correctness first;
      // speed is only ever a tie-break among equally-approved candidates.
      if (lhs.approval_rate != rhs.approval_rate) {
        return lhs.approval_rate > rhs.approval_rate;
      }
      if (lhs.avg_iterations != rhs.avg_iterations) {
        return lhs.avg_iterations < rhs.avg_iterations;
      }
    }
    // The final key makes the order TOTAL, which is what makes two runs over
    // the same database byte-identical despite the map accumulation above.
    return lhs.candidate < rhs.candidate;
  });

  {
    std::optional<std::string> current;
    std::size_t                counter = 0;
    for (auto& row : out.scorecard) {
      if (!current || *current != row.work_type) {
        current = row.work_type;
        counter = 0;
      }
      if (row.insufficient_data) {
        row.rank.reset();
      } else {
        row.rank = ++counter;
      }
    }
  }

  for (const auto& row : out.scorecard) {
    if (row.rank != 1) {
      continue;
    }
    out.recommendations.push_back(
        {row.work_type, row.vendor, row.tier, row.candidate,
         std::format("{}/{} approved, avg {:.2f} iterations to approval (highest-ranked scored candidate "
                     "for {})",
                     row.approved_count, row.dispatch_count, row.avg_iterations, row.work_type)});
  }
  return out;
}

namespace {

auto append_key(std::string& out, std::string_view key) -> void {
  append_json_string(out, key);
  out.push_back(':');
}

auto append_optional_string(std::string& out, const std::optional<std::string>& value) -> void {
  if (value) {
    append_json_string(out, *value);
  } else {
    out.append("null");
  }
}

} // namespace

auto evals_json(const result& value) -> std::string {
  std::string out = "{\"scorecard\":[";
  for (std::size_t i = 0; i < value.scorecard.size(); ++i) {
    const auto& row = value.scorecard[i];
    if (i != 0) {
      out.push_back(',');
    }
    out.push_back('{');
    append_key(out, "work_type");
    append_json_string(out, row.work_type);
    out.push_back(',');
    append_key(out, "candidate");
    append_json_string(out, row.candidate);
    out.push_back(',');
    append_key(out, "vendor");
    append_optional_string(out, row.vendor);
    out.push_back(',');
    append_key(out, "tier");
    append_json_string(out, row.tier);
    out.append(std::format(",\"dispatch_count\":{},\"approved_count\":{},\"aborted_count\":{},"
                           "\"other_count\":{}",
                           row.dispatch_count, row.approved_count, row.aborted_count, row.other_count));
    out.append(std::format(",\"approval_rate\":{},\"avg_iterations\":{}", json_number(row.approval_rate),
                           json_number(row.avg_iterations)));
    out.append(std::format(",\"test_coder_ok_count\":{},\"test_coder_other_count\":{}", row.test_coder_ok_count,
                           row.test_coder_other_count));
    out.append(std::format(",\"insufficient_data\":{}", row.insufficient_data ? "true" : "false"));
    out.append(",\"rank\":");
    if (row.rank) {
      out.append(std::format("{}", *row.rank));
    } else {
      out.append("null");
    }
    out.push_back('}');
  }
  out.append("],\"recommendations\":[");
  for (std::size_t i = 0; i < value.recommendations.size(); ++i) {
    const auto& rec = value.recommendations[i];
    if (i != 0) {
      out.push_back(',');
    }
    out.push_back('{');
    append_key(out, "work_type");
    append_json_string(out, rec.work_type);
    out.push_back(',');
    append_key(out, "vendor");
    append_optional_string(out, rec.vendor);
    out.push_back(',');
    append_key(out, "tier");
    append_json_string(out, rec.tier);
    out.push_back(',');
    append_key(out, "candidate");
    append_json_string(out, rec.candidate);
    out.push_back(',');
    append_key(out, "rationale");
    append_json_string(out, rec.rationale);
    out.push_back('}');
  }
  out.append(std::format("],\"signals_sourced\":{{\"reviewer_disposition\":{},\"iteration_count\":{},"
                         "\"quality_gate_pass_fail\":{},\"test_coder_expansion\":{}}}",
                         value.signals_sourced.reviewer_disposition ? "true" : "false",
                         value.signals_sourced.iteration_count ? "true" : "false",
                         value.signals_sourced.quality_gate_pass_fail ? "true" : "false",
                         value.signals_sourced.test_coder_expansion ? "true" : "false"));
  out.append(std::format(",\"legacy_dispatch_notes_skipped\":{}}}\n", value.legacy_dispatch_notes_skipped));
  return out;
}

auto evals_text(const result& value) -> std::string {
  // Every dash below is U+2014 EM DASH and every arrow U+2192; they are in the
  // oracle's bytes, not decoration this port added.
  std::string out = "routing evals scorecard (per work-type, candidate) — read-only, writes nothing:\n";
  if (value.scorecard.empty()) {
    out.append("  (no completed dispatch history recorded yet)\n");
  }
  for (const auto& row : value.scorecard) {
    const auto vendor = row.vendor.value_or("?");
    if (row.insufficient_data) {
      // Unreachable through the CLI today — sibling enumeration was deleted
      // with the model catalog, so no row is ever flagged. Kept because it is
      // part of the wire contract, and driven directly by a test.
      out.append(
          std::format("  {} {} insufficient-data  [{}/{}]\n", pad(row.work_type, 14), pad(row.candidate, 24), vendor, row.tier));
      continue;
    }
    out.append(std::format("  {} {} rank {} {}/{} approved  avg {:.2f} iter  [{}/{}]\n", pad(row.work_type, 14),
                           pad(row.candidate, 24), pad(std::format("{}", *row.rank), 2), row.approved_count, row.dispatch_count,
                           row.avg_iterations, vendor, row.tier));
  }

  out.append("\nrecommendations (preview only — writes nothing; apply is a separate operator-gated "
             "step):\n");
  if (value.recommendations.empty()) {
    out.append("  (none — no work type has scored dispatch history yet)\n");
  }
  for (const auto& rec : value.recommendations) {
    out.append(
        std::format("  {} → {} [{}/{}]: {}\n", rec.work_type, rec.candidate, rec.vendor.value_or("?"), rec.tier, rec.rationale));
  }

  out.append(std::format(
      "\nsignals sourced: reviewer_disposition={} iteration_count={} "
      "quality_gate_pass_fail={} test_coder_expansion={}\n",
      value.signals_sourced.reviewer_disposition ? "yes" : "no", value.signals_sourced.iteration_count ? "yes" : "no",
      value.signals_sourced.quality_gate_pass_fail ? "yes" : "no", value.signals_sourced.test_coder_expansion ? "yes" : "no"));
  if (value.legacy_dispatch_notes_skipped > 0) {
    out.append(std::format("note: {} dispatch note(s) skipped — missing/malformed model_choice work_type\n",
                           value.legacy_dispatch_notes_skipped));
  }
  return out;
}

auto cohort_requires_project_error() -> std::string {
  return "error: --project is required when ranking a cohort\n";
}

} // namespace planar::engine::models::legacy
