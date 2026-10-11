/// @file packet.cpp
/// @brief Implementation of `planar.engine.ingest.packet`. See packet.cppm for
/// the bucket-placement rationale and the readiness reasons with their order.

module planar.engine.ingest.packet;

import std;
import planar.db;
import planar.coordination;
import planar.json_text;
import planar.engine.ingest.materialize;

namespace planar::engine::ingest::packet {

namespace mz = planar::engine::ingest::materialize;
namespace co = planar::coordination;
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

// =========================================================================
// Predicates
// =========================================================================

/// @brief `sha256(bytes)` as 64 lowercase hex characters.
///
/// Delegates to `materialize`'s implementation rather than carrying a second
/// copy; the two modules share a bucket, so this needs no new dependency.
[[nodiscard]] auto digest(std::string_view bytes) -> std::string {
  return mz::sha256_hex(bytes);
}

[[nodiscard]] auto status_in(std::string_view status, std::span<const std::string_view> accepted) -> bool {
  return std::ranges::find(accepted, status) != accepted.end();
}

/// @brief Append a reason unless it is already present.
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
  if (auto c = a.status <=> b.status; c != 0) {
    return c < 0;
  }
  if (auto c = a.provenance <=> b.provenance; c != 0) {
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
    out.append(",\"status\":");
    append_json_string(out, item.status);
    out.append(",\"provenance\":");
    append_json_string(out, item.provenance);
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
  append_evidence_field(out, "owning_plans", input.owning_plans, include_display_labels);
  append_references_field(out, input.references, include_display_labels);
  append_evidence_field(out, "dependencies", input.dependencies, include_display_labels);
  append_evidence_field(out, "open_questions", input.open_questions, include_display_labels);
  append_evidence_field(out, "proposed_decisions", input.proposed_decisions, include_display_labels);
  append_evidence_field(out, "claims", input.claims, include_display_labels);
  out.push_back('}');
  return out;
}

// =========================================================================
// Loaders
// =========================================================================

/// @brief Materialize one evidence row from a 7-column result whose columns
/// are (kind, id, locator, text, status, provenance, display_label).
///
/// `source_digest` and `current_digest` are BOTH set to `digest(text)`, so a
/// row loaded this way is current by construction.
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
/// Every query passed declares exactly one placeholder; one to three are
/// accepted so a future query does not silently bind short.
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

/// @brief Evidence for each of `ids`, loaded by `entity_sql` with the id bound
/// to its placeholder. An id whose row has vanished is skipped.
[[nodiscard]] auto evidence_for_ids(db::connection& conn, const std::vector<std::int64_t>& ids, std::string_view entity_sql)
    -> std::expected<std::vector<evidence>, packet_error> {
  std::vector<evidence> out;
  for (const auto target : ids) {
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

/// @brief The tasks this one directly `depends-on`, whatever their status, as
/// display evidence.
///
/// The row's status is the dependency task's own `tasks.status`. Which of them
/// block is decided by `co::unfinished_dependencies`, not here. A self-link is
/// left out, as it is there.
[[nodiscard]] auto dependency_evidence(db::connection& conn, std::int64_t task_id)
    -> std::expected<std::vector<evidence>, packet_error> {
  auto ids = conn.prepare("select to_id from entity_links where from_kind='task' and from_id=? "
                          "and to_kind='task' and relationship='depends-on' and to_id != from_id order by to_id");
  if (!ids || !ids->bind_int64(1, task_id)) {
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
  return evidence_for_ids(conn, targets,
                          "select 'dependency',id,'task:'||id,title,status,'task:'||id,title from tasks where id=?");
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

/// @brief Active claims, with an EXPIRED lease reported as `expired`.
///
/// The WHERE clause admits only `status='active'` rows, and the CASE then
/// re-derives `active` vs `expired` from `lease_expires_at`. Only an `active`
/// row (a live lease) holds a `doing` task back; an `expired` row stays visible
/// as evidence that the task can be recovered.
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

} // namespace

// ===========================================================================
// Public surface
// ===========================================================================

auto reason_name(readiness_reason reason) -> std::string_view {
  using r = readiness_reason;
  switch (reason) {
  case r::missing_title:
    return "missing_title";
  case r::invalid_task_status:
    return "invalid_task_status";
  case r::missing_owning_plan:
    return "missing_owning_plan";
  case r::unresolved_citation:
    return "unresolved_citation";
  case r::blocked_by_open_task:
    return "blocked_by_open_task";
  case r::open_question_linked:
    return "open_question_linked";
  case r::proposed_decision_linked:
    return "proposed_decision_linked";
  }
  return "unknown";
}

namespace {

/// @brief Whether the task can be taken by an agent, judged on status and claims.
///
/// `todo` is claimable. `doing` is claimable only when no claim holds a live
/// lease: a stale, expired or absent claim is recovery, a live one is not.
/// Every other status is not.
[[nodiscard]] auto claimable(const task_input& input) -> bool {
  if (input.status == "todo") {
    return true;
  }
  if (input.status == "doing") {
    return std::ranges::none_of(input.claims, [](const evidence& claim) { return claim.status == "active"; });
  }
  return false;
}

} // namespace

auto compile_task(const task_input& input) -> task_packet {
  using r = readiness_reason;
  std::vector<readiness_reason> reasons;

  // The sequence below IS the emission order documented in packet.cppm. Readers
  // select a reason by name; keep the order stable so renderings are too.
  if (trim(input.title).empty()) {
    append_reason(reasons, r::missing_title);
  }
  if (!claimable(input)) {
    append_reason(reasons, r::invalid_task_status);
  }
  if (input.owning_plans.empty()) {
    append_reason(reasons, r::missing_owning_plan);
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
  if (!input.blocking_dependencies.empty()) {
    append_reason(reasons, r::blocked_by_open_task);
  }
  if (!input.open_questions.empty()) {
    append_reason(reasons, r::open_question_linked);
  }
  if (!input.proposed_decisions.empty()) {
    append_reason(reasons, r::proposed_decision_linked);
  }

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

  std::vector<evidence> owning;
  if (plan_id.has_value()) {
    auto owner_row =
        one_row_evidence(conn, "select 'plan',id,'plan:'||id,'',status,'plan:'||id,title from plans where id=?", *plan_id);
    if (!owner_row) {
      return std::unexpected(owner_row.error());
    }
    if (owner_row->has_value()) {
      owning.push_back(std::move(**owner_row));
    }
  }

  auto references = reference_evidence(conn, task_id);
  if (!references) {
    return std::unexpected(references.error());
  }
  auto dependencies = dependency_evidence(conn, task_id);
  if (!dependencies) {
    return std::unexpected(dependencies.error());
  }
  // The dependency, question and decision links are decided by
  // `planar.coordination`, the functions `recommend-strategy` rules 5 and 6
  // call, so the packet and the rule cannot disagree about which link blocks.
  auto blocking = co::unfinished_dependencies(conn, task_id);
  if (!blocking) {
    return std::unexpected(packet_error::query_failed);
  }
  auto open_question_ids = co::open_questions_linked(conn, task_id);
  if (!open_question_ids) {
    return std::unexpected(packet_error::query_failed);
  }
  auto open_questions = evidence_for_ids(conn, *open_question_ids,
                                         "select 'question',id,'question:'||id,coalesce(answer_body,body,title),status,"
                                         "'question:'||id,title from questions where id=?");
  if (!open_questions) {
    return std::unexpected(open_questions.error());
  }
  auto proposed_decision_ids = co::proposed_decisions_linked(conn, task_id);
  if (!proposed_decision_ids) {
    return std::unexpected(packet_error::query_failed);
  }
  auto proposed_decisions = evidence_for_ids(conn, *proposed_decision_ids,
                                             "select 'decision',id,'decision:'||id,body,status,'decision:'||id,title "
                                             "from decisions where id=?");
  if (!proposed_decisions) {
    return std::unexpected(proposed_decisions.error());
  }
  auto claims = claim_evidence(conn, task_id);
  if (!claims) {
    return std::unexpected(claims.error());
  }
  task_input input{.task_id               = task_id,
                   .status                = task_status,
                   .title                 = title,
                   .body                  = body,
                   .next_action           = next_action,
                   .owning_plans          = std::move(owning),
                   .references            = std::move(*references),
                   .dependencies          = std::move(*dependencies),
                   .blocking_dependencies = std::move(*blocking),
                   .open_questions        = std::move(*open_questions),
                   .proposed_decisions    = std::move(*proposed_decisions),
                   .claims                = std::move(*claims)};
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
      out.append(",\"status\":");
      append_json_string(out, item.status);
      out.append(",\"provenance\":");
      append_json_string(out, item.provenance);
      out.push_back('}');
    }
    out.push_back(']');
  };
  emit("owning_plans", in.owning_plans);
  out.append(",\"references\":[");
  for (std::size_t i = 0; i < in.references.size(); ++i) {
    if (i > 0) {
      out.push_back(',');
    }
    append_reference(out, in.references[i], true);
  }
  out.push_back(']');
  emit("dependencies", in.dependencies);
  emit("open_questions", in.open_questions);
  emit("proposed_decisions", in.proposed_decisions);
  emit("claims", in.claims);
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
