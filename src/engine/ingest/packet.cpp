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

/// @brief Whether the task cites an artifact of `kind`.
[[nodiscard]] auto has_kind(const std::vector<reference>& values, std::string_view kind) -> bool {
  return std::ranges::any_of(values, [&](const reference& v) { return v.kind == kind; });
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

/// @brief Append a reason unless it is already present. First occurrence wins,
/// which is what lets a class-walk reason land at an earlier position.
auto append_reason(std::vector<readiness_reason>& reasons, readiness_reason value) -> void {
  if (std::ranges::find(reasons, value) == reasons.end()) {
    reasons.push_back(value);
  }
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

/// @brief Append one `{"ref":...}` reference object. The `label` key is
/// written only when `include_label` is set, mirroring `display_label`.
auto append_reference(std::string& out, const reference& item, bool include_label) -> void {
  out.append("{\"ref\":");
  append_json_string(out, item.ref);
  out.append(",\"kind\":");
  append_json_string(out, item.kind);
  out.append(std::format(",\"id\":{},\"locator\":", item.id));
  append_json_string(out, item.locator);
  out.append(",\"digest\":");
  append_json_string(out, item.digest);
  if (include_label) {
    out.append(",\"label\":");
    append_json_string(out, item.label);
  }
  out.append(",\"status\":");
  append_json_string(out, item.status);
  out.push_back('}');
}

/// @brief Append the sorted `references` array to the canonical body.
auto append_references_field(std::string& out, const std::vector<reference>& values, bool include_labels) -> void {
  auto sorted = values;
  std::ranges::sort(sorted, [](const reference& a, const reference& b) {
    return std::tie(a.kind, a.id, a.locator, a.digest, a.status, a.label) <
           std::tie(b.kind, b.id, b.locator, b.digest, b.status, b.label);
  });
  out.append(",\"references\":[");
  bool first = true;
  for (const auto& item : sorted) {
    if (!first) {
      out.push_back(',');
    }
    first = false;
    append_reference(out, item, include_labels);
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
  append_references_field(out, input.references, include_display_labels);
  append_evidence_field(out, "decisions", input.decisions, include_display_labels);
  append_evidence_field(out, "questions", input.questions, include_display_labels);
  append_evidence_field(out, "scenarios", input.scenarios, include_display_labels);
  append_evidence_field(out, "dependencies", input.dependencies, include_display_labels);
  append_evidence_field(out, "touches", input.touches, include_display_labels);
  append_evidence_field(out, "claims", input.claims, include_display_labels);
  append_evidence_field(out, "validation_gates", input.validation_gates, include_display_labels);
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
  static constexpr std::string_view decision_ok[] = {"accepted", "ready"};
  static constexpr std::string_view question_ok[] = {"answered", "non_blocking", "ready"};
  static constexpr std::string_view dep_ok[]      = {"satisfied", "ready", "done"};
  static constexpr std::string_view touch_ok[]    = {"resolved", "ready"};
  static constexpr std::string_view claim_ok[]    = {"active"};
  static constexpr std::string_view gate_ok[]     = {"required", "ready"};

  const evidence_class classes[] = {
      {&input.owning_plans, readiness_reason::invalid_owning_plan, plan_ok},
      {&input.anchor_plans, readiness_reason::invalid_anchor_plan, plan_ok},
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
      if (!status_in(item.status, cls.accepted)) {
        append_reason(reasons, cls.reason);
      }
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
/// behaviour and it is why plan/decision/scenario/touch/claim evidence is
/// never stale.
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

/// @brief A label on one line: newlines and tabs become spaces, ends trimmed.
[[nodiscard]] auto one_line(std::string_view text) -> std::string {
  std::string out{trim(text)};
  std::ranges::replace_if(out, [](char c) { return c == '\n' || c == '\r' || c == '\t'; }, ' ');
  return out;
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

/// @brief The artifacts the task cites, as references.
///
/// One reference per `cites` link from the task to an artifact, ordered by the
/// artifact's kind and id. The digest is taken over the artifact's whole body
/// at read time, so a reference is current by construction and the packet
/// changes digest the moment a cited artifact does.
[[nodiscard]] auto reference_evidence(db::connection& conn, std::int64_t task_id)
    -> std::expected<std::vector<reference>, packet_error> {
  auto stmt = conn.prepare(R"(select a.kind,a.id,coalesce(a.body,''),a.title,a.status
from entity_links el
join artifacts a on a.id=el.to_id
where el.from_kind='task' and el.from_id=? and el.to_kind='artifact' and el.relationship='cites'
order by a.kind,a.id)");
  if (!stmt || !stmt->bind_int64(1, task_id)) {
    return std::unexpected(packet_error::query_failed);
  }
  std::vector<reference> out;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(packet_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    reference item;
    item.kind   = stmt->column_text(0);
    item.id     = stmt->column_int64(1);
    item.ref    = std::format("artifact:{}", item.id);
    item.digest = mz::source_digest("artifact", item.id, item.locator, stmt->column_text(2));
    item.label  = one_line(stmt->column_text(3));
    item.status = stmt->column_text(4);
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
  if (!has_kind(input.references, "product_spec")) {
    append_reason(reasons, r::missing_product_spec);
  }
  if (!has_kind(input.references, "tech_spec")) {
    append_reason(reasons, r::missing_tech_spec);
  }
  if (!has_kind(input.references, "roadmap")) {
    append_reason(reasons, r::missing_roadmap);
  }
  if (!has_kind(input.references, "test_spec")) {
    append_reason(reasons, r::missing_test_spec);
  }
  {
    static constexpr std::string_view citation_ok[] = {"active", "draft", "ready"};
    for (const auto& cited : input.references) {
      if (cited.digest.empty() || !status_in(cited.status, citation_ok)) {
        append_reason(reasons, r::unresolved_citation);
        break;
      }
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
  if (input.scenarios.empty()) {
    append_reason(reasons, r::uncovered_required_scenario);
  }
  for (const auto& scenario : input.scenarios) {
    if (scenario.required && !scenario.covered) {
      append_reason(reasons, r::uncovered_required_scenario);
      break;
    }
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

  auto references = reference_evidence(conn, task_id);
  if (!references) {
    return std::unexpected(references.error());
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
  task_input input{.task_id             = task_id,
                   .status              = task_status,
                   .title               = title,
                   .body                = body,
                   .next_action         = next_action,
                   .acceptance_criteria = std::string{acceptance.value_or(std::string_view{})},
                   .owning_plans        = std::move(owning),
                   .anchor_plans        = std::move(anchors),
                   .references          = std::move(*references),
                   .decisions           = std::move(*decisions),
                   .questions           = std::move(*questions),
                   .scenarios           = std::move(*scenarios),
                   .dependencies        = std::move(*dependencies),
                   .touches             = std::move(*touches),
                   .claims              = std::move(*claims),
                   .validation_gates    = gate_evidence(task_id, body)};
  return compile_task(input);
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
  if (!packet.input.references.empty()) {
    out.append(std::format("references (expand one with: planar task packet {} --expand <ref>):\n", packet.input.task_id));
    for (const auto& item : packet.input.references) {
      out.append(std::format("- {} {} {}\n", item.ref, item.digest.substr(0, 12), item.label));
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
  out.append(",\"references\":[");
  for (std::size_t i = 0; i < in.references.size(); ++i) {
    if (i > 0) {
      out.push_back(',');
    }
    append_reference(out, in.references[i], true);
  }
  out.push_back(']');
  emit("decisions", in.decisions);
  emit("questions", in.questions);
  emit("scenarios", in.scenarios);
  emit("dependencies", in.dependencies);
  emit("touches", in.touches);
  emit("claims", in.claims);
  emit("validation_gates", in.validation_gates);
  out.append("},\"digest\":");
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

namespace {

/// @brief The two parts of `artifact:<id>[#<heading>]`.
struct parsed_reference {
  std::int64_t id = 0;   ///< The artifact id.
  std::string  fragment; ///< The heading fragment; empty for the whole body.
};

/// @brief Splits `artifact:<id>[#<heading>]` into its parts.
/// @param ref The reference as written.
/// @return The parts, or `nullopt` when `ref` is malformed.
[[nodiscard]] auto parse_reference(std::string_view ref) -> std::optional<parsed_reference> {
  constexpr std::string_view prefix = "artifact:";
  if (!ref.starts_with(prefix)) {
    return std::nullopt;
  }
  ref.remove_prefix(prefix.size());
  const auto   hash    = ref.find('#');
  const auto   digits  = ref.substr(0, hash);
  std::int64_t id      = 0;
  const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), id);
  if (digits.empty() || ec != std::errc{} || end != digits.data() + digits.size() || id <= 0) {
    return std::nullopt;
  }
  parsed_reference out{.id = id, .fragment = {}};
  if (hash != std::string_view::npos) {
    out.fragment = std::string{trim(ref.substr(hash + 1))};
    if (out.fragment.empty()) {
      return std::nullopt;
    }
  }
  return out;
}

} // namespace

auto expand_reference(db::connection& conn, std::int64_t task_id, std::string_view ref)
    -> std::expected<expansion, expand_error> {
  {
    auto stmt = conn.prepare("select 1 from tasks where id=?");
    if (!stmt || !stmt->bind_int64(1, task_id)) {
      return std::unexpected(expand_error::query_failed);
    }
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(expand_error::query_failed);
    }
    if (*step != db::step_result::row) {
      return std::unexpected(expand_error::task_not_found);
    }
  }
  const auto parsed = parse_reference(ref);
  if (!parsed.has_value()) {
    return std::unexpected(expand_error::unknown_reference);
  }
  // The artifact must be one the task cites; a task is not a window onto every artifact.
  auto stmt = conn.prepare("select coalesce(a.body,'') from entity_links el join artifacts a on a.id=el.to_id "
                           "where el.from_kind='task' and el.from_id=? and el.to_kind='artifact' and el.to_id=? "
                           "and el.relationship='cites'");
  if (!stmt || !stmt->bind_int64(1, task_id) || !stmt->bind_int64(2, parsed->id)) {
    return std::unexpected(expand_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(expand_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(expand_error::unknown_reference);
  }
  const auto body = stmt->column_text(0);

  expansion out;
  out.ref = std::string{ref};
  if (parsed->fragment.empty()) {
    out.text   = body;
    out.digest = mz::source_digest("artifact", parsed->id, "body", out.text);
    return out;
  }
  const auto locator = std::format("#{}", parsed->fragment);
  const auto section = mz::artifact_section(body, locator);
  if (!section.has_value()) {
    return std::unexpected(expand_error::section_not_found);
  }
  out.text   = std::string{*section};
  out.digest = mz::source_digest("artifact", parsed->id, locator, out.text);
  return out;
}

auto render_expansion_text(const expansion& value) -> std::string {
  std::string out = value.text;
  if (out.empty() || out.back() != '\n') {
    out.push_back('\n');
  }
  return out;
}

auto render_expansion_json(const expansion& value) -> std::string {
  std::string out = "{\"ref\":";
  append_json_string(out, value.ref);
  out.append(",\"digest\":");
  append_json_string(out, value.digest);
  out.append(",\"text\":");
  append_json_string(out, value.text);
  out.append("}\n");
  return out;
}

} // namespace planar::engine::ingest::packet
