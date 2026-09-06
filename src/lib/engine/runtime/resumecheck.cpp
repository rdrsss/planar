/// @file resumecheck.cpp
/// @brief Implementation of `planar.engine.runtime.resumecheck` (plan 996,
/// task 6040). See resumecheck.cppm for the derived rule set and for why
/// only `validate` lives here.

module planar.engine.runtime.resumecheck;

import std;
import planar.db;
import planar.json_text;
import planar.engine.runtime.snapshot;
import planar.engine.runtime.session;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.handoff;

namespace planar::engine::runtime::resumecheck {

namespace {

namespace snap  = planar::engine::runtime::snapshot;
namespace sess  = planar::engine::runtime::session;
namespace agent = planar::engine::runtime::agentactivity;
namespace ho    = planar::engine::runtime::handoff;

/// @brief Section 3. Mirrors `resume.zig`'s `buildPlanPosition` raw SQL and
/// fold exactly, reading `plan_steps` directly rather than through
/// `planar.engine.planning.plan_step` (a sibling layer-2 bucket — see this
/// module's header on why the cross-bucket edge is avoided).
auto build_plan_position(db::connection& conn, std::optional<std::int64_t> plan_id_opt)
    -> std::expected<plan_position, resume_error> {
  if (!plan_id_opt.has_value()) {
    return plan_position{};
  }
  auto const plan_id = *plan_id_opt;

  plan_position out;
  out.plan_id = plan_id;

  {
    auto stmt = conn.prepare("select coalesce(title, '') from plans where id = ?");
    if (!stmt) {
      return std::unexpected(resume_error::query_failed);
    }
    if (auto b = stmt->bind_int64(1, plan_id); !b) {
      return std::unexpected(resume_error::query_failed);
    }
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(resume_error::query_failed);
    }
    if (*step == db::step_result::row) {
      out.plan_title = stmt->column_text(0);
    }
    // A dangling plan_id (row deleted out from under the task) leaves
    // plan_title empty and completed/current/remaining empty, matching
    // the oracle's early return on `.done`.
    if (*step != db::step_result::row) {
      return out;
    }
  }

  auto stmt = conn.prepare("select ordinal, coalesce(body, ''), coalesce(status, 'pending') "
                           "from plan_steps where plan_id = ? order by ordinal");
  if (!stmt) {
    return std::unexpected(resume_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, plan_id); !b) {
    return std::unexpected(resume_error::query_failed);
  }
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(resume_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    plan_step_summary s{
        .ordinal = stmt->column_int64(0),
        .body    = stmt->column_text(1),
        .status  = stmt->column_text(2),
    };
    if (s.status == "done" || s.status == "skipped") {
      out.completed.push_back(std::move(s));
    } else if (s.status == "in-progress") {
      out.current.push_back(std::move(s));
    } else {
      out.remaining.push_back(std::move(s));
    }
  }
  return out;
}

/// @brief Section 4. Mirrors `resume.zig`'s `buildOperationalPlane`,
/// reading `external_links` directly rather than through
/// `planar.engine.external.link` (see this module's header). Only the
/// columns the packet actually surfaces are selected; `remote_status` /
/// `remote_assignee` / `refresh_error` are always empty (decision 996).
auto build_operational_plane(db::connection& conn, std::int64_t task_id) -> std::expected<operational_plane, resume_error> {
  auto stmt = conn.prepare("select id, external_id, coalesce(external_url, ''), last_sync_status, "
                           "coalesce(last_synced_at, '') "
                           "from external_links where entity_kind = 'task' and entity_id = ? order by id");
  if (!stmt) {
    return std::unexpected(resume_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, task_id); !b) {
    return std::unexpected(resume_error::query_failed);
  }
  operational_plane out;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(resume_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    auto const sync_status = stmt->column_text(3);
    out.links.push_back(external_link_state{
        .link_id        = stmt->column_int64(0),
        .external_id    = stmt->column_text(1),
        .external_url   = stmt->column_text(2),
        .last_synced_at = stmt->column_text(4),
        .sync_status    = sync_status,
        .conflict       = sync_status == "conflict",
    });
  }
  return out;
}

/// @brief Section 6a. Mirrors `resume.zig`'s `buildDecisions` EXACTLY —
/// INNER JOIN on task-bound sessions only, deliberately NOT the more
/// permissive `decisions_for_task` (see this module's header).
auto build_decisions(db::connection& conn, std::int64_t task_id) -> std::expected<std::vector<decision_summary>, resume_error> {
  auto stmt = conn.prepare("select d.id, coalesce(d.title, ''), coalesce(d.status, '') "
                           "from decisions d "
                           "join sessions s on s.id = d.session_id "
                           "where s.task_id = ? "
                           "order by d.id");
  if (!stmt) {
    return std::unexpected(resume_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, task_id); !b) {
    return std::unexpected(resume_error::query_failed);
  }
  std::vector<decision_summary> out;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(resume_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    out.push_back(decision_summary{
        .id     = stmt->column_int64(0),
        .title  = stmt->column_text(1),
        .status = stmt->column_text(2),
    });
  }
  return out;
}

/// @brief Section 6b. Mirrors `resume.zig`'s `buildQuestions`: an
/// `entity_links` join in EITHER direction between the task and the
/// question, deduplicated.
auto build_questions(db::connection& conn, std::int64_t task_id) -> std::expected<std::vector<question_summary>, resume_error> {
  auto stmt = conn.prepare("select distinct q.id, coalesce(q.title, ''), coalesce(q.status, ''), "
                           "coalesce(q.answer_body, '') "
                           "from questions q "
                           "join entity_links el "
                           "  on (el.from_kind = 'task' and el.from_id = ? and el.to_kind = 'question' and el.to_id = q.id) "
                           "  or (el.to_kind = 'task' and el.to_id = ? and el.from_kind = 'question' and el.from_id = q.id) "
                           "order by q.id");
  if (!stmt) {
    return std::unexpected(resume_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, task_id); !b) {
    return std::unexpected(resume_error::query_failed);
  }
  if (auto b = stmt->bind_int64(2, task_id); !b) {
    return std::unexpected(resume_error::query_failed);
  }
  std::vector<question_summary> out;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(resume_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    out.push_back(question_summary{
        .id          = stmt->column_int64(0),
        .title       = stmt->column_text(1),
        .status      = stmt->column_text(2),
        .answer_body = stmt->column_text(3),
    });
  }
  return out;
}

/// @brief Section 7. Mirrors `resume.zig`'s `buildArtifacts`: an
/// `entity_links` join FROM the task TO the artifact only (one direction,
/// unlike questions).
auto build_artifacts(db::connection& conn, std::int64_t task_id) -> std::expected<std::vector<artifact_link>, resume_error> {
  auto stmt = conn.prepare("select a.id, coalesce(a.title, ''), coalesce(a.kind, ''), "
                           "coalesce(el.relationship, '') "
                           "from artifacts a "
                           "join entity_links el on el.to_kind = 'artifact' and el.to_id = a.id "
                           "where el.from_kind = 'task' and el.from_id = ? "
                           "order by a.id");
  if (!stmt) {
    return std::unexpected(resume_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, task_id); !b) {
    return std::unexpected(resume_error::query_failed);
  }
  std::vector<artifact_link> out;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(resume_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    out.push_back(artifact_link{
        .artifact_id  = stmt->column_int64(0),
        .title        = stmt->column_text(1),
        .kind         = stmt->column_text(2),
        .relationship = stmt->column_text(3),
    });
  }
  return out;
}

/// @brief Section 8, active-claim half. `agentactivity::list_claims_by_entity`
/// orders `claimed_at ASC` (a pre-existing divergence from the oracle's
/// `listByEntity`, which orders DESC — see this module's header for why it
/// is not touched here); the most-recent ACTIVE row is therefore the LAST
/// one found scanning ascending, not the first.
auto build_active_claim(db::connection& conn, std::int64_t task_id) -> std::expected<std::optional<active_claim_state>, resume_error> {
  auto rows = agent::list_claims_by_entity(conn, "task", task_id);
  if (!rows) {
    return std::unexpected(resume_error::query_failed);
  }
  std::optional<active_claim_state> found;
  for (auto const& c : *rows) {
    if (c.status != agent::claim_status::active) {
      continue;
    }
    found = active_claim_state{
        .claim_id      = c.id,
        .claim_token   = c.claim_token,
        .vendor        = c.vendor,
        .worktree_path = c.worktree_path.value_or(""),
        .repo_root     = c.repo_root.value_or(""),
        .branch        = c.branch.value_or(""),
    };
  }
  return found;
}

/// @brief Section 8, cold-start fallback. Mirrors `resume.zig`'s
/// `buildHandoffWorktree`.
auto build_handoff_worktree(db::connection& conn, std::int64_t task_id)
    -> std::expected<std::optional<handoff_worktree_state>, resume_error> {
  auto h = ho::get_latest_with_worktree_for_task(conn, task_id);
  if (!h) {
    return std::unexpected(resume_error::query_failed);
  }
  if (!h->has_value()) {
    return std::optional<handoff_worktree_state>{};
  }
  return std::optional<handoff_worktree_state>{handoff_worktree_state{
      .handoff_id    = (*h)->id,
      .worktree_path = (*h)->worktree_path.value_or(""),
      .repo_root     = (*h)->repo_root.value_or(""),
      .branch        = (*h)->branch.value_or(""),
  }};
}

} // namespace

auto validate(db::connection& conn, std::int64_t task_id) -> std::expected<validation_result, resume_error> {
  // Read the task's own columns directly rather than through
  // `planar.engine.planning.task`. That is the Zig original's explicit
  // choice ("keep this module independent of engine.planning.task"), and
  // here it is also what keeps this bucket at layer 2 with no
  // `engine_* -> engine_*` edge, which cmake/architecture.cmake FATALs on.
  auto stmt = conn.prepare("select coalesce(next_action, '') from tasks where id = ?");
  if (!stmt) {
    return std::unexpected(resume_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, task_id); !bound) {
    return std::unexpected(resume_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(resume_error::query_failed);
  }
  if (*stepped != db::step_result::row) {
    return std::unexpected(resume_error::not_found);
  }
  auto const next_action = stmt->column_text(0);

  validation_result result{.task_id = task_id, .resumable = false, .failures = {}};

  // Check 1 — next_action. A BYTE-LENGTH test, deliberately: a
  // next_action of a single space passes here and passes in the oracle.
  // See this module's header for the probe that established it.
  if (next_action.empty()) {
    result.failures.push_back(validation_failure{
        .check       = "next_action",
        .message     = "next_action is null",
        .remediation = std::format("planar task update {} --next-action \"<text>\"", task_id),
    });
  }

  // Check 2 — at least one TASK-SCOPED context snapshot. A session-level
  // snapshot (task_id NULL) does not satisfy this; `get_latest_for_task`
  // filters on `where task_id = ?`, which NULL never matches.
  auto const latest = snap::get_latest_for_task(conn, task_id);
  if (!latest) {
    return std::unexpected(resume_error::query_failed);
  }
  if (!latest->has_value()) {
    result.failures.push_back(validation_failure{
        .check       = "snapshot",
        .message     = "no context snapshot found",
        .remediation = std::format("planar capture snapshot --task {}", task_id),
    });
  }

  result.resumable = result.failures.empty();
  return result;
}

auto render_validate_json(const validation_result& result) -> std::string {
  std::string out =
      std::format("{{\"task_id\":{},\"resumable\":{},\"failures\":", result.task_id, result.resumable ? "true" : "false");
  // Empty failures serialize as `null`, NOT `[]`. Cross-binary parity with
  // the Go original is load-bearing here and the oracle comment says so.
  if (result.failures.empty()) {
    out += "null}\n";
    return out;
  }
  out += "[";
  bool first = true;
  for (auto const& failure : result.failures) {
    if (!first) {
      out += ",";
    }
    first = false;
    out += "{\"check\":";
    json_text::append_json_string(out, failure.check);
    out += ",\"message\":";
    json_text::append_json_string(out, failure.message);
    out += ",\"remediation\":";
    json_text::append_json_string(out, failure.remediation);
    out += "}";
  }
  out += "]}\n";
  return out;
}

auto render_validate_text(const validation_result& result) -> std::string {
  if (result.resumable) {
    return std::format("OK task:{} is resume-ready\n", result.task_id);
  }
  std::string out = std::format("FAIL task:{} is not resumable:\n", result.task_id);
  for (auto const& failure : result.failures) {
    // U+2192 RIGHTWARDS ARROW, spelled as UTF-8 bytes so the file's own
    // encoding cannot silently change the payload.
    out += std::format("  - {} \xe2\x86\x92 run: {}\n", failure.message, failure.remediation);
  }
  return out;
}

auto build_packet(db::connection& conn, std::int64_t task_id) -> std::expected<packet, resume_error> {
  packet out;

  // ---- Sections 1+2 — Identity + State -------------------------------
  {
    auto stmt = conn.prepare("select id, plan_id, coalesce(title, ''), coalesce(status, ''), "
                             "coalesce(scope_kind, 'global'), scope_id, coalesce(next_action, '') "
                             "from tasks where id = ?");
    if (!stmt) {
      return std::unexpected(resume_error::query_failed);
    }
    if (auto b = stmt->bind_int64(1, task_id); !b) {
      return std::unexpected(resume_error::query_failed);
    }
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(resume_error::query_failed);
    }
    if (*step != db::step_result::row) {
      return std::unexpected(resume_error::not_found);
    }
    auto const plan_id    = stmt->is_null(1) ? std::optional<std::int64_t>{} : std::optional{stmt->column_int64(1)};
    auto const title      = stmt->column_text(2);
    auto const status_txt = stmt->column_text(3);
    auto const scope_kind = stmt->column_text(4);
    auto const scope_id   = stmt->is_null(5) ? std::optional<std::int64_t>{} : std::optional{stmt->column_int64(5)};
    auto const next_action = stmt->column_text(6);

    out.ident = identity{
        .task_id    = stmt->column_int64(0),
        .plan_id    = plan_id,
        .title      = title,
        .status     = status_txt,
        .scope_kind = scope_kind,
        .scope_id   = scope_id,
    };
    out.st = state{
        .status      = status_txt,
        .next_action = next_action,
    };
  }

  // ---- Section 3 — Plan position --------------------------------------
  auto plan_pos = build_plan_position(conn, out.ident.plan_id);
  if (!plan_pos) {
    return std::unexpected(plan_pos.error());
  }
  out.plan = std::move(*plan_pos);

  // ---- Section 4 — Operational plane -----------------------------------
  auto op_plane = build_operational_plane(conn, task_id);
  if (!op_plane) {
    return std::unexpected(op_plane.error());
  }
  out.op_plane = std::move(*op_plane);

  // ---- Section 5 — Recent activity -------------------------------------
  auto entries = sess::recent_entries_for_task(conn, task_id, 50);
  if (!entries) {
    return std::unexpected(resume_error::query_failed);
  }
  for (auto const& e : *entries) {
    out.recent_activity.push_back(recent_entry{
        .session_id = e.session_id,
        .prefix     = e.prefix,
        .body       = e.body,
        .created_at = e.created_at,
    });
  }
  if (!entries->empty()) {
    out.st.last_action_at   = entries->front().created_at;
    out.st.last_action_body = entries->front().body;
  }

  // A context snapshot is the durable resume boundary. Prefer its body
  // over the session timeline so `resume --json` carries the exact
  // checkpoint that `resume validate` accepted. Timeline activity remains
  // the fallback for tasks whose latest snapshot has no narrative body.
  auto latest_snapshot = snap::get_latest_for_task(conn, task_id);
  if (!latest_snapshot) {
    return std::unexpected(resume_error::query_failed);
  }
  if (latest_snapshot->has_value() && !(*latest_snapshot)->body.empty()) {
    out.st.last_action_at   = (*latest_snapshot)->created_at;
    out.st.last_action_body = (*latest_snapshot)->body;
  }

  // ---- Section 6 — Decisions + questions --------------------------------
  auto decisions = build_decisions(conn, task_id);
  if (!decisions) {
    return std::unexpected(decisions.error());
  }
  out.decisions = std::move(*decisions);

  auto questions = build_questions(conn, task_id);
  if (!questions) {
    return std::unexpected(questions.error());
  }
  out.questions = std::move(*questions);

  // ---- Section 7 — Artifacts ---------------------------------------------
  auto artifacts = build_artifacts(conn, task_id);
  if (!artifacts) {
    return std::unexpected(artifacts.error());
  }
  out.artifacts = std::move(*artifacts);

  // ---- Section 8 — Audit footer -------------------------------------------
  auto sessions = sess::recent_sessions_for_task(conn, task_id, 1);
  if (!sessions) {
    return std::unexpected(resume_error::query_failed);
  }
  if (!sessions->empty()) {
    out.audit = audit_footer{
        .session_id = sessions->front().id,
        .vendor     = sessions->front().vendor,
        .started_at = sessions->front().started_at,
    };
  }

  // ---- Active claim — surfaces worktree state for cd-prefix ---------------
  auto active_claim = build_active_claim(conn, task_id);
  if (!active_claim) {
    return std::unexpected(active_claim.error());
  }
  out.active_claim = std::move(*active_claim);

  // ---- Handoff-fallback worktree — cold-start recovery path ---------------
  bool const need_fallback = out.active_claim.has_value() ? out.active_claim->worktree_path.empty() : true;
  if (need_fallback) {
    auto from_handoff = build_handoff_worktree(conn, task_id);
    if (!from_handoff) {
      return std::unexpected(from_handoff.error());
    }
    out.from_handoff = std::move(*from_handoff);
  }

  return out;
}

auto render_packet_json(const packet& p) -> std::string {
  std::string out = "{\"identity\":{\"task_id\":";
  out += std::format("{}", p.ident.task_id);
  out += ",\"plan_id\":";
  if (p.ident.plan_id.has_value()) {
    out += std::format("{}", *p.ident.plan_id);
  } else {
    out += "null";
  }
  out += ",\"title\":";
  json_text::append_json_string(out, p.ident.title);
  out += ",\"status\":";
  json_text::append_json_string(out, p.ident.status);
  out += ",\"scope_kind\":";
  json_text::append_json_string(out, p.ident.scope_kind);
  out += ",\"scope_id\":";
  if (p.ident.scope_id.has_value()) {
    out += std::format("{}", *p.ident.scope_id);
  } else {
    out += "null";
  }
  out += "},\"state\":{\"status\":";
  json_text::append_json_string(out, p.st.status);
  out += ",\"next_action\":";
  json_text::append_json_string(out, p.st.next_action);
  out += ",\"last_action_at\":";
  json_text::append_json_string(out, p.st.last_action_at);
  out += ",\"last_action_body\":";
  json_text::append_json_string(out, p.st.last_action_body);
  out += "},\"plan\":{\"plan_id\":";
  if (p.plan.plan_id.has_value()) {
    out += std::format("{}", *p.plan.plan_id);
  } else {
    out += "null";
  }
  out += ",\"plan_title\":";
  json_text::append_json_string(out, p.plan.plan_title);

  auto append_steps = [&out](std::vector<plan_step_summary> const& steps) {
    out += "[";
    bool first = true;
    for (auto const& s : steps) {
      if (!first) {
        out += ",";
      }
      first = false;
      out += std::format("{{\"ordinal\":{},\"body\":", s.ordinal);
      json_text::append_json_string(out, s.body);
      out += ",\"status\":";
      json_text::append_json_string(out, s.status);
      out += "}";
    }
    out += "]";
  };
  out += ",\"completed\":";
  append_steps(p.plan.completed);
  out += ",\"current\":";
  append_steps(p.plan.current);
  out += ",\"remaining\":";
  append_steps(p.plan.remaining);
  out += "},\"operational_plane\":{\"links\":[";
  {
    bool first = true;
    for (auto const& l : p.op_plane.links) {
      if (!first) {
        out += ",";
      }
      first = false;
      out += std::format("{{\"link_id\":{},\"external_id\":", l.link_id);
      json_text::append_json_string(out, l.external_id);
      out += ",\"external_url\":";
      json_text::append_json_string(out, l.external_url);
      out += ",\"remote_status\":";
      json_text::append_json_string(out, l.remote_status);
      out += ",\"remote_assignee\":";
      json_text::append_json_string(out, l.remote_assignee);
      out += ",\"last_synced_at\":";
      json_text::append_json_string(out, l.last_synced_at);
      out += ",\"sync_status\":";
      json_text::append_json_string(out, l.sync_status);
      out += std::format(",\"conflict\":{},\"refresh_error\":", l.conflict ? "true" : "false");
      json_text::append_json_string(out, l.refresh_error);
      out += "}";
    }
  }
  out += "],\"refresh_note\":";
  json_text::append_json_string(out, p.op_plane.refresh_note);
  out += "},\"recent_activity\":[";
  {
    bool first = true;
    for (auto const& e : p.recent_activity) {
      if (!first) {
        out += ",";
      }
      first = false;
      out += std::format("{{\"session_id\":{},\"prefix\":", e.session_id);
      json_text::append_json_string(out, e.prefix);
      out += ",\"body\":";
      json_text::append_json_string(out, e.body);
      out += ",\"created_at\":";
      json_text::append_json_string(out, e.created_at);
      out += "}";
    }
  }
  out += "],\"decisions\":[";
  {
    bool first = true;
    for (auto const& d : p.decisions) {
      if (!first) {
        out += ",";
      }
      first = false;
      out += std::format("{{\"id\":{},\"title\":", d.id);
      json_text::append_json_string(out, d.title);
      out += ",\"status\":";
      json_text::append_json_string(out, d.status);
      out += "}";
    }
  }
  out += "],\"questions\":[";
  {
    bool first = true;
    for (auto const& q : p.questions) {
      if (!first) {
        out += ",";
      }
      first = false;
      out += std::format("{{\"id\":{},\"title\":", q.id);
      json_text::append_json_string(out, q.title);
      out += ",\"status\":";
      json_text::append_json_string(out, q.status);
      out += ",\"answer_body\":";
      json_text::append_json_string(out, q.answer_body);
      out += "}";
    }
  }
  out += "],\"artifacts\":[";
  {
    bool first = true;
    for (auto const& a : p.artifacts) {
      if (!first) {
        out += ",";
      }
      first = false;
      out += std::format("{{\"artifact_id\":{},\"title\":", a.artifact_id);
      json_text::append_json_string(out, a.title);
      out += ",\"kind\":";
      json_text::append_json_string(out, a.kind);
      out += ",\"relationship\":";
      json_text::append_json_string(out, a.relationship);
      out += "}";
    }
  }
  out += "],\"audit\":";
  if (p.audit.has_value()) {
    out += std::format("{{\"session_id\":{},\"vendor\":", p.audit->session_id);
    json_text::append_json_string(out, p.audit->vendor);
    out += ",\"started_at\":";
    json_text::append_json_string(out, p.audit->started_at);
    out += "}";
  } else {
    out += "null";
  }
  out += ",\"active_claim\":";
  if (p.active_claim.has_value()) {
    out += std::format("{{\"claim_id\":{},\"claim_token\":", p.active_claim->claim_id);
    json_text::append_json_string(out, p.active_claim->claim_token);
    out += ",\"vendor\":";
    json_text::append_json_string(out, p.active_claim->vendor);
    out += ",\"worktree_path\":";
    json_text::append_json_string(out, p.active_claim->worktree_path);
    out += ",\"repo_root\":";
    json_text::append_json_string(out, p.active_claim->repo_root);
    out += ",\"branch\":";
    json_text::append_json_string(out, p.active_claim->branch);
    out += "}";
  } else {
    out += "null";
  }
  out += ",\"from_handoff\":";
  if (p.from_handoff.has_value()) {
    out += std::format("{{\"handoff_id\":{},\"worktree_path\":", p.from_handoff->handoff_id);
    json_text::append_json_string(out, p.from_handoff->worktree_path);
    out += ",\"repo_root\":";
    json_text::append_json_string(out, p.from_handoff->repo_root);
    out += ",\"branch\":";
    json_text::append_json_string(out, p.from_handoff->branch);
    out += "}";
  } else {
    out += "null";
  }
  out += "}\n";
  return out;
}

auto render_packet_text(const packet& p) -> std::string {
  std::string out;
  out += std::format("=== Resume Packet: task {} ===\n\n", p.ident.task_id);

  out += "## 1. Identity\n";
  out += std::format("  task:   {}  \"{}\"\n", p.ident.task_id, p.ident.title);
  out += std::format("  status: {}\n", p.ident.status);
  out += std::format("  scope:  {}", p.ident.scope_kind);
  if (p.ident.scope_id.has_value()) {
    out += std::format(":{}", *p.ident.scope_id);
  }
  out += "\n";
  if (p.ident.plan_id.has_value()) {
    out += std::format("  plan:   {}\n", *p.ident.plan_id);
  }
  out += "\n";

  out += "## 2. State\n";
  out += std::format("  status:      {}\n", p.st.status);
  if (!p.st.next_action.empty()) {
    out += std::format("  next_action: {}\n", p.st.next_action);
  } else {
    out += "  next_action: (not set)\n";
  }
  if (!p.st.last_action_at.empty()) {
    out += std::format("  last_action: {}  [{}]\n", p.st.last_action_body, p.st.last_action_at);
  }
  out += "\n";

  out += "## 3. Plan Position\n";
  if (p.plan.plan_id.has_value()) {
    out += std::format("  plan: {} \"{}\"\n", *p.plan.plan_id, p.plan.plan_title);
    out += std::format("  completed: {}  current: {}  remaining: {}\n", p.plan.completed.size(), p.plan.current.size(),
                       p.plan.remaining.size());
  } else {
    out += "  (no parent plan)\n";
  }
  out += "\n";

  out += "## 4. Operational Plane\n";
  if (!p.op_plane.refresh_note.empty()) {
    out += std::format("  note: {}\n", p.op_plane.refresh_note);
  }
  if (p.op_plane.links.empty()) {
    out += "  (no external links)\n";
  } else {
    for (auto const& l : p.op_plane.links) {
      out += std::format("  link {}: {} [{}]  {}\n", l.link_id, l.external_id, l.sync_status, l.external_url);
    }
  }
  out += "\n";

  out += std::format("## 5. Recent Activity ({} entries)\n", p.recent_activity.size());
  if (p.recent_activity.empty()) {
    out += "  (none)\n";
  }
  for (auto const& e : p.recent_activity) {
    out += std::format("  [{}]  session:{}  {}  \xe2\x80\x94 {}\n", e.prefix, e.session_id, e.created_at, e.body);
  }
  out += "\n";

  out += std::format("## 6. Decisions ({}) and Questions ({})\n", p.decisions.size(), p.questions.size());
  for (auto const& d : p.decisions) {
    out += std::format("  decision {}: \"{}\"  [{}]\n", d.id, d.title, d.status);
  }
  for (auto const& q : p.questions) {
    out += std::format("  question {}: \"{}\"  [{}]\n", q.id, q.title, q.status);
  }
  if (p.decisions.empty() && p.questions.empty()) {
    out += "  (none)\n";
  }
  out += "\n";

  out += std::format("## 7. Linked Artifacts ({})\n", p.artifacts.size());
  for (auto const& a : p.artifacts) {
    out += std::format("  {} artifact:{} \"{}\"  [{}]\n", a.relationship, a.artifact_id, a.title, a.kind);
  }
  if (p.artifacts.empty()) {
    out += "  (none)\n";
  }
  out += "\n";

  out += "## 8. Audit Footer\n";
  if (p.audit.has_value()) {
    out += std::format("  session: {}  vendor: {}  started: {}\n", p.audit->session_id, p.audit->vendor, p.audit->started_at);
  } else {
    out += "  (no prior session)\n";
  }
  if (p.active_claim.has_value()) {
    out += std::format("  active claim: {}  vendor: {}\n", p.active_claim->claim_id, p.active_claim->vendor);
    if (!p.active_claim->worktree_path.empty()) {
      out += std::format("  worktree: {}\n", p.active_claim->worktree_path);
      out += std::format("  cd: {}\n", p.active_claim->worktree_path);
    }
    if (!p.active_claim->branch.empty()) {
      out += std::format("  branch: {}\n", p.active_claim->branch);
    }
    if (!p.active_claim->repo_root.empty()) {
      out += std::format("  repo_root: {}\n", p.active_claim->repo_root);
    }
  }
  // Cold-start fallback: surfaced only when the active claim (if any)
  // carries no worktree path.
  bool const active_has_worktree = p.active_claim.has_value() && !p.active_claim->worktree_path.empty();
  if (p.from_handoff.has_value() && !active_has_worktree) {
    out += std::format("  from handoff: {}\n", p.from_handoff->handoff_id);
    if (!p.from_handoff->worktree_path.empty()) {
      out += std::format("  worktree: {}\n", p.from_handoff->worktree_path);
      out += std::format("  cd: {}\n", p.from_handoff->worktree_path);
    }
    if (!p.from_handoff->branch.empty()) {
      out += std::format("  branch: {}\n", p.from_handoff->branch);
    }
    if (!p.from_handoff->repo_root.empty()) {
      out += std::format("  repo_root: {}\n", p.from_handoff->repo_root);
    }
  }
  out += "\n";
  return out;
}

} // namespace planar::engine::runtime::resumecheck
