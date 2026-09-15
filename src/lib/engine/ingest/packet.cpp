/// @file packet.cpp
/// @brief Implementation of `planar.engine.ingest.packet` (plan 996, task
/// 6324). See packet.cppm for the bucket-placement rationale, the reason
/// ordering contract, and the oracle facts that are easy to assume wrongly.

module planar.engine.ingest.packet;

import std;
import planar.db;
import planar.json_text;
import planar.engine.ingest.materialize;

namespace planar::engine::ingest::packet {

namespace mz = planar::engine::ingest::materialize;
using json_text::append_json_string;

namespace {

constexpr std::string_view whitespace = " \t\r\n";

[[nodiscard]] auto trim(std::string_view s) -> std::string_view {
  const auto first = s.find_first_not_of(whitespace);
  if (first == std::string_view::npos) {
    return {};
  }
  return s.substr(first, s.find_last_not_of(whitespace) - first + 1);
}

[[nodiscard]] auto trim_end_cr(std::string_view s) -> std::string_view {
  const auto last = s.find_last_not_of('\r');
  return last == std::string_view::npos ? std::string_view{} : s.substr(0, last + 1);
}

[[nodiscard]] auto lower(char c) -> char {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

[[nodiscard]] auto equals_fold(std::string_view a, std::string_view b) -> bool {
  if (a.size() != b.size()) {
    return false;
  }
  return std::ranges::equal(a, b, [](char x, char y) { return lower(x) == lower(y); });
}

/// @brief ASCII case-insensitive substring search. Mirrors the oracle's
/// `containsFold`, INCLUDING its empty-needle behaviour: the oracle's loop
/// runs once and `eqlIgnoreCase("", "")` is true, so an empty needle matches.
/// None of the three call sites passes one, but the difference would be a
/// silent divergence rather than a compile error.
[[nodiscard]] auto contains_fold(std::string_view haystack, std::string_view needle) -> bool {
  if (needle.size() > haystack.size()) {
    return false;
  }
  for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    if (equals_fold(haystack.substr(i, needle.size()), needle)) {
      return true;
    }
  }
  return false;
}

/// @brief `sha256(bytes)` as 64 lowercase hex characters.
///
/// Delegates to `materialize`'s implementation rather than carrying a second
/// copy — the two modules share a bucket, so this needs no new dependency.
[[nodiscard]] auto digest(std::string_view bytes) -> std::string {
  return mz::sha256_hex(bytes);
}

// =========================================================================
// Section extraction
// =========================================================================

/// @brief The body text under a heading that appears as an EXACT whole line.
///
/// Distinct from `extract_section_fold` below and from `materialize::section`:
/// this one requires the heading to be the entire line (modulo a trailing
/// `\r`), which is what makes `missing_acceptance_section` a real check rather
/// than a substring accident.
[[nodiscard]] auto extract_exact_section(std::string_view body, std::string_view heading) -> std::optional<std::string_view> {
  std::size_t offset = 0;
  while (offset <= body.size()) {
    const auto line_end = body.find('\n', offset);
    const auto real_end = line_end == std::string_view::npos ? body.size() : line_end;
    const auto line     = body.substr(offset, real_end - offset);
    if (trim_end_cr(line) == heading) {
      const auto content_start = std::min(offset + line.size() + 1, body.size());
      const auto rest          = body.substr(content_start);
      const auto end           = rest.find("\n## ");
      return trim(rest.substr(0, end == std::string_view::npos ? rest.size() : end));
    }
    if (line_end == std::string_view::npos) {
      break;
    }
    offset = line_end + 1;
  }
  return std::nullopt;
}

/// @brief The body text after a case-insensitive heading match ANYWHERE in the
/// body. Used only for `## Required validation`, whose oracle check is a fold
/// search rather than a line match.
[[nodiscard]] auto extract_section_fold(std::string_view body, std::string_view heading) -> std::optional<std::string_view> {
  if (heading.size() > body.size()) {
    return std::nullopt;
  }
  for (std::size_t start = 0; start + heading.size() <= body.size(); ++start) {
    if (!equals_fold(body.substr(start, heading.size()), heading)) {
      continue;
    }
    const auto rest = body.substr(start + heading.size());
    const auto end  = rest.find("\n## ");
    return trim(rest.substr(0, end == std::string_view::npos ? rest.size() : end));
  }
  return std::nullopt;
}

// =========================================================================
// Predicates
// =========================================================================

[[nodiscard]] auto status_in(std::string_view status, std::span<const std::string_view> accepted) -> bool {
  return std::ranges::find(accepted, status) != accepted.end();
}

/// @brief Whether the evidence's recorded digest still matches the live one.
///
/// All five clauses are the oracle's: both digests present, equal, freshness
/// literally `current`, and a non-blank provenance.
[[nodiscard]] auto evidence_current(const evidence& value) -> bool {
  return !value.source_digest.empty() && !value.current_digest.empty() && value.source_digest == value.current_digest &&
         value.freshness == "current" && !trim(value.provenance).empty();
}

/// @brief Whether a locator names a materialized citation source.
[[nodiscard]] auto section_locator(std::string_view locator) -> bool {
  if (locator == "body") {
    return true;
  }
  const auto hash = locator.find('#');
  return hash != std::string_view::npos && hash > 0 && !trim(locator.substr(hash + 1)).empty();
}

/// @brief Whether a current, section-shaped citation of `kind` is present.
[[nodiscard]] auto has_kind(const std::vector<evidence>& values, std::string_view kind) -> bool {
  return std::ranges::any_of(
      values, [&](const evidence& v) { return v.kind == kind && section_locator(v.locator) && evidence_current(v); });
}

[[nodiscard]] auto generic_acceptance(std::string_view value) -> bool {
  const auto cleaned = trim(value);
  return cleaned.empty() || contains_fold(cleaned, "is implemented and tested") || contains_fold(cleaned, "works as expected") ||
         contains_fold(cleaned, "per spec");
}

[[nodiscard]] auto generic_next_action(std::string_view value) -> bool {
  const auto cleaned = trim(value);
  return cleaned.empty() || contains_fold(cleaned, "implement per acceptance criteria") ||
         contains_fold(cleaned, "implement the task") || equals_fold(cleaned, "todo");
}

/// @brief The truth value of a mandatory boolean fact.
/// @return `nullopt` when no current, required fact of that kind exists;
/// otherwise whether its text reads as true. Note the oracle returns on the
/// FIRST matching fact — a later one cannot override it.
[[nodiscard]] auto mandatory_boolean_fact(const std::vector<evidence>& values, std::string_view kind) -> std::optional<bool> {
  for (const auto& value : values) {
    if (value.kind != kind || !value.required || !evidence_current(value)) {
      continue;
    }
    return value.text == "1" || equals_fold(value.text, "true");
  }
  return std::nullopt;
}

/// @brief Two required facts sharing kind, id and locator but disagreeing on
/// text. Unreachable from `assemble_task` — see packet.cppm's header.
[[nodiscard]] auto contradictory(const std::vector<evidence>& values) -> bool {
  for (std::size_t i = 0; i < values.size(); ++i) {
    const auto& a = values[i];
    if (!a.required) {
      continue;
    }
    for (std::size_t j = i + 1; j < values.size(); ++j) {
      const auto& b = values[j];
      if (b.required && a.kind == b.kind && a.id == b.id && a.locator == b.locator && a.text != b.text) {
        return true;
      }
    }
  }
  return false;
}

/// @brief Append a reason unless it is already present. First occurrence wins,
/// which is what lets a class-walk reason land at an earlier position.
auto append_reason(std::vector<readiness_reason>& reasons, readiness_reason value) -> void {
  if (std::ranges::find(reasons, value) == reasons.end()) {
    reasons.push_back(value);
  }
}

/// @brief Same contract as the overload above, for `planning_reason`.
auto append_reason(std::vector<planning_reason>& reasons, planning_reason value) -> void {
  if (std::ranges::find(reasons, value) == reasons.end()) {
    reasons.push_back(value);
  }
}

// =========================================================================
// Planning predicates
// =========================================================================

/// @brief Whether every REQUIRED artifact is current and in a draft/active
/// status. An artifact this module never loaded (e.g. no `derives-from`
/// link) is simply absent from `values` and cannot trip this check.
[[nodiscard]] auto planning_artifacts_current(const std::vector<evidence>& values) -> bool {
  static constexpr std::string_view accepted[] = {"draft", "active"};
  for (const auto& value : values) {
    if (value.required && (!evidence_current(value) || !status_in(value.status, accepted))) {
      return false;
    }
  }
  return true;
}

/// @brief Whether every REQUIRED decision is current and `accepted`.
[[nodiscard]] auto planning_decisions_accepted(const std::vector<evidence>& values) -> bool {
  for (const auto& value : values) {
    if (value.required && (!evidence_current(value) || value.status != "accepted")) {
      return false;
    }
  }
  return true;
}

/// @brief Whether every REQUIRED coverage row is current, covered, and
/// `complete` — and there is at least one row to say so.
[[nodiscard]] auto planning_coverage_complete(const std::vector<evidence>& values) -> bool {
  for (const auto& value : values) {
    if (value.required && (!evidence_current(value) || !value.covered || value.status != "complete")) {
      return false;
    }
  }
  return !values.empty();
}

/// @brief Whether a gate (strict-preview or apply-boundary) row set has at
/// least one REQUIRED row and every required row is current and accepted.
[[nodiscard]] auto planning_gate_accepted(const std::vector<evidence>& values) -> bool {
  if (values.empty()) {
    return false;
  }
  static constexpr std::string_view accepted[]     = {"accepted", "applied", "ready"};
  std::size_t                       required_count = 0;
  for (const auto& value : values) {
    if (!value.required) {
      continue;
    }
    ++required_count;
    if (!evidence_current(value) || !status_in(value.status, accepted)) {
      return false;
    }
  }
  return required_count > 0;
}

/// @brief Whether a current row of each of the four spec kinds is present, in
/// a draft/active status.
[[nodiscard]] auto has_four_current_artifacts(const std::vector<evidence>& values) -> bool {
  static constexpr std::string_view kinds[]    = {"product_spec", "tech_spec", "roadmap", "test_spec"};
  static constexpr std::string_view accepted[] = {"draft", "active"};
  for (const auto kind : kinds) {
    bool found = false;
    for (const auto& value : values) {
      if (value.kind == kind && evidence_current(value) && status_in(value.status, accepted)) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

/// @brief Whether a current row of each of the four spec kinds is present AND
/// `active` — the REVIEWED bar, stricter than `has_four_current_artifacts`.
[[nodiscard]] auto has_four_reviewed_artifacts(const std::vector<evidence>& values) -> bool {
  static constexpr std::string_view kinds[] = {"product_spec", "tech_spec", "roadmap", "test_spec"};
  for (const auto kind : kinds) {
    bool found = false;
    for (const auto& value : values) {
      if (value.kind == kind && evidence_current(value) && value.status == "active") {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

// =========================================================================
// Canonical rendering
// =========================================================================

/// @brief The oracle's total order over evidence.
///
/// Total rather than merely deterministic: every field participates, ending
/// with `display_label`, so two rows differing only in a display label still
/// have a defined order and the canonical body is stable under input
/// permutation.
[[nodiscard]] auto less_evidence(const evidence& a, const evidence& b) -> bool {
  if (auto c = a.kind <=> b.kind; c != 0) {
    return c < 0;
  }
  if (a.id != b.id) {
    return a.id < b.id;
  }
  if (auto c = a.locator <=> b.locator; c != 0) {
    return c < 0;
  }
  if (auto c = a.text <=> b.text; c != 0) {
    return c < 0;
  }
  if (auto c = a.source_digest <=> b.source_digest; c != 0) {
    return c < 0;
  }
  if (auto c = a.current_digest <=> b.current_digest; c != 0) {
    return c < 0;
  }
  if (a.required != b.required) {
    return !a.required;
  }
  if (a.covered != b.covered) {
    return !a.covered;
  }
  if (auto c = a.status <=> b.status; c != 0) {
    return c < 0;
  }
  if (auto c = a.provenance <=> b.provenance; c != 0) {
    return c < 0;
  }
  if (auto c = a.materializer_version <=> b.materializer_version; c != 0) {
    return c < 0;
  }
  if (auto c = a.current_materializer_version <=> b.current_materializer_version; c != 0) {
    return c < 0;
  }
  if (auto c = a.freshness <=> b.freshness; c != 0) {
    return c < 0;
  }
  return a.display_label < b.display_label;
}

auto append_evidence_field(std::string& out, std::string_view name, const std::vector<evidence>& values,
                           bool include_display_labels) -> void {
  auto sorted = values;
  std::ranges::sort(sorted, less_evidence);
  out.append(",\"");
  out.append(name);
  out.append("\":[");
  bool first = true;
  for (const auto& item : sorted) {
    if (!first) {
      out.push_back(',');
    }
    first = false;
    out.append("{\"kind\":");
    append_json_string(out, item.kind);
    out.append(std::format(",\"id\":{},\"locator\":", item.id));
    append_json_string(out, item.locator);
    out.append(",\"text\":");
    append_json_string(out, item.text);
    if (include_display_labels) {
      out.append(",\"display_label\":");
      append_json_string(out, item.display_label);
    }
    out.append(",\"source_digest\":");
    append_json_string(out, item.source_digest);
    out.append(",\"current_digest\":");
    append_json_string(out, item.current_digest);
    out.append(std::format(",\"required\":{},\"covered\":{}", item.required, item.covered));
    out.append(",\"status\":");
    append_json_string(out, item.status);
    out.append(",\"provenance\":");
    append_json_string(out, item.provenance);
    out.append(",\"materializer_version\":");
    append_json_string(out, item.materializer_version);
    out.append(",\"current_materializer_version\":");
    append_json_string(out, item.current_materializer_version);
    out.append(",\"freshness\":");
    append_json_string(out, item.freshness);
    out.push_back('}');
  }
  out.push_back(']');
}

/// @brief The canonical body.
/// @param include_display_labels True for the `canonical` field, false for the
/// bytes the digest is taken over. The two forms differ by exactly the
/// `display_label` key, which is what makes the digest ignore cosmetic edits.
[[nodiscard]] auto canonical_task(const task_input& input, bool include_display_labels) -> std::string {
  std::string out;
  out.append("{\"policy\":");
  append_json_string(out, policy_version);
  out.append(std::format(",\"task_id\":{},\"status\":", input.task_id));
  append_json_string(out, input.status);
  out.append(",\"title\":");
  append_json_string(out, input.title);
  out.append(",\"body\":");
  append_json_string(out, input.body);
  out.append(",\"next_action\":");
  append_json_string(out, input.next_action);
  out.append(",\"acceptance_criteria\":");
  append_json_string(out, input.acceptance_criteria);
  append_evidence_field(out, "owning_plans", input.owning_plans, include_display_labels);
  append_evidence_field(out, "anchor_plans", input.anchor_plans, include_display_labels);
  append_evidence_field(out, "citations", input.citations, include_display_labels);
  append_evidence_field(out, "decisions", input.decisions, include_display_labels);
  append_evidence_field(out, "questions", input.questions, include_display_labels);
  append_evidence_field(out, "scenarios", input.scenarios, include_display_labels);
  append_evidence_field(out, "dependencies", input.dependencies, include_display_labels);
  append_evidence_field(out, "touches", input.touches, include_display_labels);
  append_evidence_field(out, "claims", input.claims, include_display_labels);
  append_evidence_field(out, "validation_gates", input.validation_gates, include_display_labels);
  append_evidence_field(out, "facts", input.facts, include_display_labels);
  out.push_back('}');
  return out;
}

/// @brief The canonical body for a planning packet. Same include/exclude
/// contract as `canonical_task`.
[[nodiscard]] auto canonical_planning(const planning_input& input, bool include_display_labels) -> std::string {
  std::string out;
  out.append("{\"policy\":");
  append_json_string(out, policy_version);
  out.append(",\"role\":");
  append_json_string(out, planning_role_name(input.role));
  out.append(",\"goal\":");
  append_json_string(out, input.goal);
  append_evidence_field(out, "scope_facts", input.scope_facts, include_display_labels);
  append_evidence_field(out, "artifacts", input.artifacts, include_display_labels);
  append_evidence_field(out, "questions", input.questions, include_display_labels);
  append_evidence_field(out, "constraints", input.constraints, include_display_labels);
  append_evidence_field(out, "required_outputs", input.required_outputs, include_display_labels);
  append_evidence_field(out, "strict_preview", input.strict_preview, include_display_labels);
  append_evidence_field(out, "coverage", input.coverage, include_display_labels);
  append_evidence_field(out, "decisions", input.decisions, include_display_labels);
  out.append(",\"review_rubric_version\":");
  append_json_string(out, input.review_rubric_version);
  append_evidence_field(out, "apply_boundary", input.apply_boundary, include_display_labels);
  out.push_back('}');
  return out;
}

// =========================================================================
// Evidence classes
// =========================================================================

struct evidence_class {
  const std::vector<evidence>*      values;
  readiness_reason                  reason;
  std::span<const std::string_view> accepted;
};

auto validate_evidence_classes(std::vector<readiness_reason>& reasons, const task_input& input) -> void {
  static constexpr std::string_view plan_ok[]     = {"active", "paused", "ready"};
  static constexpr std::string_view citation_ok[] = {"active", "draft", "ready"};
  static constexpr std::string_view decision_ok[] = {"accepted", "ready"};
  static constexpr std::string_view question_ok[] = {"answered", "non_blocking", "ready"};
  static constexpr std::string_view dep_ok[]      = {"satisfied", "ready", "done"};
  static constexpr std::string_view touch_ok[]    = {"resolved", "ready"};
  static constexpr std::string_view claim_ok[]    = {"active"};
  static constexpr std::string_view gate_ok[]     = {"required", "ready"};

  const evidence_class classes[] = {
      {&input.owning_plans, readiness_reason::invalid_owning_plan, plan_ok},
      {&input.anchor_plans, readiness_reason::invalid_anchor_plan, plan_ok},
      {&input.citations, readiness_reason::unresolved_citation, citation_ok},
      {&input.decisions, readiness_reason::invalid_locked_decision, decision_ok},
      {&input.questions, readiness_reason::unresolved_question, question_ok},
      {&input.dependencies, readiness_reason::invalid_dependency, dep_ok},
      {&input.touches, readiness_reason::invalid_touch, touch_ok},
      {&input.claims, readiness_reason::inactive_claim, claim_ok},
      {&input.validation_gates, readiness_reason::invalid_validation_gate, gate_ok},
  };

  for (const auto& cls : classes) {
    for (const auto& item : *cls.values) {
      if (!item.required) {
        continue;
      }
      if (trim(item.provenance).empty()) {
        append_reason(reasons, readiness_reason::missing_provenance);
      }
      // NOTE: this class walk checks digest equality DIRECTLY rather than
      // through `evidence_current` — it does not consult `freshness` or
      // `provenance`. The scenario walk below DOES use `evidence_current`.
      // The two are not interchangeable and the oracle uses each in exactly
      // one place.
      if (item.source_digest.empty() || item.current_digest.empty() || item.source_digest != item.current_digest) {
        append_reason(reasons, readiness_reason::stale_mandatory_evidence);
      }
      if (!status_in(item.status, cls.accepted)) {
        append_reason(reasons, cls.reason);
      }
    }
  }

  // Scenario lifecycle is independent of coverage. A linked DRAFT scenario
  // covers its task in the authoritative oracle; only the verifies/ownership
  // relationship (carried by `covered`) determines coverage readiness. That is
  // why scenarios are absent from the status-checked classes above.
  for (const auto& item : input.scenarios) {
    if (!item.required) {
      continue;
    }
    if (trim(item.provenance).empty()) {
      append_reason(reasons, readiness_reason::missing_provenance);
    }
    if (!evidence_current(item)) {
      append_reason(reasons, readiness_reason::stale_mandatory_evidence);
    }
    if (!item.covered) {
      append_reason(reasons, readiness_reason::uncovered_required_scenario);
    }
  }
}

// =========================================================================
// Loaders
// =========================================================================

/// @brief Materialize one evidence row from a 7-column result whose columns
/// are (kind, id, locator, text, status, provenance, display_label).
///
/// `source_digest` and `current_digest` are BOTH set to `digest(text)`, so a
/// row loaded this way is current by construction. That is the oracle's
/// behaviour and it is why plan/decision/scenario/touch/claim evidence can
/// never be `stale_mandatory_evidence` — only citations and facts, which carry
/// a stored digest to disagree with, can.
[[nodiscard]] auto row_evidence(db::statement& stmt) -> evidence {
  auto       text    = stmt.column_text(3);
  const auto current = digest(text);
  evidence   out;
  out.kind           = stmt.column_text(0);
  out.id             = stmt.column_int64(1);
  out.locator        = stmt.column_text(2);
  out.text           = std::move(text);
  out.display_label  = stmt.column_text(6);
  out.source_digest  = current;
  out.current_digest = current;
  out.status         = stmt.column_text(4);
  out.provenance     = stmt.column_text(5);
  return out;
}

/// @brief Run a single-row evidence query, binding `id` to every `?` it
/// declares.
///
/// The oracle supports one, two or three placeholders and errors beyond that.
/// Every query it actually passes declares exactly one; the wider arms are
/// reproduced so a future query does not silently bind short.
[[nodiscard]] auto one_row_evidence(db::connection& conn, std::string_view sql, std::int64_t id)
    -> std::expected<std::optional<evidence>, packet_error> {
  const auto placeholders = static_cast<int>(std::ranges::count(sql, '?'));
  if (placeholders < 1 || placeholders > 3) {
    return std::unexpected(packet_error::query_failed);
  }
  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(packet_error::query_failed);
  }
  for (int i = 1; i <= placeholders; ++i) {
    if (!stmt->bind_int64(i, id)) {
      return std::unexpected(packet_error::query_failed);
    }
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(packet_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::optional<evidence>{};
  }
  return std::optional<evidence>{row_evidence(*stmt)};
}

/// @brief Evidence for every entity linked FROM the task by `relationship`.
[[nodiscard]] auto linked_evidence(db::connection& conn, std::int64_t task_id, std::string_view kind,
                                   std::string_view relationship, std::string_view entity_sql)
    -> std::expected<std::vector<evidence>, packet_error> {
  auto ids = conn.prepare("select to_id from entity_links where from_kind='task' and from_id=? "
                          "and to_kind=? and relationship=? order by to_id");
  if (!ids) {
    return std::unexpected(packet_error::query_failed);
  }
  if (!ids->bind_int64(1, task_id) || !ids->bind_text(2, kind) || !ids->bind_text(3, relationship)) {
    return std::unexpected(packet_error::query_failed);
  }
  std::vector<std::int64_t> targets;
  while (true) {
    auto step = ids->step();
    if (!step) {
      return std::unexpected(packet_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    targets.push_back(ids->column_int64(0));
  }
  std::vector<evidence> out;
  for (const auto target : targets) {
    auto row = one_row_evidence(conn, entity_sql, target);
    if (!row) {
      return std::unexpected(row.error());
    }
    if (row->has_value()) {
      out.push_back(std::move(**row));
    }
  }
  return out;
}

/// @brief The section a fact's locator resolves to in the LIVE database.
///
/// The digest of this string is what the stored `source_digest` is compared
/// against, so every arm here must match what `materialize` wrote, byte for
/// byte, or the fact is permanently stale.
[[nodiscard]] auto fact_semantic_source(db::connection& conn, std::int64_t task_id, std::string_view fact_kind,
                                        std::string_view source_kind, std::int64_t source_id, std::string_view locator)
    -> std::expected<std::optional<std::string>, packet_error>;

[[nodiscard]] auto scalar_text(db::connection& conn, std::string_view sql, std::int64_t id)
    -> std::expected<std::optional<std::string>, packet_error> {
  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(packet_error::query_failed);
  }
  if (!stmt->bind_int64(1, id)) {
    return std::unexpected(packet_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(packet_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::optional<std::string>{};
  }
  return std::optional<std::string>{stmt->column_text(0)};
}

[[nodiscard]] auto count_query(db::connection& conn, std::string_view sql, std::int64_t id)
    -> std::expected<std::int64_t, packet_error> {
  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(packet_error::query_failed);
  }
  if (!stmt->bind_int64(1, id)) {
    return std::unexpected(packet_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(packet_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::int64_t{0};
  }
  return stmt->column_int64(0);
}

enum class relationship_direction : std::uint8_t { outgoing, incoming };

[[nodiscard]] auto live_relationship(db::connection& conn, std::int64_t task_id, std::string_view other_kind,
                                     std::int64_t other_id, std::string_view relationship, relationship_direction direction)
    -> std::expected<bool, packet_error> {
  const std::string_view sql  = direction == relationship_direction::outgoing
                                    ? "select count(*) from entity_links where relationship=? and from_kind='task' and from_id=? "
                                      "and to_kind=? and to_id=?"
                                    : "select count(*) from entity_links where relationship=? and to_kind='task' and to_id=? "
                                      "and from_kind=? and from_id=?";
  auto                   stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(packet_error::query_failed);
  }
  if (!stmt->bind_text(1, relationship) || !stmt->bind_int64(2, task_id) || !stmt->bind_text(3, other_kind) ||
      !stmt->bind_int64(4, other_id)) {
    return std::unexpected(packet_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(packet_error::query_failed);
  }
  return *step == db::step_result::row && stmt->column_int64(0) > 0;
}

auto fact_semantic_source(db::connection& conn, std::int64_t task_id, std::string_view fact_kind, std::string_view source_kind,
                          std::int64_t source_id, std::string_view locator)
    -> std::expected<std::optional<std::string>, packet_error> {
  const auto none = std::optional<std::string>{};

  if (source_kind == "task" && source_id == task_id) {
    if (locator == "body#acceptance-criteria") {
      auto body = scalar_text(conn, "select coalesce(body,'') from tasks where id=?", task_id);
      if (!body) {
        return std::unexpected(body.error());
      }
      if (!body->has_value()) {
        return none;
      }
      return std::optional<std::string>{std::string{mz::section(**body, "## Acceptance Criteria")}};
    }
    if (locator == "next_action") {
      return scalar_text(conn, "select coalesce(next_action,'') from tasks where id=?", task_id);
    }
    if (locator == "body") {
      return scalar_text(conn, "select coalesce(body,'') from tasks where id=?", task_id);
    }
    // These labels MUST match what `materialize`'s stage-count pass stored,
    // byte for byte, or the fact is permanently stale — hence the shared
    // constants rather than literals. In particular the dependency label stays
    // `blocks`: it is a FACT KIND, not the renamed entity_links relationship,
    // even though the live count below correctly queries `depends-on`.
    std::optional<std::string_view> count_kind;
    if (locator == mz::locator_touches) {
      count_kind = mz::counted_kind_touch;
    } else if (locator == mz::locator_scenarios) {
      count_kind = mz::counted_kind_scenario;
    } else if (locator == mz::locator_dependency_fanout) {
      count_kind = mz::counted_kind_dependency;
    }
    if (count_kind.has_value()) {
      std::string_view count_sql;
      if (*count_kind == mz::counted_kind_touch) {
        count_sql = "select count(*) from entity_links where from_kind='task' and from_id=? "
                    "and relationship='touches'";
      } else if (*count_kind == mz::counted_kind_scenario) {
        count_sql = "select count(*) from entity_links where to_kind='task' and to_id=? "
                    "and from_kind='test_scenario' and relationship='verifies'";
      } else {
        count_sql = "select count(*) from entity_links where from_kind='task' and from_id=? "
                    "and to_kind='task' and relationship='depends-on'";
      }
      auto count = count_query(conn, count_sql, task_id);
      if (!count) {
        return std::unexpected(count.error());
      }
      return std::optional<std::string>{std::format("{}:{}", *count_kind, *count)};
    }
  }

  if (source_kind == "artifact") {
    auto body = scalar_text(conn, "select coalesce(body,'') from artifacts where id=?", source_id);
    if (!body) {
      return std::unexpected(body.error());
    }
    if (!body->has_value()) {
      return none;
    }
    if (locator == "body") {
      return body;
    }
    // Same roadmap-locator rule as the citation path. Fixing only that one
    // left the FACT for the same locator resolving to nothing, so a task's
    // roadmap citation read `current` while its `cited_artifact_section` fact
    // stayed permanently stale — one locator with two answers.
    if (locator.starts_with("roadmap#")) {
      return mz::roadmap_section(**body, locator);
    }
    const auto section = mz::artifact_section(**body, locator);
    if (!section.has_value()) {
      return none;
    }
    return std::optional<std::string>{std::string{*section}};
  }

  if (source_kind == "decision") {
    return scalar_text(conn, "select body from decisions where id=?", source_id);
  }
  if (source_kind == "question") {
    return scalar_text(conn, "select coalesce(body,'') from questions where id=?", source_id);
  }
  if (source_kind == "test_scenario") {
    if (locator == "status") {
      return scalar_text(conn, "select status from test_scenarios where id=?", source_id);
    }
    auto body = scalar_text(conn, "select coalesce(body,'') from test_scenarios where id=?", source_id);
    if (!body) {
      return std::unexpected(body.error());
    }
    if (!body->has_value()) {
      return none;
    }
    if (locator == "body#acceptance") {
      const auto value = mz::field(**body, "**Acceptance:**");
      return std::optional<std::string>{value.has_value() ? std::string{*value} : std::string{}};
    }
    return body;
  }

  // `blocks` / `blocked_by` are routing FACT KINDS, a separate vocabulary from
  // the entity_links relationship (renamed to `depends-on` in migration
  // 00033). The fact kind is unchanged; only the relationship it resolves
  // against moved.
  if (fact_kind == "touch") {
    auto live = live_relationship(conn, task_id, source_kind, source_id, "touches", relationship_direction::outgoing);
    if (!live) {
      return std::unexpected(live.error());
    }
    return *live ? std::optional<std::string>{"touches"} : none;
  }
  if (fact_kind == "blocks") {
    auto live = live_relationship(conn, task_id, source_kind, source_id, "depends-on", relationship_direction::outgoing);
    if (!live) {
      return std::unexpected(live.error());
    }
    return *live ? std::optional<std::string>{"depends-on"} : none;
  }
  if (fact_kind == "blocked_by") {
    auto live = live_relationship(conn, task_id, source_kind, source_id, "depends-on", relationship_direction::incoming);
    if (!live) {
      return std::unexpected(live.error());
    }
    return *live ? std::optional<std::string>{"depends-on"} : none;
  }
  return none;
}

/// @brief Mandatory task citations: the persisted section facts ingest
/// produced, joined back to BOTH the direct task link and the current
/// artifact. A whole-artifact link is necessary identity but never sufficient
/// context, which is why the join demands the fact row too.
[[nodiscard]] auto citation_evidence(db::connection& conn, std::int64_t task_id)
    -> std::expected<std::vector<evidence>, packet_error> {
  auto stmt = conn.prepare(R"(select a.kind,a.id,f.source_locator,coalesce(a.body,''),a.status,
       'artifact:'||a.id,a.title,f.source_digest,f.materializer_version
from routing_task_facts f
join artifacts a on a.id=f.source_entity_id
join entity_links el on el.from_kind='task' and el.from_id=f.task_id
 and el.to_kind='artifact' and el.to_id=a.id and el.relationship='cites'
where f.task_id=? and f.fact_kind='cited_artifact_section'
 and f.source_entity_kind='artifact'
 and (f.source_locator='body' or not exists (
   select 1 from routing_task_facts reviewed
   where reviewed.task_id=f.task_id and reviewed.fact_kind='cited_artifact_section'
     and reviewed.source_entity_kind='artifact' and reviewed.source_entity_id=f.source_entity_id
     and reviewed.source_locator='body'
 ))
order by a.kind,a.id,f.source_locator)");
  if (!stmt) {
    return std::unexpected(packet_error::query_failed);
  }
  if (!stmt->bind_int64(1, task_id)) {
    return std::unexpected(packet_error::query_failed);
  }
  std::vector<evidence> out;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(packet_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    const auto body    = stmt->column_text(3);
    const auto locator = stmt->column_text(2);
    // Roadmap locators are `roadmap#milestone:N/item:M`, which
    // `artifact_section` cannot resolve — it expects `#Heading`. Routing them
    // there yielded an empty digest, so every roadmap citation was stale the
    // instant ingestion wrote it, and no amount of operator work could make
    // the packet ready.
    std::optional<std::string> section;
    if (locator == "body") {
      section = body;
    } else if (locator.starts_with("roadmap#")) {
      section = mz::roadmap_section(body, locator);
    } else if (const auto found = mz::artifact_section(body, locator); found.has_value()) {
      section = std::string{*found};
    }
    const auto  artifact_id    = stmt->column_int64(1);
    std::string current        = section.has_value() ? mz::source_digest("artifact", artifact_id, locator, *section) : "";
    const auto  source         = stmt->column_text(7);
    const auto  stored_version = stmt->column_text(8);
    // Same two-materializer rule as `fact_evidence` below: a citation fact
    // staged by `task facts stage` carries `operator-v1` and must not be
    // stale merely for saying so (decision 1102).
    const bool known = stored_version == mz::materializer_version || stored_version == mz::operator_materializer_version;
    const bool fresh = !current.empty() && source == current && known;

    evidence item;
    item.kind                 = stmt->column_text(0);
    item.id                   = artifact_id;
    item.locator              = locator;
    item.text                 = section.value_or(std::string{});
    item.display_label        = stmt->column_text(6);
    item.source_digest        = source;
    item.current_digest       = std::move(current);
    item.status               = stmt->column_text(4);
    item.provenance           = stmt->column_text(5);
    item.materializer_version = stored_version;
    // Report the stored version back as "current" when routing recognises it,
    // so readiness's `materializer_version != current_materializer_version`
    // comparison agrees with the freshness rule above rather than flagging
    // every operator fact.
    item.current_materializer_version = known ? item.materializer_version : std::string{mz::materializer_version};
    item.freshness                    = fresh ? "current" : "stale";
    out.push_back(std::move(item));
  }
  return out;
}

/// @brief Every direct `verifies` scenario, with coverage-oracle membership
/// carried separately.
///
/// Preserve every direct verifies link. Coverage-oracle membership is a
/// SEPARATE fact: a scenario is covered only when it also belongs to the
/// owning anchor plan. Cross-plan scenarios stay visible with `covered=false`
/// rather than vanishing, so the reason names the real problem.
[[nodiscard]] auto scenario_evidence(db::connection& conn, std::int64_t task_id, std::int64_t anchor_plan_id)
    -> std::expected<std::vector<evidence>, packet_error> {
  auto stmt = conn.prepare(R"(select 'scenario',s.id,'scenario:'||s.id,coalesce(s.body,s.title),
       s.status,'scenario:'||s.id,s.title,
       exists(
         select 1 from entity_links owner
         where owner.from_kind='test_scenario' and owner.from_id=s.id
         and owner.to_kind='plan' and owner.to_id=?
         and owner.relationship='derives-from'
       )
from test_scenarios s
join entity_links verifies on verifies.from_kind='test_scenario'
 and verifies.from_id=s.id and verifies.to_kind='task'
 and verifies.to_id=? and verifies.relationship='verifies'
where s.status != 'retired'
order by s.id)");
  if (!stmt) {
    return std::unexpected(packet_error::query_failed);
  }
  if (!stmt->bind_int64(1, anchor_plan_id) || !stmt->bind_int64(2, task_id)) {
    return std::unexpected(packet_error::query_failed);
  }
  std::vector<evidence> out;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(packet_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    auto item    = row_evidence(*stmt);
    item.covered = stmt->column_int64(7) != 0;
    out.push_back(std::move(item));
  }
  return out;
}

/// @brief Declared touches: path-level rows first, then whole-repo edges.
///
/// The two queries append in that fixed order, so the packet's `touches` list
/// is paths-then-repos regardless of ids. The canonical body sorts anyway; the
/// JSON `input` does not, so this order is visible.
[[nodiscard]] auto path_evidence(db::connection& conn, std::int64_t task_id)
    -> std::expected<std::vector<evidence>, packet_error> {
  std::vector<evidence> out;
  auto                  paths = conn.prepare("select 'touch',id,path,path,'resolved','task_touch_path:'||id,path "
                                             "from task_touch_paths where task_id=? order by id");
  if (!paths) {
    return std::unexpected(packet_error::query_failed);
  }
  if (!paths->bind_int64(1, task_id)) {
    return std::unexpected(packet_error::query_failed);
  }
  while (true) {
    auto step = paths->step();
    if (!step) {
      return std::unexpected(packet_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    out.push_back(row_evidence(*paths));
  }
  auto repos = conn.prepare("select 'touch',p.id,'repo:'||p.id,p.slug,'resolved','project:'||p.id,p.name "
                            "from entity_links el join projects p on p.id=el.to_id "
                            "where el.from_kind='task' and el.from_id=? and el.to_kind='repo' "
                            "and el.relationship='touches' order by p.id");
  if (!repos) {
    return std::unexpected(packet_error::query_failed);
  }
  if (!repos->bind_int64(1, task_id)) {
    return std::unexpected(packet_error::query_failed);
  }
  while (true) {
    auto step = repos->step();
    if (!step) {
      return std::unexpected(packet_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    out.push_back(row_evidence(*repos));
  }
  return out;
}

/// @brief Active claims, with an EXPIRED lease reported as `expired`.
///
/// The WHERE clause admits only `status='active'` rows, and the CASE then
/// re-derives `active` vs `expired` from `lease_expires_at`. So a lapsed lease
/// is visible evidence that blocks the packet rather than an absent row that
/// silently permits it.
[[nodiscard]] auto claim_evidence(db::connection& conn, std::int64_t task_id)
    -> std::expected<std::vector<evidence>, packet_error> {
  auto stmt = conn.prepare("select 'claim',id,'claim:'||id,claim_token,"
                           "case when status='active' and lease_expires_at>strftime('%Y-%m-%dT%H:%M:%fZ','now') then 'active' "
                           "when status='active' then 'expired' else status end,"
                           "'agent_work_claim:'||id,'' from agent_work_claims "
                           "where entity_kind='task' and entity_id=? and status='active' order by id");
  if (!stmt) {
    return std::unexpected(packet_error::query_failed);
  }
  if (!stmt->bind_int64(1, task_id)) {
    return std::unexpected(packet_error::query_failed);
  }
  std::vector<evidence> out;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(packet_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    out.push_back(row_evidence(*stmt));
  }
  return out;
}

/// @brief The task's declared validation gates, read out of its own body.
///
/// A single evidence row keyed on the task, not a per-command list — the
/// packet only asks whether the section EXISTS and is unchanged.
[[nodiscard]] auto gate_evidence(std::int64_t task_id, std::string_view body) -> std::vector<evidence> {
  const auto section = extract_section_fold(body, "## Required validation");
  if (!section.has_value()) {
    return {};
  }
  const auto current = digest(*section);
  evidence   item;
  item.kind           = "validation_gate";
  item.id             = task_id;
  item.locator        = "task:required-validation";
  item.text           = std::string{*section};
  item.source_digest  = current;
  item.current_digest = current;
  item.status         = "required";
  item.provenance     = "task.body";
  return {std::move(item)};
}

/// @brief Every persisted routing fact for the task, with freshness recomputed
/// live against its source.
[[nodiscard]] auto fact_evidence(db::connection& conn, std::int64_t task_id)
    -> std::expected<std::vector<evidence>, packet_error> {
  auto stmt =
      conn.prepare("select fact_kind,id,source_locator,"
                   "coalesce(value_text,cast(value_bool as text),cast(value_integer as text),cast(value_real as text),''),"
                   "source_digest,source_entity_kind,source_entity_id,materializer_version "
                   "from routing_task_facts where task_id=? order by id");
  if (!stmt) {
    return std::unexpected(packet_error::query_failed);
  }
  if (!stmt->bind_int64(1, task_id)) {
    return std::unexpected(packet_error::query_failed);
  }
  struct raw_fact {
    std::string  fact_kind;
    std::int64_t id = 0;
    std::string  locator;
    std::string  text;
    std::string  source;
    std::string  source_kind;
    std::int64_t source_id = 0;
    std::string  stored_version;
  };
  std::vector<raw_fact> rows;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(packet_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    rows.push_back({.fact_kind      = stmt->column_text(0),
                    .id             = stmt->column_int64(1),
                    .locator        = stmt->column_text(2),
                    .text           = stmt->column_text(3),
                    .source         = stmt->column_text(4),
                    .source_kind    = stmt->column_text(5),
                    .source_id      = stmt->column_int64(6),
                    .stored_version = stmt->column_text(7)});
  }

  std::vector<evidence> out;
  for (auto& row : rows) {
    auto semantic = fact_semantic_source(conn, task_id, row.fact_kind, row.source_kind, row.source_id, row.locator);
    if (!semantic) {
      return std::unexpected(semantic.error());
    }
    std::string current = semantic->has_value() ? mz::source_digest(row.source_kind, row.source_id, row.locator, **semantic) : "";
    // Two materializers now write facts: `spec ingest --apply` for a whole
    // anchor subtree, and `task facts stage` for one operator-owned task
    // (decision 1102). Both are versions routing still understands, so the
    // check is set membership rather than equality with one constant --
    // stamping an operator fact with an unrecognised version would make it
    // unconditionally stale and `stale_fact` is a blanket reason over every
    // fact, so the packet could never be ready. The DIGEST comparison is
    // deliberately untouched: an operator-staged fact still goes stale the
    // moment its source text changes.
    const bool known = row.stored_version == mz::materializer_version || row.stored_version == mz::operator_materializer_version;
    const bool fresh = !current.empty() && row.source == current && known;

    evidence item;
    item.kind                 = std::move(row.fact_kind);
    item.id                   = row.id;
    item.locator              = std::move(row.locator);
    item.text                 = std::move(row.text);
    item.source_digest        = std::move(row.source);
    item.current_digest       = std::move(current);
    item.status               = "materialized";
    item.provenance           = std::format("{}:{}", row.source_kind, row.source_id);
    item.materializer_version = std::move(row.stored_version);
    // Report a RECOGNISED stored version back as the current one, so
    // readiness's `materializer_version != current_materializer_version`
    // clause agrees with the freshness rule above. Reporting the ingest
    // constant unconditionally would fire `stale_fact` for every
    // operator-staged fact even though its digest matches.
    item.current_materializer_version = known ? item.materializer_version : std::string{mz::materializer_version};
    item.freshness                    = fresh ? "current" : "stale";
    out.push_back(std::move(item));
  }
  return out;
}

// =========================================================================
// Planning loaders
// =========================================================================

/// @brief Evidence for every entity linked TO the plan by `relationship
/// = 'derives-from'` — the reverse direction from `linked_evidence`, which
/// walks edges FROM a task.
[[nodiscard]] auto plan_linked_evidence(db::connection& conn, std::int64_t plan_id, std::string_view kind,
                                        std::string_view entity_sql) -> std::expected<std::vector<evidence>, packet_error> {
  auto ids = conn.prepare("select from_id from entity_links where from_kind=? and to_kind='plan' and to_id=? "
                          "and relationship='derives-from' order by from_id");
  if (!ids) {
    return std::unexpected(packet_error::query_failed);
  }
  if (!ids->bind_text(1, kind) || !ids->bind_int64(2, plan_id)) {
    return std::unexpected(packet_error::query_failed);
  }
  std::vector<std::int64_t> targets;
  while (true) {
    auto step = ids->step();
    if (!step) {
      return std::unexpected(packet_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    targets.push_back(ids->column_int64(0));
  }
  std::vector<evidence> out;
  for (const auto target : targets) {
    auto row = one_row_evidence(conn, entity_sql, target);
    if (!row) {
      return std::unexpected(row.error());
    }
    if (row->has_value()) {
      out.push_back(std::move(**row));
    }
  }
  return out;
}

/// @brief The four fixed `required_output` rows every planning packet
/// declares — always present, always current by construction.
[[nodiscard]] auto fixed_outputs() -> std::vector<evidence> {
  static constexpr std::string_view names[] = {"product_spec", "tech_spec", "roadmap", "test_spec"};
  std::vector<evidence>             out;
  out.reserve(std::size(names));
  for (std::size_t index = 0; index < std::size(names); ++index) {
    const auto current = digest(names[index]);
    evidence   item;
    item.kind           = "required_output";
    item.id             = static_cast<std::int64_t>(index + 1);
    item.locator        = std::string{names[index]};
    item.text           = std::string{names[index]};
    item.source_digest  = current;
    item.current_digest = current;
    item.provenance     = "routing-packet-policy";
    out.push_back(std::move(item));
  }
  return out;
}

/// @brief `total_tasks`: every task belonging to the anchor plan or one of
/// its `plan -> plan derives-from` milestones.
///
/// DUPLICATED from `test_spec_status::compute`'s step-1/step-4 walk rather
/// than called — see packet.cppm's header. This is the single-query
/// reproduction of that walk's `total_tasks` aggregate.
[[nodiscard]] auto milestone_task_count(db::connection& conn, std::int64_t plan_id) -> std::expected<std::int64_t, packet_error> {
  auto stmt = conn.prepare("select count(*) from tasks where plan_id=? or plan_id in ("
                           "select from_id from entity_links where from_kind='plan' and to_kind='plan' "
                           "and to_id=? and relationship='derives-from')");
  if (!stmt) {
    return std::unexpected(packet_error::query_failed);
  }
  if (!stmt->bind_int64(1, plan_id) || !stmt->bind_int64(2, plan_id)) {
    return std::unexpected(packet_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(packet_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::int64_t{0};
  }
  return stmt->column_int64(0);
}

/// @brief `total_scenarios`: every scenario the anchor plan's own
/// `derives-from` edges reach (milestone plans are NOT walked for this one —
/// matching the oracle, which scopes scenarios to the anchor alone).
[[nodiscard]] auto milestone_scenario_count(db::connection& conn, std::int64_t plan_id)
    -> std::expected<std::int64_t, packet_error> {
  auto stmt = conn.prepare("select count(*) from test_scenarios where id in ("
                           "select from_id from entity_links where from_kind='test_scenario' and to_kind='plan' "
                           "and to_id=? and relationship='derives-from')");
  if (!stmt) {
    return std::unexpected(packet_error::query_failed);
  }
  if (!stmt->bind_int64(1, plan_id)) {
    return std::unexpected(packet_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(packet_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::int64_t{0};
  }
  return stmt->column_int64(0);
}

/// @brief `tasks_covered`: the distinct count of milestone tasks (same set as
/// `milestone_task_count`) that some anchor-attached scenario `verifies`.
///
/// A task belongs to exactly one plan, so `count(distinct t.id)` reproduces
/// the oracle's per-milestone tally summed across milestones without
/// double-counting.
[[nodiscard]] auto milestone_tasks_covered_count(db::connection& conn, std::int64_t plan_id)
    -> std::expected<std::int64_t, packet_error> {
  auto stmt = conn.prepare(
      "select count(distinct t.id) from tasks t "
      "where (t.plan_id=? or t.plan_id in ("
      "  select from_id from entity_links where from_kind='plan' and to_kind='plan' and to_id=? and relationship='derives-from'"
      ")) and t.id in ("
      "  select el.to_id from entity_links el where el.from_kind='test_scenario' and el.to_kind='task' "
      "  and el.relationship='verifies' and el.from_id in ("
      "    select from_id from entity_links where from_kind='test_scenario' and to_kind='plan' and to_id=? "
      "    and relationship='derives-from'"
      "  )"
      ")");
  if (!stmt) {
    return std::unexpected(packet_error::query_failed);
  }
  if (!stmt->bind_int64(1, plan_id) || !stmt->bind_int64(2, plan_id) || !stmt->bind_int64(3, plan_id)) {
    return std::unexpected(packet_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(packet_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::int64_t{0};
  }
  return stmt->column_int64(0);
}

/// @brief The single `coverage` evidence row, from the three duplicated
/// counting queries above. Complete iff there is at least one task, every
/// milestone task is covered, and at least one scenario exists — the same
/// three-way AND `assemble_planning`'s oracle applies inline.
[[nodiscard]] auto planning_coverage_evidence(db::connection& conn, std::int64_t plan_id)
    -> std::expected<std::vector<evidence>, packet_error> {
  auto total_tasks = milestone_task_count(conn, plan_id);
  if (!total_tasks) {
    return std::unexpected(total_tasks.error());
  }
  auto tasks_covered = milestone_tasks_covered_count(conn, plan_id);
  if (!tasks_covered) {
    return std::unexpected(tasks_covered.error());
  }
  auto total_scenarios = milestone_scenario_count(conn, plan_id);
  if (!total_scenarios) {
    return std::unexpected(total_scenarios.error());
  }

  const bool complete = *total_tasks > 0 && *tasks_covered == *total_tasks && *total_scenarios > 0;
  const auto text     = std::format("tasks:{};covered:{};scenarios:{}", *total_tasks, *tasks_covered, *total_scenarios);
  const auto current  = digest(text);

  evidence item;
  item.kind           = "coverage";
  item.id             = plan_id;
  item.locator        = "plan:test-scenario-coverage";
  item.text           = text;
  item.source_digest  = current;
  item.current_digest = current;
  item.covered        = complete;
  item.status         = complete ? "complete" : "incomplete";
  item.provenance     = std::format("plan:{}", plan_id);
  return std::vector<evidence>{std::move(item)};
}

} // namespace

// ===========================================================================
// Public surface
// ===========================================================================

auto reason_name(readiness_reason reason) -> std::string_view {
  using r = readiness_reason;
  switch (reason) {
  case r::missing_title:
    return "missing_title";
  case r::missing_body:
    return "missing_body";
  case r::invalid_task_status:
    return "invalid_task_status";
  case r::missing_acceptance_section:
    return "missing_acceptance_section";
  case r::generic_acceptance:
    return "generic_acceptance";
  case r::generic_next_action:
    return "generic_next_action";
  case r::missing_acceptance_fact:
    return "missing_acceptance_fact";
  case r::missing_next_action_fact:
    return "missing_next_action_fact";
  case r::invalid_mandatory_fact:
    return "invalid_mandatory_fact";
  case r::missing_owning_plan:
    return "missing_owning_plan";
  case r::missing_anchor_plan:
    return "missing_anchor_plan";
  case r::invalid_owning_plan:
    return "invalid_owning_plan";
  case r::invalid_anchor_plan:
    return "invalid_anchor_plan";
  case r::missing_product_spec:
    return "missing_product_spec";
  case r::missing_tech_spec:
    return "missing_tech_spec";
  case r::missing_roadmap:
    return "missing_roadmap";
  case r::missing_test_spec:
    return "missing_test_spec";
  case r::missing_locked_decision:
    return "missing_locked_decision";
  case r::missing_dependency:
    return "missing_dependency";
  case r::missing_touch:
    return "missing_touch";
  case r::absent_validation_gates:
    return "absent_validation_gates";
  case r::uncovered_required_scenario:
    return "uncovered_required_scenario";
  case r::stale_fact:
    return "stale_fact";
  case r::contradictory_mandatory_fact:
    return "contradictory_mandatory_fact";
  case r::unresolved_citation:
    return "unresolved_citation";
  case r::invalid_locked_decision:
    return "invalid_locked_decision";
  case r::unresolved_question:
    return "unresolved_question";
  case r::invalid_dependency:
    return "invalid_dependency";
  case r::invalid_touch:
    return "invalid_touch";
  case r::inactive_claim:
    return "inactive_claim";
  case r::invalid_validation_gate:
    return "invalid_validation_gate";
  case r::missing_provenance:
    return "missing_provenance";
  case r::stale_mandatory_evidence:
    return "stale_mandatory_evidence";
  }
  return "unknown";
}

auto compile_task(const task_input& input) -> task_packet {
  using r = readiness_reason;
  std::vector<readiness_reason> reasons;

  // The sequence below IS the emission order. Do not reorder it, do not sort
  // the result, and do not "group related reasons together" — the orchestrator
  // reads this array positionally when it explains a refusal.
  if (trim(input.title).empty()) {
    append_reason(reasons, r::missing_title);
  }
  if (trim(input.body).empty()) {
    append_reason(reasons, r::missing_body);
  }
  {
    static constexpr std::string_view open_statuses[] = {"todo", "doing"};
    if (!status_in(input.status, open_statuses)) {
      append_reason(reasons, r::invalid_task_status);
    }
  }
  if (!extract_exact_section(input.body, "## Acceptance Criteria").has_value()) {
    append_reason(reasons, r::missing_acceptance_section);
  }
  if (generic_acceptance(input.acceptance_criteria)) {
    append_reason(reasons, r::generic_acceptance);
  }
  if (generic_next_action(input.next_action)) {
    append_reason(reasons, r::generic_next_action);
  }
  if (input.owning_plans.empty()) {
    append_reason(reasons, r::missing_owning_plan);
  }
  if (input.anchor_plans.empty()) {
    append_reason(reasons, r::missing_anchor_plan);
  }
  if (!has_kind(input.citations, "product_spec")) {
    append_reason(reasons, r::missing_product_spec);
  }
  if (!has_kind(input.citations, "tech_spec")) {
    append_reason(reasons, r::missing_tech_spec);
  }
  if (!has_kind(input.citations, "roadmap")) {
    append_reason(reasons, r::missing_roadmap);
  }
  if (!has_kind(input.citations, "test_spec")) {
    append_reason(reasons, r::missing_test_spec);
  }
  for (const auto& citation : input.citations) {
    if (citation.required && (!section_locator(citation.locator) || !evidence_current(citation))) {
      append_reason(reasons, r::unresolved_citation);
      break;
    }
  }
  if (input.decisions.empty()) {
    append_reason(reasons, r::missing_locked_decision);
  }
  // Root tasks intentionally have no dependency edge. A declared unfinished
  // dependency remains `invalid_dependency`; an empty list adds no reason.
  if (input.touches.empty()) {
    append_reason(reasons, r::missing_touch);
  }
  if (input.validation_gates.empty()) {
    append_reason(reasons, r::absent_validation_gates);
  }
  if (const auto fact = mandatory_boolean_fact(input.facts, "acceptance_complete"); !fact.has_value()) {
    append_reason(reasons, r::missing_acceptance_fact);
  } else if (!*fact) {
    append_reason(reasons, r::invalid_mandatory_fact);
  }
  if (const auto fact = mandatory_boolean_fact(input.facts, "next_action_exact"); !fact.has_value()) {
    append_reason(reasons, r::missing_next_action_fact);
  } else if (!*fact) {
    append_reason(reasons, r::invalid_mandatory_fact);
  }
  if (input.scenarios.empty()) {
    append_reason(reasons, r::uncovered_required_scenario);
  }
  for (const auto& scenario : input.scenarios) {
    if (scenario.required && !scenario.covered) {
      append_reason(reasons, r::uncovered_required_scenario);
      break;
    }
  }
  for (const auto& fact : input.facts) {
    if (fact.source_digest.empty() || fact.current_digest.empty() || fact.source_digest != fact.current_digest ||
        (!fact.materializer_version.empty() && fact.materializer_version != fact.current_materializer_version) ||
        fact.freshness != "current") {
      append_reason(reasons, r::stale_fact);
      break;
    }
  }
  if (contradictory(input.facts)) {
    append_reason(reasons, r::contradictory_mandatory_fact);
  }
  validate_evidence_classes(reasons, input);

  return {.input     = input,
          .canonical = canonical_task(input, true),
          .digest    = digest(canonical_task(input, false)),
          .reasons   = std::move(reasons)};
}

auto assemble_task(db::connection& conn, std::int64_t task_id) -> std::expected<task_packet, packet_error> {
  auto task = conn.prepare("select title,coalesce(body,''),coalesce(next_action,''),plan_id,status "
                           "from tasks where id=?");
  if (!task) {
    return std::unexpected(packet_error::query_failed);
  }
  if (!task->bind_int64(1, task_id)) {
    return std::unexpected(packet_error::query_failed);
  }
  auto step = task->step();
  if (!step) {
    return std::unexpected(packet_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(packet_error::task_not_found);
  }
  const auto title       = task->column_text(0);
  const auto body        = task->column_text(1);
  const auto next_action = task->column_text(2);
  const auto plan_id     = task->is_null(3) ? std::optional<std::int64_t>{} : std::optional{task->column_int64(3)};
  const auto task_status = task->column_text(4);

  const auto acceptance = extract_exact_section(body, "## Acceptance Criteria");

  std::vector<evidence> owning;
  std::vector<evidence> anchors;
  if (plan_id.has_value()) {
    auto owner_row =
        one_row_evidence(conn, "select 'plan',id,'plan:'||id,'',status,'plan:'||id,title from plans where id=?", *plan_id);
    if (!owner_row) {
      return std::unexpected(owner_row.error());
    }
    if (owner_row->has_value()) {
      owning.push_back(std::move(**owner_row));
    }
    // The anchor is the owning plan's PARENT when it has one, else the plan a
    // `derives-from` edge points at WITHIN THE SAME SCOPE, else the plan
    // itself. The scope equality in the subquery is load-bearing: a
    // cross-scope derives-from edge must not silently retarget the anchor.
    auto anchor_row =
        one_row_evidence(conn,
                         "select 'plan',p.id,'plan:'||p.id,'',p.status,'plan:'||p.id,p.title from plans owner "
                         "join plans p on p.id=coalesce(owner.parent_plan_id,(select el.to_id from entity_links el "
                         "join plans parent on parent.id=el.to_id where el.from_kind='plan' and el.from_id=owner.id "
                         "and el.to_kind='plan' and el.relationship='derives-from' and parent.scope_kind=owner.scope_kind "
                         "and coalesce(parent.scope_id,0)=coalesce(owner.scope_id,0) limit 1),owner.id) where owner.id=?",
                         *plan_id);
    if (!anchor_row) {
      return std::unexpected(anchor_row.error());
    }
    if (anchor_row->has_value()) {
      anchors.push_back(std::move(**anchor_row));
    }
  }

  auto citations = citation_evidence(conn, task_id);
  if (!citations) {
    return std::unexpected(citations.error());
  }
  auto decisions = linked_evidence(conn, task_id, "decision", "cites",
                                   "select 'decision',id,'decision:'||id,body,status,'decision:'||id,title "
                                   "from decisions where id=?");
  if (!decisions) {
    return std::unexpected(decisions.error());
  }
  auto questions = linked_evidence(conn, task_id, "question", "addresses",
                                   "select 'question',id,'question:'||id,coalesce(answer_body,body,title),status,"
                                   "'question:'||id,title from questions where id=?");
  if (!questions) {
    return std::unexpected(questions.error());
  }

  const std::int64_t anchor_plan_id = anchors.size() == 1 ? anchors[0].id : plan_id.value_or(0);

  auto scenarios = scenario_evidence(conn, task_id, anchor_plan_id);
  if (!scenarios) {
    return std::unexpected(scenarios.error());
  }
  // A dependency's `done` status is REWRITTEN to `satisfied` by the query, so
  // the evidence-class walk accepts it. Any other status flows through
  // unchanged and reports `invalid_dependency`.
  auto dependencies = linked_evidence(conn, task_id, "task", "depends-on",
                                      "select 'dependency',id,'task:'||id,title,"
                                      "case when status='done' then 'satisfied' else status end,"
                                      "'task:'||id,title from tasks where id=?");
  if (!dependencies) {
    return std::unexpected(dependencies.error());
  }
  auto touches = path_evidence(conn, task_id);
  if (!touches) {
    return std::unexpected(touches.error());
  }
  auto claims = claim_evidence(conn, task_id);
  if (!claims) {
    return std::unexpected(claims.error());
  }
  auto facts = fact_evidence(conn, task_id);
  if (!facts) {
    return std::unexpected(facts.error());
  }

  task_input input{.task_id             = task_id,
                   .status              = task_status,
                   .title               = title,
                   .body                = body,
                   .next_action         = next_action,
                   .acceptance_criteria = std::string{acceptance.value_or(std::string_view{})},
                   .owning_plans        = std::move(owning),
                   .anchor_plans        = std::move(anchors),
                   .citations           = std::move(*citations),
                   .decisions           = std::move(*decisions),
                   .questions           = std::move(*questions),
                   .scenarios           = std::move(*scenarios),
                   .dependencies        = std::move(*dependencies),
                   .touches             = std::move(*touches),
                   .claims              = std::move(*claims),
                   .validation_gates    = gate_evidence(task_id, body),
                   .facts               = std::move(*facts)};
  return compile_task(input);
}

auto planning_role_name(planning_role value) -> std::string_view {
  switch (value) {
  case planning_role::planner:
    return "planner";
  case planning_role::spec_reviewer:
    return "spec_reviewer";
  case planning_role::ingestor:
    return "ingestor";
  case planning_role::orchestrator:
    return "orchestrator";
  }
  return "unknown";
}

auto planning_reason_name(planning_reason reason) -> std::string_view {
  using r = planning_reason;
  switch (reason) {
  case r::missing_goal:
    return "missing_goal";
  case r::missing_scope_facts:
    return "missing_scope_facts";
  case r::missing_source_artifacts:
    return "missing_source_artifacts";
  case r::missing_required_outputs:
    return "missing_required_outputs";
  case r::missing_artifact_digests:
    return "missing_artifact_digests";
  case r::non_current_artifacts:
    return "non_current_artifacts";
  case r::missing_reviewed_artifacts:
    return "missing_reviewed_artifacts";
  case r::missing_strict_preview:
    return "missing_strict_preview";
  case r::invalid_strict_preview:
    return "invalid_strict_preview";
  case r::missing_review_rubric:
    return "missing_review_rubric";
  case r::missing_coverage:
    return "missing_coverage";
  case r::incomplete_coverage:
    return "incomplete_coverage";
  case r::missing_locked_decisions:
    return "missing_locked_decisions";
  case r::invalid_locked_decisions:
    return "invalid_locked_decisions";
  case r::missing_apply_boundary:
    return "missing_apply_boundary";
  case r::invalid_apply_boundary:
    return "invalid_apply_boundary";
  }
  return "unknown";
}

auto compile_planning(const planning_input& input) -> planning_packet {
  using r = planning_reason;
  std::vector<planning_reason> reasons;

  // Same discipline as `compile_task`: this sequence IS the emission order.
  if (trim(input.goal).empty()) {
    append_reason(reasons, r::missing_goal);
  }
  if (!input.artifacts.empty() && !planning_artifacts_current(input.artifacts)) {
    append_reason(reasons, r::non_current_artifacts);
  }
  if (!input.decisions.empty() && !planning_decisions_accepted(input.decisions)) {
    append_reason(reasons, r::invalid_locked_decisions);
  }

  switch (input.role) {
  case planning_role::planner:
    if (input.scope_facts.empty()) {
      append_reason(reasons, r::missing_scope_facts);
    }
    if (input.artifacts.empty()) {
      append_reason(reasons, r::missing_source_artifacts);
    }
    if (input.required_outputs.empty()) {
      append_reason(reasons, r::missing_required_outputs);
    }
    break;
  case planning_role::spec_reviewer:
    if (!has_four_current_artifacts(input.artifacts)) {
      append_reason(reasons, r::missing_artifact_digests);
    }
    if (input.strict_preview.empty()) {
      append_reason(reasons, r::missing_strict_preview);
    }
    if (!input.strict_preview.empty() && !planning_gate_accepted(input.strict_preview)) {
      append_reason(reasons, r::invalid_strict_preview);
    }
    if (trim(input.review_rubric_version).empty()) {
      append_reason(reasons, r::missing_review_rubric);
    }
    break;
  case planning_role::ingestor:
    if (!has_four_current_artifacts(input.artifacts)) {
      append_reason(reasons, r::missing_artifact_digests);
    }
    if (!has_four_reviewed_artifacts(input.artifacts)) {
      append_reason(reasons, r::missing_reviewed_artifacts);
    }
    if (input.strict_preview.empty()) {
      append_reason(reasons, r::missing_strict_preview);
    }
    if (!input.strict_preview.empty() && !planning_gate_accepted(input.strict_preview)) {
      append_reason(reasons, r::invalid_strict_preview);
    }
    if (input.coverage.empty()) {
      append_reason(reasons, r::missing_coverage);
    }
    if (!input.coverage.empty() && !planning_coverage_complete(input.coverage)) {
      append_reason(reasons, r::incomplete_coverage);
    }
    if (input.decisions.empty()) {
      append_reason(reasons, r::missing_locked_decisions);
    }
    if (input.apply_boundary.empty()) {
      append_reason(reasons, r::missing_apply_boundary);
    }
    if (!input.apply_boundary.empty() && !planning_gate_accepted(input.apply_boundary)) {
      append_reason(reasons, r::invalid_apply_boundary);
    }
    break;
  case planning_role::orchestrator:
    if (input.scope_facts.empty()) {
      append_reason(reasons, r::missing_scope_facts);
    }
    break;
  }

  return {.input     = input,
          .canonical = canonical_planning(input, true),
          .digest    = digest(canonical_planning(input, false)),
          .reasons   = std::move(reasons)};
}

auto assemble_planning(db::connection& conn, planning_role role, std::int64_t anchor_plan_id)
    -> std::expected<planning_packet, packet_error> {
  auto plan = conn.prepare("select title,coalesce(summary,''),scope_kind,coalesce(scope_id,0) from plans where id=?");
  if (!plan) {
    return std::unexpected(packet_error::query_failed);
  }
  if (!plan->bind_int64(1, anchor_plan_id)) {
    return std::unexpected(packet_error::query_failed);
  }
  auto step = plan->step();
  if (!step) {
    return std::unexpected(packet_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(packet_error::plan_not_found);
  }
  const auto title      = plan->column_text(0);
  const auto goal       = plan->column_text(1);
  const auto scope_kind = plan->column_text(2);
  const auto scope_id   = plan->column_int64(3);

  std::vector<evidence> scope_facts;
  {
    const auto scope_text   = std::format("{}:{}", scope_kind, scope_id);
    const auto scope_digest = digest(scope_text);
    evidence   item;
    item.kind           = "scope";
    item.id             = scope_id;
    item.locator        = "plan:scope";
    item.text           = scope_text;
    item.display_label  = title;
    item.source_digest  = scope_digest;
    item.current_digest = scope_digest;
    item.status         = "ready";
    item.provenance     = std::format("plan:{}", anchor_plan_id);
    scope_facts.push_back(std::move(item));
  }

  auto artifacts =
      plan_linked_evidence(conn, anchor_plan_id, "artifact",
                           "select kind,id,coalesce(source_path,'artifact:'||id),coalesce(body,''),status,'artifact:'||id,title "
                           "from artifacts where id=?");
  if (!artifacts) {
    return std::unexpected(artifacts.error());
  }
  auto questions = plan_linked_evidence(conn, anchor_plan_id, "question",
                                        "select 'question',id,'question:'||id,coalesce(answer_body,body,title),status,"
                                        "'question:'||id,title from questions where id=?");
  if (!questions) {
    return std::unexpected(questions.error());
  }
  auto decisions =
      plan_linked_evidence(conn, anchor_plan_id, "decision",
                           "select 'decision',id,'decision:'||id,body,status,'decision:'||id,title from decisions where id=?");
  if (!decisions) {
    return std::unexpected(decisions.error());
  }

  auto coverage = planning_coverage_evidence(conn, anchor_plan_id);
  if (!coverage) {
    return std::unexpected(coverage.error());
  }

  // `constraints` and `decisions` are the SAME rows, matching the oracle's
  // `PlanningInput{.constraints = decisions, ..., .decisions = decisions}` —
  // an arena slice reused twice. This tree copies once (`constraints`, which
  // is evaluated first in field-declaration order) then moves the original
  // into `decisions` last.
  planning_input input{
      .role                  = role,
      .goal                  = goal,
      .scope_facts           = std::move(scope_facts),
      .artifacts             = std::move(*artifacts),
      .questions             = std::move(*questions),
      .constraints           = *decisions,
      .required_outputs      = fixed_outputs(),
      .strict_preview        = {},
      .coverage              = std::move(*coverage),
      .decisions             = std::move(*decisions),
      .review_rubric_version = "spec-review-v1",
      .apply_boundary        = {},
  };
  return compile_planning(input);
}

auto render_text(const task_packet& packet) -> std::string {
  std::string out = std::format("task packet {}: {}\n", packet.input.task_id, packet.ready() ? "ready" : "not_ready");
  out.append(std::format("digest: {}\n", packet.digest));
  if (!packet.reasons.empty()) {
    out.append("reasons:\n");
    for (const auto reason : packet.reasons) {
      out.append("- ");
      out.append(reason_name(reason));
      out.push_back('\n');
    }
  }
  out.append(packet.canonical);
  out.push_back('\n');
  return out;
}

auto render_json(const task_packet& packet) -> std::string {
  const auto& in  = packet.input;
  std::string out = "{\"policy_version\":";
  append_json_string(out, policy_version);
  out.append(std::format(",\"ready\":{},\"input\":{{\"task_id\":{},\"status\":", packet.ready(), in.task_id));
  append_json_string(out, in.status);
  out.append(",\"title\":");
  append_json_string(out, in.title);
  out.append(",\"body\":");
  append_json_string(out, in.body);
  out.append(",\"next_action\":");
  append_json_string(out, in.next_action);
  out.append(",\"acceptance_criteria\":");
  append_json_string(out, in.acceptance_criteria);
  // The `input` object lists evidence in LOADER order, unsorted — only the
  // canonical body sorts. A consumer diffing two `input` blocks is looking at
  // the order the rows came out of SQL, which every loader pins with an
  // explicit `order by`.
  const auto emit = [&out](std::string_view name, const std::vector<evidence>& values) {
    out.append(",\"");
    out.append(name);
    out.append("\":[");
    bool first = true;
    for (const auto& item : values) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      out.append("{\"kind\":");
      append_json_string(out, item.kind);
      out.append(std::format(",\"id\":{},\"locator\":", item.id));
      append_json_string(out, item.locator);
      out.append(",\"text\":");
      append_json_string(out, item.text);
      out.append(",\"display_label\":");
      append_json_string(out, item.display_label);
      out.append(",\"source_digest\":");
      append_json_string(out, item.source_digest);
      out.append(",\"current_digest\":");
      append_json_string(out, item.current_digest);
      out.append(std::format(",\"required\":{},\"covered\":{}", item.required, item.covered));
      out.append(",\"status\":");
      append_json_string(out, item.status);
      out.append(",\"provenance\":");
      append_json_string(out, item.provenance);
      out.append(",\"materializer_version\":");
      append_json_string(out, item.materializer_version);
      out.append(",\"current_materializer_version\":");
      append_json_string(out, item.current_materializer_version);
      out.append(",\"freshness\":");
      append_json_string(out, item.freshness);
      out.push_back('}');
    }
    out.push_back(']');
  };
  emit("owning_plans", in.owning_plans);
  emit("anchor_plans", in.anchor_plans);
  emit("citations", in.citations);
  emit("decisions", in.decisions);
  emit("questions", in.questions);
  emit("scenarios", in.scenarios);
  emit("dependencies", in.dependencies);
  emit("touches", in.touches);
  emit("claims", in.claims);
  emit("validation_gates", in.validation_gates);
  emit("facts", in.facts);
  out.append("},\"canonical\":");
  append_json_string(out, packet.canonical);
  out.append(",\"digest\":");
  append_json_string(out, packet.digest);
  out.append(",\"reasons\":[");
  bool first = true;
  for (const auto reason : packet.reasons) {
    if (!first) {
      out.push_back(',');
    }
    first = false;
    append_json_string(out, reason_name(reason));
  }
  out.append("]}\n");
  return out;
}

} // namespace planar::engine::ingest::packet
