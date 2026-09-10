/// @file render.cpp
/// @brief Implementation of `planar.engine.models.render` (plan 996, task
/// 6096). See render.cppm for scope and the oracle-derived output shapes.

module planar.engine.models.render;

import std;
import planar.json_text;
import planar.engine.models.registry;
import planar.engine.models.ranking;
import planar.engine.models.views;

namespace planar::engine::models::render {

// The one shared escape table, layer 1. See json_text.cppm.
using json_text::append_json_string;

namespace {

/// @brief Render a double the way `std.json.Stringify` does: shortest
/// round-trippable, with no forced fractional part.
///
/// NaN is the one case `std::format` alone gets wrong, and it is
/// OPERATOR-REACHABLE: `models evals --quality-floor nan` is accepted by
/// Zig's `parseFloat` and the value is echoed back in the `gates` object.
/// The oracle writes it as the QUOTED string `"nan"` while writing the
/// infinities BARE — `inf` and `-inf`, neither of which is valid JSON
/// either. Captured:
///
///   --quality-floor nan   -> "gates":{...,"quality_floor":"nan"}
///   --quality-floor -nan  -> "gates":{...,"quality_floor":"nan"}   (sign dropped)
///   --quality-floor inf   -> "gates":{...,"quality_floor":inf}
///   --quality-floor -inf  -> "gates":{...,"quality_floor":-inf}
///
/// This asymmetry is not a rule anyone would guess, and it was invisible
/// until `models evals` was wired at layer 3 (plan 996, task 6149) — no
/// engine test reached this function with a non-finite value, because no
/// caller could yet supply one. Reproduced, not "fixed" (D2).
auto json_number(double value) -> std::string {
  if (std::isnan(value)) {
    return "\"nan\"";
  }
  return std::format("{}", value);
}

/// @brief Append `"key":` — a quoted key plus its colon.
auto append_key(std::string& out, std::string_view key) -> void {
  append_json_string(out, key);
  out.push_back(':');
}

/// @brief Append a quoted string field preceded by its key.
auto append_string_field(std::string& out, std::string_view key, std::string_view value) -> void {
  append_key(out, key);
  append_json_string(out, value);
}

/// @brief Append an optional string field, emitting a bare `null` when unset.
auto append_optional_string_field(std::string& out, std::string_view key, const std::optional<std::string>& value) -> void {
  append_key(out, key);
  if (value.has_value()) {
    append_json_string(out, *value);
  } else {
    out.append("null");
  }
}

/// @brief Append an optional double field, emitting a bare `null` when unset.
///
/// `null` rather than `0` is load-bearing: a zero here would claim an
/// unmeasured candidate is instant and free, which is exactly the ordering
/// error the quality floor exists to prevent.
auto append_optional_double_field(std::string& out, std::string_view key, std::optional<double> value) -> void {
  append_key(out, key);
  if (value.has_value()) {
    out.append(json_number(*value));
  } else {
    out.append("null");
  }
}

auto bool_text(bool value) -> std::string_view {
  return value ? std::string_view{"true"} : std::string_view{"false"};
}

auto append_observation(std::string& out, const registry::host_observation& observation) -> void {
  out.push_back('{');
  out.append(std::format("\"id\":{},\"candidate_id\":{},", observation.id, observation.candidate_id));
  append_string_field(out, "host_id", observation.host_id);
  out.append(std::format(",\"observation_version\":{},", observation.observation_version));
  append_string_field(out, "availability", registry::availability_to_text(observation.availability_));
  out.push_back(',');
  append_string_field(out, "spawn_verification", registry::spawn_verification_to_text(observation.spawn_verification_));
  out.push_back(',');
  append_string_field(out, "evidence_ref", observation.evidence_ref);
  out.push_back(',');
  append_string_field(out, "captured_at", observation.captured_at);
  out.push_back(',');
  append_string_field(out, "expires_at", observation.expires_at);
  out.push_back('}');
}

auto append_candidate(std::string& out, const registry::candidate& value) -> void {
  out.append("{\"registration\":{");
  out.append(std::format("\"id\":{},", value.registration_.id));
  append_string_field(out, "vendor", value.registration_.vendor);
  out.push_back(',');
  append_string_field(out, "candidate_id", value.registration_.candidate_id);
  out.append(std::format(",\"enabled\":{},\"fallback_order\":{},\"registration_version\":{},",
                         bool_text(value.registration_.enabled), value.registration_.fallback_order,
                         value.registration_.registration_version));
  append_string_field(out, "compatibility_source", value.registration_.compatibility_source);
  out.append("},\"bindings\":[");
  bool first = true;
  for (const auto& bound : value.bindings) {
    if (!first) {
      out.push_back(',');
    }
    first = false;
    out.append(std::format("{{\"candidate_id\":{},", bound.candidate_id));
    append_string_field(out, "role", bound.role);
    out.push_back(',');
    append_string_field(out, "tier", registry::tier_to_text(bound.tier_));
    out.push_back('}');
  }
  out.append("],\"latest_observation\":");
  if (value.latest_observation.has_value()) {
    append_observation(out, *value.latest_observation);
  } else {
    out.append("null");
  }
  out.push_back('}');
}

/// @brief The shared `{"registry_version":1,"candidates":[...],
/// "migration_warning":"..."}` envelope both registry views emit.
auto registry_envelope(std::span<const registry::candidate> candidates) -> std::string {
  std::string out   = "{\"registry_version\":1,\"candidates\":[";
  bool        first = true;
  for (const auto& value : candidates) {
    if (!first) {
      out.push_back(',');
    }
    first = false;
    append_candidate(out, value);
  }
  out.append("],");
  append_string_field(out, "migration_warning", migration_warning);
  out.append("}\n");
  return out;
}

auto append_ranking_row(std::string& out, const ranking::row& value) -> void {
  out.append(std::format("{{\"candidate_id\":{},", value.candidate_id));
  append_string_field(out, "candidate", value.candidate);
  out.push_back(',');
  append_string_field(out, "vendor", value.vendor);
  out.append(std::format(",\"fallback_order\":{},\"samples\":{},\"successes\":{},\"gate_failures\":{},"
                         "\"excess_attempts\":{},",
                         value.fallback_order, value.samples, value.successes, value.gate_failures, value.excess_attempts));
  append_optional_double_field(out, "mean_latency_ms", value.mean_latency_ms);
  out.push_back(',');
  append_optional_double_field(out, "mean_cost_micros", value.mean_cost_micros);
  out.append(std::format(",\"measured_samples\":{},", value.measured_samples));
  out.append(std::format("\"raw_rate\":{},\"wilson_lower\":{},\"gate_failure_rate\":{},"
                         "\"expected_excess_iterations\":{},",
                         json_number(value.raw_rate), json_number(value.wilson_lower), json_number(value.gate_failure_rate),
                         json_number(value.expected_excess_iterations)));
  out.append(std::format("\"insufficient_data\":{},\"below_quality_floor\":{},\"rank\":", bool_text(value.insufficient_data),
                         bool_text(value.below_quality_floor)));
  if (value.rank.has_value()) {
    out.append(std::format("{}", *value.rank));
  } else {
    out.append("null");
  }
  out.push_back('}');
}

auto append_experiment(std::string& out, const views::experiment& value) -> void {
  out.append(std::format("{{\"id\":{},", value.id));
  append_string_field(out, "experiment_key", value.experiment_key);
  out.push_back(',');
  append_string_field(out, "status", value.status);
  out.push_back(',');
  append_string_field(out, "vendor", value.vendor);
  out.push_back(',');
  append_string_field(out, "role", value.role);
  out.push_back(',');
  append_string_field(out, "tier", value.tier);
  out.push_back(',');
  append_string_field(out, "work_type", value.work_type);
  out.push_back(',');
  append_string_field(out, "complexity", value.complexity);
  out.push_back(',');
  append_string_field(out, "validation_policy_version", value.validation_policy_version);
  out.push_back(',');
  append_string_field(out, "routing_policy_version", value.routing_policy_version);
  out.push_back(',');
  append_string_field(out, "manifest_digest", value.manifest_digest);
  out.push_back(',');
  append_string_field(out, "operator_approved_at", value.operator_approved_at);
  out.append(std::format(",\"population_size\":{},\"candidate_count\":{},\"samples\":{},\"eligible_samples\":{}}}",
                         value.population_size, value.candidate_count, value.samples, value.eligible_samples));
}

auto append_outcome(std::string& out, const views::outcome& value) -> void {
  out.append(std::format("{{\"id\":{},\"experiment_id\":{},", value.id, value.experiment_id));
  append_string_field(out, "logical_work_item_id", value.logical_work_item_id);
  out.push_back(',');
  append_string_field(out, "role", value.role);
  out.push_back(',');
  append_string_field(out, "vendor", value.vendor);
  out.push_back(',');
  append_string_field(out, "candidate", value.candidate);
  out.push_back(',');
  append_string_field(out, "tier", value.tier);
  out.push_back(',');
  append_string_field(out, "work_type", value.work_type);
  out.push_back(',');
  append_string_field(out, "complexity", value.complexity);
  out.push_back(',');
  append_string_field(out, "terminal_state", value.terminal_state);
  out.append(std::format(",\"quality_success\":{},\"counts_toward_recommendation\":{},", bool_text(value.quality_success),
                         bool_text(value.counts_toward_recommendation)));
  append_optional_string_field(out, "exclusion_reason", value.exclusion_reason);
  out.push_back(',');
  append_string_field(out, "finalized_at", value.finalized_at);
  out.push_back('}');
}

/// @brief Left-align `text` in `width` columns, matching zig's `{s: <N}`.
///
/// Zig's width specifier does not TRUNCATE an over-long value, and neither
/// does this: a 40-character candidate identifier pushes the rest of the row
/// right rather than being silently cut, in both implementations.
auto pad_left_align(std::string_view text, std::size_t width) -> std::string {
  std::string out(text);
  while (out.size() < width) {
    out.push_back(' ');
  }
  return out;
}

/// @brief Right-align `text` in `width` columns, matching zig's `{s: >N}`.
auto pad_right_align(std::string_view text, std::size_t width) -> std::string {
  std::string out;
  while (out.size() + text.size() < width) {
    out.push_back(' ');
  }
  out.append(text);
  return out;
}

} // namespace

auto registry_json(std::span<const registry::candidate> candidates) -> std::string {
  return registry_envelope(candidates);
}

auto registry_export_stderr_warning() -> std::string {
  return std::format("warning: {}\n", migration_warning);
}

auto registry_text(std::span<const registry::candidate> candidates) -> std::string {
  std::string out;
  for (const auto& value : candidates) {
    out.append(std::format("{} {} {} enabled={} order={} bindings={} observation={}\n", value.registration_.id,
                           value.registration_.vendor, value.registration_.candidate_id, bool_text(value.registration_.enabled),
                           value.registration_.fallback_order, value.bindings.size(),
                           value.latest_observation.has_value() ? "present" : "none"));
  }
  return out;
}

auto id_line(std::int64_t id) -> std::string {
  return std::format("{}\n", id);
}

auto eligibility_json(std::int64_t candidate_id, std::string_view host_id, const registry::eligibility& gates) -> std::string {
  std::string out = std::format("{{\"candidate\":{},", candidate_id);
  append_string_field(out, "host", host_id);
  out.append(std::format(",\"eligible\":{},\"gates\":{{", bool_text(gates.eligible())));
  out.append(std::format("\"cli_available\":{},\"exact_spawn_verified\":{},\"role_tier_bound\":{},"
                         "\"role_surface_override_supported\":{},\"host_policy_permits\":{},"
                         "\"observation_fresh\":{}}}",
                         bool_text(gates.cli_available), bool_text(gates.exact_spawn_verified), bool_text(gates.role_tier_bound),
                         bool_text(gates.role_surface_override_supported), bool_text(gates.host_policy_permits),
                         bool_text(gates.observation_fresh)));
  out.append(",\"reasons\":[");
  bool first = true;
  for (const auto reason : registry::reasons(gates)) {
    if (!first) {
      out.push_back(',');
    }
    first = false;
    append_json_string(out, registry::eligibility_reason_to_text(reason));
  }
  out.append("]}\n");
  return out;
}

auto verify_identity_json(std::int64_t candidate_id, registry::identity_verification outcome) -> std::string {
  return std::format("{{\"candidate\":{},\"identity\":\"{}\"}}\n", candidate_id,
                     registry::identity_verification_to_text(outcome));
}

auto evals_json(const ranking::result& outcome, const ranking::gates& gate_config) -> std::string {
  std::string out   = std::format("{{\"version\":\"{}\",\"evidence\":\"declared_experiment\","
                                  "\"gates\":{{\"minimum_samples\":{},\"quality_floor\":{}}},\"rows\":[",
                                  ranking::ranking_version, gate_config.minimum_samples, json_number(gate_config.quality_floor));
  bool        first = true;
  for (const auto& value : outcome.rows) {
    if (!first) {
      out.push_back(',');
    }
    first = false;
    append_ranking_row(out, value);
  }
  out.append("],");
  append_optional_string_field(out, "recommended", outcome.recommended);
  out.push_back(',');
  append_optional_string_field(out, "no_recommendation_reason", outcome.no_recommendation_reason);
  out.append("}\n");
  return out;
}

auto evals_text(const ranking::result& outcome, const ranking::gates& gate_config) -> std::string {
  std::string out =
      std::format("routing evidence ranking ({}) — declared-experiment samples only, read-only:\n", ranking::ranking_version);
  out.append(std::format("  gates: minimum_samples={} quality_floor={:.2f}\n\n", gate_config.minimum_samples,
                         gate_config.quality_floor));
  if (outcome.rows.empty()) {
    // NOTE the early return: no table header and no recommendation footer.
    out.append("  (no cohort-eligible declared-experiment samples)\n");
    return out;
  }
  out.append(std::format("  {} {} {} {} {} {} {} {}\n", pad_left_align("rank", 4), pad_left_align("candidate", 28),
                         pad_right_align("samples", 7), pad_right_align("success", 8), pad_right_align("raw", 8),
                         pad_right_align("wilson", 8), pad_right_align("gatefail", 9), pad_right_align("excess", 7)));
  for (const auto& value : outcome.rows) {
    // A gated row gets `n/a` (under-sampled) or `--` (below the floor)
    // instead of a number: printing a rank next to a candidate we just said
    // we cannot judge would read as an endorsement.
    std::string rank_text = "--";
    if (value.rank.has_value()) {
      rank_text = std::format("{}", *value.rank);
    } else if (value.insufficient_data) {
      rank_text = "n/a";
    }
    out.append(std::format(
        "  {} {} {} {} {} {} {} {}", pad_left_align(rank_text, 4), pad_left_align(value.candidate, 28),
        pad_right_align(std::format("{}", value.samples), 7), pad_right_align(std::format("{}", value.successes), 8),
        pad_right_align(std::format("{:.3f}", value.raw_rate), 8), pad_right_align(std::format("{:.3f}", value.wilson_lower), 8),
        pad_right_align(std::format("{:.3f}", value.gate_failure_rate), 9),
        pad_right_align(std::format("{:.2f}", value.expected_excess_iterations), 7)));
    if (value.insufficient_data) {
      out.append("  insufficient_data");
    } else if (value.below_quality_floor) {
      out.append("  below_quality_floor");
    }
    // Say "unmeasured" rather than printing nothing: a blank column would
    // read as zero, and zero here means instant and free.
    if (value.mean_latency_ms.has_value()) {
      out.append(std::format("  {:.0f}ms", *value.mean_latency_ms));
    } else {
      out.append("  latency:unmeasured");
    }
    out.push_back('\n');
  }
  if (outcome.recommended.has_value()) {
    out.append(std::format("\nrecommended: {} (preview only; writes nothing)\n", *outcome.recommended));
  } else {
    out.append(std::format("\nno recommendation: {}\n", outcome.no_recommendation_reason.value_or(std::string{"gated"})));
  }
  return out;
}

auto experiments_json(std::span<const views::experiment> experiments) -> std::string {
  std::string out   = std::format("{{\"views_version\":\"{}\",\"experiments\":[", views::views_version);
  bool        first = true;
  for (const auto& value : experiments) {
    if (!first) {
      out.push_back(',');
    }
    first = false;
    append_experiment(out, value);
  }
  out.append("]}\n");
  return out;
}

auto experiments_text(std::span<const views::experiment> experiments) -> std::string {
  if (experiments.empty()) {
    return "no declared routing experiments\n";
  }
  std::string out = std::format("routing experiments ({}) — read-only:\n\n", views::views_version);
  for (const auto& value : experiments) {
    out.append(std::format("  [{}] {}  status={}\n", value.id, value.experiment_key, value.status));
    out.append(std::format("       cohort: {}/{}/{}/{}/{}  policies: {} + {}\n", value.vendor, value.role, value.tier,
                           value.work_type, value.complexity, value.validation_policy_version, value.routing_policy_version));
    out.append(std::format("       manifest: {} approved {}  population={} candidates={}\n", value.manifest_digest,
                           value.operator_approved_at, value.population_size, value.candidate_count));
    // The excluded count is shown as a DIFFERENCE rather than as its own
    // column so "recorded" and "counted" stay adjacent and comparable.
    out.append(std::format("       samples: {} recorded, {} counted ({} excluded)\n\n", value.samples, value.eligible_samples,
                           value.samples - value.eligible_samples));
  }
  return out;
}

auto outcomes_json(std::span<const views::outcome> outcomes) -> std::string {
  std::string out   = std::format("{{\"views_version\":\"{}\",\"outcomes\":[", views::views_version);
  bool        first = true;
  for (const auto& value : outcomes) {
    if (!first) {
      out.push_back(',');
    }
    first = false;
    append_outcome(out, value);
  }
  out.append("]}\n");
  return out;
}

auto outcomes_text(std::span<const views::outcome> outcomes) -> std::string {
  if (outcomes.empty()) {
    return "no recorded terminal outcomes\n";
  }
  std::string out = std::format("terminal outcomes ({}) — read-only:\n\n", views::views_version);
  for (const auto& value : outcomes) {
    out.append(std::format("  [{}] {}  {}  {}\n", value.id, value.candidate, value.terminal_state, value.logical_work_item_id));
    if (value.counts_toward_recommendation) {
      out.append(
          std::format("       counts toward recommendation (quality_success={})\n\n", value.quality_success ? "yes" : "no"));
    } else {
      // Never print "excluded" without saying why: an unexplained exclusion
      // is indistinguishable from a bug.
      out.append(
          std::format("       EXCLUDED from recommendations: {}\n\n", value.exclusion_reason.value_or(std::string{"unknown"})));
    }
  }
  return out;
}

} // namespace planar::engine::models::render
