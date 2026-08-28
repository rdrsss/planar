/// @file feedback_triage.cpp
/// @brief Implementation of `planar.engine.planning.feedback_triage`.
///
/// Ported from `zig/src/engine/planning/feedback_triage.zig`. Where the two
/// trees could plausibly diverge, the oracle's behaviour was measured rather
/// than assumed; each such point carries a comment naming what was measured.
module;

module planar.engine.planning.feedback_triage;

import std;
import planar.db;
import planar.json_text;

namespace planar::engine::planning {

using json_text::json_string;

namespace {

/// @brief The slug the oracle requires a finding's plan to carry.
///
/// Not configurable and not derived: `findingPlan` compares the joined plan
/// slug against this literal and answers `different_feedback_plan` for
/// anything else. A fixture built on a plain plan refuses here, which is why
/// every interesting arm of this family needs a plan created with exactly
/// this slug.
constexpr std::string_view k_feedback_plan_slug = "planar-feedback";

/// @brief The projection shared by `show` and `list`.
///
/// Transcribed from the oracle's `select_sql`. Two joins carry meaning: `qp`
/// resolves a QUESTION finding's plan through a `derives-from` entity link
/// (questions have no `plan_id` column), and `dup` re-renders the duplicate
/// target as a finding ref rather than exposing the internal triage id.
constexpr std::string_view k_select_columns =
    "select ft.id, case when ft.finding_task_id is not null then 'task:'||ft.finding_task_id else "
    "'question:'||ft.finding_question_id end, coalesce(t.plan_id,qp.to_id), "
    "ft.severity,ft.disposition,ft.reproduction_status, case when dup.finding_task_id is not null then "
    "'task:'||dup.finding_task_id when dup.finding_question_id is not null then "
    "'question:'||dup.finding_question_id end,ft.evidence_summary,ft.created_at,ft.updated_at "
    "from feedback_triage ft "
    "left join tasks t on t.id=ft.finding_task_id "
    "left join entity_links qp on qp.from_kind='question' and qp.from_id=ft.finding_question_id "
    "and qp.to_kind='plan' and qp.relationship='derives-from' "
    "left join feedback_triage dup on dup.id=ft.duplicate_of_triage_id";

/// @brief Parse a finding id exactly as `cliapp::parse_int64_zig` does.
///
/// Reproduced, not re-derived: a leading `+`/`-` is consumed, the first and
/// last characters must both be digits (so `1_0` parses and `10_` / `_10` do
/// not), and interior underscores are skipped. Any divergence from that
/// helper is a bug in this copy, and the module's tests pin the three
/// spellings that distinguish it from a plain `std::from_chars`.
auto parse_id_zig(std::string_view body) -> std::optional<std::int64_t> {
  bool negative = false;
  if (!body.empty() && (body.front() == '+' || body.front() == '-')) {
    negative = body.front() == '-';
    body.remove_prefix(1);
  }
  const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
  if (body.empty() || !is_digit(body.front()) || !is_digit(body.back())) {
    return std::nullopt;
  }
  std::string digits;
  digits.reserve(body.size() + 1);
  if (negative) {
    digits.push_back('-');
  }
  for (const char c : body) {
    if (c == '_') {
      continue;
    }
    if (!is_digit(c)) {
      return std::nullopt;
    }
    digits.push_back(c);
  }
  std::int64_t value   = 0;
  const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value, 10);
  if (ec != std::errc{} || ptr != digits.data() + digits.size()) {
    return std::nullopt;
  }
  return value;
}

auto opt_text(const db::statement& stmt, int index) -> std::optional<std::string> {
  return stmt.is_null(index) ? std::optional<std::string>{} : std::optional<std::string>{stmt.column_text(index)};
}

auto opt_int(const db::statement& stmt, int index) -> std::optional<std::int64_t> {
  return stmt.is_null(index) ? std::optional<std::int64_t>{} : std::optional<std::int64_t>{stmt.column_int64(index)};
}

/// @brief Decode one row of `k_select_columns`.
///
/// An unparseable enum column is `query_failed`, matching the oracle: the
/// table's CHECK constraints make it unreachable short of external corruption.
auto read_row(db::statement& stmt) -> std::expected<feedback_triage, feedback_triage_error> {
  const auto severity     = feedback_severity_from_text(stmt.column_text(3));
  const auto disposition  = feedback_disposition_from_text(stmt.column_text(4));
  const auto reproduction = feedback_reproduction_from_text(stmt.column_text(5));
  if (!severity.has_value() || !disposition.has_value() || !reproduction.has_value()) {
    return std::unexpected(feedback_triage_error::query_failed);
  }
  return feedback_triage{
      .id                  = stmt.column_int64(0),
      .finding             = stmt.column_text(1),
      .plan_id             = opt_int(stmt, 2),
      .severity            = *severity,
      .disposition         = *disposition,
      .reproduction_status = *reproduction,
      .duplicate_of        = opt_text(stmt, 6),
      .evidence_summary    = opt_text(stmt, 7),
      .created_at          = stmt.column_text(8),
      .updated_at          = stmt.column_text(9),
  };
}

/// @brief Resolve the feedback plan a finding belongs to, enforcing the slug.
///
/// The two arms differ structurally, not just in their table. A TASK carries
/// `plan_id` directly, so "how many plans" is always one. A QUESTION reaches
/// its plan through `entity_links`, so the count is a real aggregate and
/// `ambiguous_feedback_plan` is reachable for a question and unreachable for a
/// task. Reproduced rather than unified.
/// @return The plan id, or the first failing check.
auto finding_plan(db::connection& conn, feedback_finding_ref ref) -> std::expected<std::int64_t, feedback_triage_error> {
  const std::string_view sql =
      ref.kind == feedback_finding_kind::task
          ? "select t.plan_id,p.slug from tasks t left join plans p on p.id=t.plan_id where t.id=?"
          : "select count(el.to_id),min(el.to_id),min(p.slug) from questions q left join entity_links el on "
            "el.from_kind='question' and el.from_id=q.id and el.to_kind='plan' and el.relationship='derives-from' "
            "left join plans p on p.id=el.to_id where q.id=? group by q.id";
  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(feedback_triage_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, ref.id); !bound) {
    return std::unexpected(feedback_triage_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(feedback_triage_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(feedback_triage_error::not_found);
  }

  const bool is_question = ref.kind == feedback_finding_kind::question;
  const int  plan_col    = is_question ? 1 : 0;
  const int  slug_col    = is_question ? 2 : 1;
  const auto link_count  = is_question ? stmt->column_int64(0) : std::int64_t{1};

  if (link_count == 0 || stmt->is_null(plan_col)) {
    return std::unexpected(feedback_triage_error::missing_feedback_plan);
  }
  if (link_count > 1) {
    return std::unexpected(feedback_triage_error::ambiguous_feedback_plan);
  }
  if (stmt->is_null(slug_col)) {
    return std::unexpected(feedback_triage_error::missing_feedback_plan);
  }
  if (stmt->column_text(slug_col) != k_feedback_plan_slug) {
    return std::unexpected(feedback_triage_error::different_feedback_plan);
  }
  return stmt->column_int64(plan_col);
}

} // namespace

auto parse_finding_ref(std::string_view raw) -> std::expected<feedback_finding_ref, feedback_triage_error> {
  const auto colon = raw.find(':');
  if (colon == std::string_view::npos) {
    return std::unexpected(feedback_triage_error::invalid_input);
  }
  const auto            kind_text = raw.substr(0, colon);
  feedback_finding_kind kind{};
  if (kind_text == "task") {
    kind = feedback_finding_kind::task;
  } else if (kind_text == "question") {
    kind = feedback_finding_kind::question;
  } else {
    return std::unexpected(feedback_triage_error::invalid_input);
  }

  // The oracle parses the id with Zig's `std.fmt.parseInt(i64, .., 10)`, which
  // accepts a leading sign and `_` digit separators that `std::from_chars`
  // does not. This tree already has ONE answer to "which integers does this
  // CLI accept" — `cliapp::parse_int64_zig` — and its contract is reproduced
  // here rather than imported: `planar.cliapp.args` imports `cli11`, and an
  // engine module reaching for a command-tree dependency to borrow a parsing
  // primitive is the layer inversion `cmake/architecture.cmake` rejects.
  // Keeping the SEMANTICS identical is the part that matters; a second,
  // drifting answer is what would actually hurt. Anchored by a test that
  // pins both spellings against the shared helper's documented behaviour.
  const auto id = parse_id_zig(raw.substr(colon + 1));
  if (!id.has_value()) {
    return std::unexpected(feedback_triage_error::invalid_input);
  }
  const auto value = *id;
  if (value < 1) {
    return std::unexpected(feedback_triage_error::invalid_input);
  }
  return feedback_finding_ref{.kind = kind, .id = value};
}

auto finding_ref_to_text(feedback_finding_ref ref) -> std::string {
  return std::format("{}:{}", ref.kind == feedback_finding_kind::task ? "task" : "question", ref.id);
}

auto feedback_severity_from_text(std::string_view s) -> std::optional<feedback_severity> {
  if (s == "info") {
    return feedback_severity::info;
  }
  if (s == "low") {
    return feedback_severity::low;
  }
  if (s == "medium") {
    return feedback_severity::medium;
  }
  if (s == "high") {
    return feedback_severity::high;
  }
  if (s == "critical") {
    return feedback_severity::critical;
  }
  return std::nullopt;
}

auto feedback_severity_to_text(feedback_severity s) -> std::string_view {
  switch (s) {
  case feedback_severity::info:
    return "info";
  case feedback_severity::low:
    return "low";
  case feedback_severity::medium:
    return "medium";
  case feedback_severity::high:
    return "high";
  case feedback_severity::critical:
    return "critical";
  }
  return "info"; // unreachable
}

auto feedback_disposition_from_text(std::string_view s) -> std::optional<feedback_disposition> {
  if (s == "untriaged") {
    return feedback_disposition::untriaged;
  }
  if (s == "needs-reproduction") {
    return feedback_disposition::needs_reproduction;
  }
  if (s == "accepted") {
    return feedback_disposition::accepted;
  }
  if (s == "retained-question") {
    return feedback_disposition::retained_question;
  }
  if (s == "dismissed") {
    return feedback_disposition::dismissed;
  }
  if (s == "reported-external") {
    return feedback_disposition::reported_external;
  }
  if (s == "duplicate") {
    return feedback_disposition::duplicate;
  }
  return std::nullopt;
}

auto feedback_disposition_to_text(feedback_disposition d) -> std::string_view {
  switch (d) {
  case feedback_disposition::untriaged:
    return "untriaged";
  case feedback_disposition::needs_reproduction:
    return "needs-reproduction";
  case feedback_disposition::accepted:
    return "accepted";
  case feedback_disposition::retained_question:
    return "retained-question";
  case feedback_disposition::dismissed:
    return "dismissed";
  case feedback_disposition::reported_external:
    return "reported-external";
  case feedback_disposition::duplicate:
    return "duplicate";
  }
  return "untriaged"; // unreachable
}

auto feedback_reproduction_from_text(std::string_view s) -> std::optional<feedback_reproduction> {
  if (s == "not-run") {
    return feedback_reproduction::not_run;
  }
  if (s == "reproduced") {
    return feedback_reproduction::reproduced;
  }
  if (s == "not-reproduced") {
    return feedback_reproduction::not_reproduced;
  }
  if (s == "inconclusive") {
    return feedback_reproduction::inconclusive;
  }
  return std::nullopt;
}

auto feedback_reproduction_to_text(feedback_reproduction r) -> std::string_view {
  switch (r) {
  case feedback_reproduction::not_run:
    return "not-run";
  case feedback_reproduction::reproduced:
    return "reproduced";
  case feedback_reproduction::not_reproduced:
    return "not-reproduced";
  case feedback_reproduction::inconclusive:
    return "inconclusive";
  }
  return "not-run"; // unreachable
}

auto entity_scope(db::connection& conn, feedback_finding_ref ref)
    -> std::expected<std::optional<std::string>, feedback_triage_error> {
  const std::string_view table = ref.kind == feedback_finding_kind::task ? "tasks" : "questions";
  auto                   stmt  = conn.prepare(std::format("select scope_kind, scope_id from {} where id=?", table));
  if (!stmt) {
    return std::unexpected(feedback_triage_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, ref.id); !bound) {
    return std::unexpected(feedback_triage_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(feedback_triage_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(feedback_triage_error::not_found);
  }

  const auto kind = stmt->column_text(0);
  if (kind == "global") {
    return std::optional<std::string>{};
  }
  const auto scope_id = stmt->column_int64(1);

  // Only `repo` resolves against `projects`; every other non-global kind
  // resolves against `associations`. The oracle branches on `repo` alone
  // rather than enumerating the kinds, so an unexpected kind lands in the
  // association arm instead of erroring. Reproduced.
  const bool             is_repo   = kind == "repo";
  const std::string_view ref_table = is_repo ? "projects" : "associations";
  const std::string_view prefix    = is_repo ? "repo:" : "assoc:";

  auto slug_stmt = conn.prepare(std::format("select slug from {} where id=?", ref_table));
  if (!slug_stmt) {
    return std::unexpected(feedback_triage_error::query_failed);
  }
  if (auto bound = slug_stmt->bind_int64(1, scope_id); !bound) {
    return std::unexpected(feedback_triage_error::query_failed);
  }
  auto slug_step = slug_stmt->step();
  if (!slug_step) {
    return std::unexpected(feedback_triage_error::query_failed);
  }
  // A dangling scope id is `query_failed` here, NOT `not_found`: the oracle
  // reserves `not_found` for a missing FINDING and treats a finding pointing
  // at a vanished scope row as a broken database.
  if (*slug_step != db::step_result::row) {
    return std::unexpected(feedback_triage_error::query_failed);
  }
  return std::optional<std::string>{std::format("{}{}", prefix, slug_stmt->column_text(0))};
}

auto show_feedback_triage(db::connection& conn, feedback_finding_ref ref)
    -> std::expected<feedback_triage, feedback_triage_error> {
  const auto sql  = std::format("{} where {}=?", k_select_columns,
                                ref.kind == feedback_finding_kind::task ? "ft.finding_task_id" : "ft.finding_question_id");
  auto       stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(feedback_triage_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, ref.id); !bound) {
    return std::unexpected(feedback_triage_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(feedback_triage_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(feedback_triage_error::not_found);
  }
  return read_row(*stmt);
}

auto list_feedback_triage(db::connection& conn, const feedback_triage_list_filter& filter)
    -> std::expected<std::vector<feedback_triage>, feedback_triage_error> {
  std::string sql = std::format("{} where 1=1", k_select_columns);
  if (filter.plan_id.has_value()) {
    sql += " and coalesce(t.plan_id,qp.to_id)=?";
  }
  if (filter.severity.has_value()) {
    sql += " and ft.severity=?";
  }
  if (filter.disposition.has_value()) {
    sql += " and ft.disposition=?";
  }
  sql += " order by coalesce(t.plan_id,qp.to_id), ft.updated_at desc, ft.id";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(feedback_triage_error::query_failed);
  }
  int idx = 1;
  if (filter.plan_id.has_value()) {
    if (auto bound = stmt->bind_int64(idx++, *filter.plan_id); !bound) {
      return std::unexpected(feedback_triage_error::query_failed);
    }
  }
  if (filter.severity.has_value()) {
    if (auto bound = stmt->bind_text(idx++, feedback_severity_to_text(*filter.severity)); !bound) {
      return std::unexpected(feedback_triage_error::query_failed);
    }
  }
  if (filter.disposition.has_value()) {
    if (auto bound = stmt->bind_text(idx++, feedback_disposition_to_text(*filter.disposition)); !bound) {
      return std::unexpected(feedback_triage_error::query_failed);
    }
  }

  std::vector<feedback_triage> rows;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(feedback_triage_error::query_failed);
    }
    if (*step == db::step_result::done) {
      break;
    }
    auto row = read_row(*stmt);
    if (!row) {
      return std::unexpected(row.error());
    }
    rows.push_back(std::move(*row));
  }
  return rows;
}

auto set_feedback_triage(db::connection& conn, feedback_finding_ref finding, const feedback_triage_set_args& args)
    -> std::expected<feedback_triage, feedback_triage_error> {
  const auto plan = finding_plan(conn, finding);
  if (!plan) {
    return std::unexpected(plan.error());
  }

  // `duplicate` and `--duplicate-of` entail each other in BOTH directions.
  // The oracle expresses this as one inequality over the two booleans, so
  // `duplicate` without a target and a target without `duplicate` are the
  // same error rather than two messages.
  if ((args.disposition == feedback_disposition::duplicate) != args.duplicate_of.has_value()) {
    return std::unexpected(feedback_triage_error::invalid_input);
  }

  std::optional<std::int64_t> duplicate_id;
  if (args.duplicate_of.has_value()) {
    const auto target = *args.duplicate_of;
    if (target.kind == finding.kind && target.id == finding.id) {
      return std::unexpected(feedback_triage_error::duplicate_cycle);
    }
    const auto target_plan = finding_plan(conn, target);
    if (!target_plan) {
      return std::unexpected(target_plan.error());
    }
    if (*target_plan != *plan) {
      return std::unexpected(feedback_triage_error::different_feedback_plan);
    }
    // An UNTRIAGED duplicate target is `invalid_input`, not `not_found`: the
    // target must already have a triage row for its id to be storable.
    const auto target_row = show_feedback_triage(conn, target);
    if (!target_row) {
      if (target_row.error() == feedback_triage_error::not_found) {
        return std::unexpected(feedback_triage_error::invalid_input);
      }
      return std::unexpected(target_row.error());
    }
    duplicate_id = target_row->id;

    // Walk the target's duplicate chain to its end, rejecting a cycle back to
    // the finding being written. The table's CHECK only catches self-reference
    // at one hop, so a two-hop cycle would otherwise be storable.
    std::optional<feedback_finding_ref> cursor;
    if (target_row->duplicate_of.has_value()) {
      const auto parsed = parse_finding_ref(*target_row->duplicate_of);
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      cursor = *parsed;
    }
    while (cursor.has_value()) {
      if (cursor->kind == finding.kind && cursor->id == finding.id) {
        return std::unexpected(feedback_triage_error::duplicate_cycle);
      }
      const auto row = show_feedback_triage(conn, *cursor);
      if (!row) {
        return std::unexpected(row.error());
      }
      if (row->duplicate_of.has_value()) {
        const auto parsed = parse_finding_ref(*row->duplicate_of);
        if (!parsed) {
          return std::unexpected(parsed.error());
        }
        cursor = *parsed;
      } else {
        cursor.reset();
      }
    }
  }

  constexpr std::string_view k_upsert =
      "insert into feedback_triage "
      "(finding_task_id,finding_question_id,severity,disposition,reproduction_status,duplicate_of_triage_id,"
      "evidence_summary) values (?,?,?,?,?,?,?) on conflict do update set severity=excluded.severity, "
      "disposition=excluded.disposition, reproduction_status=excluded.reproduction_status, "
      "duplicate_of_triage_id=excluded.duplicate_of_triage_id, evidence_summary=excluded.evidence_summary, "
      "updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now')";
  auto stmt = conn.prepare(k_upsert);
  if (!stmt) {
    return std::unexpected(feedback_triage_error::query_failed);
  }

  const bool is_task = finding.kind == feedback_finding_kind::task;
  auto       ok      = true;
  ok                 = ok && (is_task ? stmt->bind_int64(1, finding.id).has_value() : stmt->bind_null(1).has_value());
  ok                 = ok && (is_task ? stmt->bind_null(2).has_value() : stmt->bind_int64(2, finding.id).has_value());
  ok                 = ok && stmt->bind_text(3, feedback_severity_to_text(args.severity)).has_value();
  ok                 = ok && stmt->bind_text(4, feedback_disposition_to_text(args.disposition)).has_value();
  ok                 = ok && stmt->bind_text(5, feedback_reproduction_to_text(args.reproduction)).has_value();
  ok = ok && (duplicate_id.has_value() ? stmt->bind_int64(6, *duplicate_id).has_value() : stmt->bind_null(6).has_value());
  // `bind_text` writes an empty string as `''`, never SQL NULL, so an absent
  // evidence summary MUST take the `bind_null` arm to land as NULL.
  ok = ok && (args.evidence.has_value() ? stmt->bind_text(7, *args.evidence).has_value() : stmt->bind_null(7).has_value());
  if (!ok) {
    return std::unexpected(feedback_triage_error::query_failed);
  }
  if (auto step = stmt->step(); !step) {
    return std::unexpected(feedback_triage_error::query_failed);
  }
  return show_feedback_triage(conn, finding);
}

auto render_text(const feedback_triage& row) -> std::string {
  return std::format("{}\n  severity: {}\n  disposition: {}\n  reproduction: {}\n  duplicate-of: {}\n  evidence: {}\n",
                     row.finding, feedback_severity_to_text(row.severity), feedback_disposition_to_text(row.disposition),
                     feedback_reproduction_to_text(row.reproduction_status),
                     row.duplicate_of.has_value() ? std::string_view{*row.duplicate_of} : std::string_view{"-"},
                     row.evidence_summary.has_value() ? std::string_view{*row.evidence_summary} : std::string_view{"-"});
}

auto render_json(const feedback_triage& row) -> std::string {
  const auto opt_str = [](const std::optional<std::string>& v) -> std::string {
    return v.has_value() ? json_string(*v) : std::string{"null"};
  };
  return std::format(R"({{"id":{},"finding":{},"plan_id":{},"severity":"{}","disposition":"{}","reproduction_status":"{}",)"
                     R"("duplicate_of":{},"evidence_summary":{},"created_at":{},"updated_at":{}}})",
                     row.id, json_string(row.finding),
                     row.plan_id.has_value() ? std::format("{}", *row.plan_id) : std::string{"null"},
                     feedback_severity_to_text(row.severity), feedback_disposition_to_text(row.disposition),
                     feedback_reproduction_to_text(row.reproduction_status), opt_str(row.duplicate_of),
                     opt_str(row.evidence_summary), json_string(row.created_at), json_string(row.updated_at));
}

auto render_list_text(std::span<const feedback_triage> rows) -> std::string {
  if (rows.empty()) {
    return "(no feedback triage)\n";
  }
  std::string out;
  for (const auto& row : rows) {
    out += std::format("{:<18} {:<8} {:<20} {}\n", row.finding, feedback_severity_to_text(row.severity),
                       feedback_disposition_to_text(row.disposition), feedback_reproduction_to_text(row.reproduction_status));
  }
  return out;
}

auto render_list_json(std::span<const feedback_triage> rows) -> std::string {
  std::string out = "[";
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += render_json(rows[i]);
  }
  out += "]";
  return out;
}

} // namespace planar::engine::planning
