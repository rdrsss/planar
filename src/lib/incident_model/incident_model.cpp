/// @file incident_model.cpp
/// @brief Implementation of `planar.incident_model` (see incident_model.cppm).

module;

module planar.incident_model;

import std;
import planar.sha256;

namespace planar::incident_model {

namespace {

constexpr auto k_statuses = std::to_array<incident_status>(
    {incident_status::new_incident, incident_status::recurring, incident_status::escalated, incident_status::reopened,
     incident_status::dismissed, incident_status::resolved, incident_status::not_recorded});

} // namespace

auto severity_ordinal(severity s) noexcept -> int {
  switch (s) {
  case severity::info:
    return 0;
  case severity::low:
    return 1;
  case severity::medium:
    return 2;
  case severity::high:
    return 3;
  case severity::critical:
    return 4;
  }
  return 0;
}

auto severity_name(severity s) noexcept -> std::string_view {
  switch (s) {
  case severity::info:
    return "info";
  case severity::low:
    return "low";
  case severity::medium:
    return "medium";
  case severity::high:
    return "high";
  case severity::critical:
    return "critical";
  }
  return "info";
}

auto parse_severity(std::string_view text) noexcept -> std::optional<severity> {
  for (auto s : {severity::info, severity::low, severity::medium, severity::high, severity::critical}) {
    if (severity_name(s) == text) {
      return s;
    }
  }
  return std::nullopt;
}

auto incident_severity(diagnostic_severity s) noexcept -> severity {
  switch (s) {
  case diagnostic_severity::info:
    return severity::info;
  case diagnostic_severity::warning:
    return severity::medium;
  case diagnostic_severity::error:
    return severity::high;
  }
  return severity::info;
}

auto diagnostic_severity_name(diagnostic_severity s) noexcept -> std::string_view {
  switch (s) {
  case diagnostic_severity::info:
    return "info";
  case diagnostic_severity::warning:
    return "warning";
  case diagnostic_severity::error:
    return "error";
  }
  return "info";
}

auto parse_diagnostic_severity(std::string_view text) noexcept -> std::optional<diagnostic_severity> {
  for (auto s : {diagnostic_severity::info, diagnostic_severity::warning, diagnostic_severity::error}) {
    if (diagnostic_severity_name(s) == text) {
      return s;
    }
  }
  return std::nullopt;
}

auto compare_severity(severity a, severity b) noexcept -> std::strong_ordering {
  return severity_ordinal(a) <=> severity_ordinal(b);
}

auto max_severity(severity a, severity b) noexcept -> severity {
  return compare_severity(a, b) == std::strong_ordering::less ? b : a;
}

auto is_recorded(diagnostic_severity s) noexcept -> bool {
  return s != diagnostic_severity::info;
}

auto check_kind_name(check_kind k) noexcept -> std::string_view {
  return k == check_kind::state ? "state" : "event";
}

auto parse_check_kind(std::string_view text) noexcept -> std::optional<check_kind> {
  if (text == "state") {
    return check_kind::state;
  }
  if (text == "event") {
    return check_kind::event;
  }
  return std::nullopt;
}

auto incident_state_name(incident_state s) noexcept -> std::string_view {
  switch (s) {
  case incident_state::open:
    return "open";
  case incident_state::acknowledged:
    return "acknowledged";
  case incident_state::reopened:
    return "reopened";
  case incident_state::resolved:
    return "resolved";
  case incident_state::dismissed:
    return "dismissed";
  }
  return "open";
}

auto parse_incident_state(std::string_view text) noexcept -> std::optional<incident_state> {
  for (auto s : {incident_state::open, incident_state::acknowledged, incident_state::reopened, incident_state::resolved,
                 incident_state::dismissed}) {
    if (incident_state_name(s) == text) {
      return s;
    }
  }
  return std::nullopt;
}

auto incident_status_name(incident_status s) noexcept -> std::string_view {
  switch (s) {
  case incident_status::new_incident:
    return "new";
  case incident_status::recurring:
    return "recurring";
  case incident_status::escalated:
    return "escalated";
  case incident_status::reopened:
    return "reopened";
  case incident_status::dismissed:
    return "dismissed";
  case incident_status::resolved:
    return "resolved";
  case incident_status::not_recorded:
    return "not-recorded";
  }
  return "not-recorded";
}

auto parse_incident_status(std::string_view text) noexcept -> std::optional<incident_status> {
  for (auto s : k_statuses) {
    if (incident_status_name(s) == text) {
      return s;
    }
  }
  return std::nullopt;
}

auto all_incident_statuses() noexcept -> std::span<const incident_status> {
  return k_statuses;
}

auto coverage_state_name(coverage_state s) noexcept -> std::string_view {
  switch (s) {
  case coverage_state::observed:
    return "observed";
  case coverage_state::unavailable:
    return "unavailable";
  case coverage_state::disabled:
    return "disabled";
  case coverage_state::not_applicable:
    return "not_applicable";
  }
  return "not_applicable";
}

auto incident_source_name(incident_source s) noexcept -> std::string_view {
  switch (s) {
  case incident_source::diagnose_check:
    return "diagnose_check";
  case incident_source::claim_failure:
    return "claim_failure";
  case incident_source::cli_failure:
    return "cli_failure";
  case incident_source::conversational:
    return "conversational";
  }
  return "diagnose_check";
}

auto parse_incident_source(std::string_view text) noexcept -> std::optional<incident_source> {
  for (auto s : {incident_source::diagnose_check, incident_source::claim_failure, incident_source::cli_failure,
                 incident_source::conversational}) {
    if (incident_source_name(s) == text) {
      return s;
    }
  }
  return std::nullopt;
}

auto entity_ref_text(const entity_ref& ref) -> std::string {
  return std::format("{}:{}", ref.kind, ref.id);
}

auto parse_entity_ref(std::string_view text) -> std::optional<entity_ref> {
  auto colon = text.find(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) {
    return std::nullopt;
  }
  auto kind = text.substr(0, colon);
  if (!std::ranges::all_of(kind, [](char c) { return (c >= 'a' && c <= 'z') || c == '_'; })) {
    return std::nullopt;
  }
  auto         digits = text.substr(colon + 1);
  std::int64_t id     = 0;
  auto [ptr, ec]      = std::from_chars(digits.data(), digits.data() + digits.size(), id);
  if (ec != std::errc{} || ptr != digits.data() + digits.size() || id < 0) {
    return std::nullopt;
  }
  return entity_ref{.kind = std::string{kind}, .id = id};
}

auto compare_entity_refs(const entity_ref& a, const entity_ref& b) -> std::strong_ordering {
  if (auto c = a.kind <=> b.kind; c != 0) {
    return c;
  }
  return a.id <=> b.id;
}

auto fingerprint(std::string_view check_id, std::span<const entity_ref> refs) -> std::string {
  std::vector<entity_ref> sorted{refs.begin(), refs.end()};
  std::ranges::sort(sorted, [](const entity_ref& a, const entity_ref& b) { return compare_entity_refs(a, b) < 0; });
  sorted.erase(std::ranges::unique(sorted).begin(), sorted.end());
  std::string out{check_id};
  out += '|';
  bool first = true;
  for (const auto& ref : sorted) {
    if (!first) {
      out += ',';
    }
    first = false;
    out += entity_ref_text(ref);
  }
  return out;
}

auto cluster_fingerprint(std::string_view check_id, const grouping& group) -> std::string {
  std::string out{check_id};
  for (const auto& part : group.key_parts) {
    out += '|';
    out += part;
  }
  out += '|';
  out += group.scope;
  return out;
}

auto member_digest(std::string_view fingerprint_text, const cluster_member& member) -> std::string {
  std::string material{fingerprint_text};
  material += '\n';
  material += entity_ref_text(member.ref);
  material += '\n';
  material += member.time;
  return sha256::hex(material);
}

auto finding_fingerprint(const finding& f) -> std::string {
  if (f.group) {
    return cluster_fingerprint(f.check_id, *f.group);
  }
  return fingerprint(f.check_id, f.evidence);
}

auto evidence_digest(std::string_view fingerprint_text, std::span<const std::string> times) -> std::string {
  std::string material{fingerprint_text};
  for (const auto& t : times) {
    material += '\n';
    material += t;
  }
  return sha256::hex(material);
}

auto finding_digest(const finding& f) -> std::string {
  return evidence_digest(finding_fingerprint(f), f.evidence_times);
}

auto earliest_evidence(const finding& f) -> std::string {
  std::vector<std::string> all = f.evidence_times;
  for (const auto& m : f.members) {
    all.push_back(m.time);
  }
  if (all.empty()) {
    return {};
  }
  return *std::ranges::min_element(all);
}

auto compare_findings(const finding& a, const finding& b) -> std::strong_ordering {
  // Highest severity first: compare b against a.
  if (auto c = compare_severity(incident_severity(b.severity), incident_severity(a.severity)); c != 0) {
    return c;
  }
  if (auto c = a.check_id <=> b.check_id; c != 0) {
    return c;
  }
  if (auto c = compare_entity_refs(a.primary, b.primary); c != 0) {
    return c;
  }
  if (auto c = earliest_evidence(a) <=> earliest_evidence(b); c != 0) {
    return c;
  }
  return finding_fingerprint(a) <=> finding_fingerprint(b);
}

auto sort_findings(std::vector<finding>& findings) -> void {
  std::ranges::stable_sort(findings, [](const finding& a, const finding& b) { return compare_findings(a, b) < 0; });
}

} // namespace planar::incident_model
