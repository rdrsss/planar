/// @file audit.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.audit`.
/// See audit.cppm for the two-exit-code and null-optional conventions.

module planar.cmd.planar.handlers.audit;

import std;
import planar.adapter;
import planar.cliapp.args;
import planar.db;
import planar.json_text;
import planar.engine.identity;
import planar.engine.runtime.audit_trail;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.sessioncommits;
import planar.engine.runtime.session;
import planar.engine.runtime.resumecheck;
import planar.engine.planning.decision;
import planar.engine.planning.task;
import planar.engine.external.link;
import planar.engine.external.system;
import planar.engine.external.sync;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.scope;
import planar.cmd.planar.handlers.ext_adapter_factory;

namespace planar::cmd::handlers {

namespace at   = engine::runtime::audit_trail;
namespace aa   = engine::runtime::agentactivity;
namespace sc   = engine::runtime::sessioncommits;
namespace xl   = engine::external::link;
namespace xsys = engine::external::system;
namespace xsy  = engine::external::sync;
namespace sess = engine::runtime::session;
namespace rck  = engine::runtime::resumecheck;
// `task.cppm` files its declarations directly in `planar::engine::planning`
// — there is no `::task` namespace to alias, unlike every other engine
// module aliased above.
namespace pt = engine::planning;

namespace {

/// @brief The row cap on both "Agent activity" sub-lists and on the
/// commits fold-in.
///
/// Ten each, from the oracle's `agent_activity_row_cap` and
/// `commit_row_cap`. Two separate constants there with the same value; one
/// here, because nothing distinguishes them and a second name would invite
/// them to drift apart for no reason.
constexpr std::int64_t k_fold_in_row_cap = 10;

/// @brief Left-align `text` in a field of `width`, never truncating.
///
/// The oracle's `{s:<12}` / `{d:<4}` pad to the width and OVERFLOW it when
/// the value is longer — a 20-character prefix prints all twenty
/// characters and pushes the rest of the line right. Reproduced: a
/// truncating implementation would silently lose entry-prefix text.
/// @param text The value.
/// @param width The minimum field width.
/// @return `text` padded on the right to at least `width`.
auto pad_right(std::string_view text, std::size_t width) -> std::string {
  std::string out{text};
  if (out.size() < width) {
    out.append(width - out.size(), ' ');
  }
  return out;
}

/// @brief Render the timeline as the oracle prints it without `--json`.
///
/// Header, then one line per entry. The header's task segment is `task:<id>`
/// when bound and the literal `(no task)` when not — note that the two arms
/// differ only in that segment, and both keep the same double-space
/// separators.
/// @param t The timeline.
/// @return The complete stdout payload including its trailing newline.
auto render_text(const at::session_timeline_result& t) -> std::string {
  std::string out;
  if (t.task_id.has_value()) {
    out = std::format("session {}  vendor: {}  task:{}  {}", t.session_id, t.vendor, *t.task_id, t.started_at);
  } else {
    out = std::format("session {}  vendor: {}  (no task)  {}", t.session_id, t.vendor, t.started_at);
  }
  out += t.ended_at.has_value() ? std::format(" → {}", *t.ended_at) : std::string{" → (active)"};
  out += "\n";
  for (auto const& e : t.entries) {
    out += std::format("  {}  [{}]  {}\n", pad_right(std::to_string(e.ordinal), 4), pad_right(e.prefix, 12), e.body);
  }
  return out;
}

/// @brief Render the timeline as the oracle's `--json` payload.
///
/// Key order is `id, vendor, started_at, task_id, ended_at, entries` — the
/// order of the anonymous struct the oracle's handler builds, which is NOT
/// the order of the engine's own result type. Unset optionals are OMITTED,
/// not emitted as `null`; see audit.cppm.
/// @param t The timeline.
/// @return The complete stdout payload including its trailing newline.
auto render_json(const at::session_timeline_result& t) -> std::string {
  std::string out = std::format("{{\"id\":{},\"vendor\":", t.session_id);
  out += json_text::json_string(t.vendor);
  out += ",\"started_at\":";
  out += json_text::json_string(t.started_at);
  if (t.task_id.has_value()) {
    out += std::format(",\"task_id\":{}", *t.task_id);
  }
  if (t.ended_at.has_value()) {
    out += ",\"ended_at\":";
    out += json_text::json_string(*t.ended_at);
  }
  out += ",\"entries\":[";
  for (std::size_t i = 0; i < t.entries.size(); ++i) {
    auto const& e = t.entries[i];
    if (i > 0) {
      out += ",";
    }
    out += std::format("{{\"ordinal\":{},\"prefix\":", e.ordinal);
    out += json_text::json_string(e.prefix);
    out += ",\"body\":";
    out += json_text::json_string(e.body);
    out += "}";
  }
  out += "]}\n";
  return out;
}

// =========================================================================
// `audit trail` — shared row types and the two handler-local reads
// =========================================================================

/// @brief A `sessions` row as both trail forms surface it.
///
/// Text columns are COALESCED to the empty string in SQL rather than kept
/// optional, because that is what the oracle's projection does and the
/// difference is visible: an empty `decided_at` renders as nothing at all
/// between two separators, leaving a run of spaces the reader sees.
struct session_row {
  std::int64_t id = 0; ///< The `sessions` row id.
  std::string  vendor;
  std::string  started_at;
  std::string  summary;
};

/// @brief A `decisions` row as the link form surfaces it.
struct decision_row {
  std::int64_t id = 0; ///< The `decisions` row id.
  std::string  title;
  std::string  status;
  std::string  decided_at;
};

/// @brief Every session attributable to one entity.
///
/// TWO DIFFERENT QUERIES, chosen on `entity_kind`, and the split is the
/// oracle's. For a TASK, a session counts if `sessions.task_id` names it OR
/// an `agent_work_claims` row on that task carries a `session_id` — the
/// second arm is how a `planar-agent claim` becomes visible here, and
/// dropping it makes the whole "sessions:" section of `audit trail --link`
/// silently empty for agent work. For every other kind there is no
/// `task_id` column to read, so the only path is `entity_links` traversed
/// in BOTH directions.
///
/// Failures degrade to an empty list at the call sites in the entity form
/// (the fold-in is best-effort there) and refuse in the link form, matching
/// the oracle's two different `catch` postures.
/// @param conn An open connection.
/// @param entity_kind The entity kind, verbatim.
/// @param entity_id The entity row id.
/// @return The rows newest-started first, or nullopt on a SQL failure.
auto load_sessions_for_entity(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id)
    -> std::optional<std::vector<session_row>> {
  auto stmt = (entity_kind == "task")
                  ? conn.prepare("select distinct s.id, coalesce(s.vendor,''), coalesce(s.started_at,''), "
                                 "coalesce(s.summary,'')\n"
                                 "from sessions s\n"
                                 "where s.task_id = ?\n"
                                 "   or s.id in (\n"
                                 "     select awc.session_id\n"
                                 "     from agent_work_claims awc\n"
                                 "     where awc.entity_kind = 'task' and awc.entity_id = ? and awc.session_id is not null\n"
                                 "   )\n"
                                 "order by s.started_at desc")
                  : conn.prepare("select distinct s.id, coalesce(s.vendor,''), coalesce(s.started_at,''), "
                                 "coalesce(s.summary,'')\n"
                                 "from sessions s where s.id in (\n"
                                 "  select to_id   from entity_links where from_kind = ? and from_id = ? and to_kind   = "
                                 "'session'\n"
                                 "  union\n"
                                 "  select from_id from entity_links where to_kind   = ? and to_id   = ? and from_kind = "
                                 "'session'\n"
                                 ")\n"
                                 "order by s.started_at desc");
  if (!stmt) {
    return std::nullopt;
  }
  bool bound = false;
  if (entity_kind == "task") {
    bound = stmt->bind_int64(1, entity_id).has_value() && stmt->bind_int64(2, entity_id).has_value();
  } else {
    bound = stmt->bind_text(1, entity_kind).has_value() && stmt->bind_int64(2, entity_id).has_value() &&
            stmt->bind_text(3, entity_kind).has_value() && stmt->bind_int64(4, entity_id).has_value();
  }
  if (!bound) {
    return std::nullopt;
  }

  std::vector<session_row> rows;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::nullopt;
    }
    if (*step != db::step_result::row) {
      return rows;
    }
    rows.push_back(session_row{
        .id         = stmt->column_int64(0),
        .vendor     = stmt->column_text(1),
        .started_at = stmt->column_text(2),
        .summary    = stmt->column_text(3),
    });
  }
}

/// @brief Decisions linked FROM one entity.
///
/// One direction only — `entity_links.from_kind/from_id` is the entity and
/// `to_kind = 'decision'`. Deliberately NOT symmetric with
/// `load_sessions_for_entity`'s union, because the oracle's query is not:
/// a decision that links TO the task does not appear. Reproduced, not
/// harmonised.
///
/// Ordered by `decisions.created_at desc`, which is not the `decided_at`
/// the renderer prints.
/// @param conn An open connection.
/// @param entity_kind The entity kind, verbatim.
/// @param entity_id The entity row id.
/// @return The rows, or nullopt on a SQL failure.
auto load_decisions_for_entity(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id)
    -> std::optional<std::vector<decision_row>> {
  auto stmt = conn.prepare("select d.id, coalesce(d.title,''), coalesce(d.status,''), coalesce(d.decided_at,'')\n"
                           "from decisions d\n"
                           "join entity_links el on el.to_kind = 'decision' and el.to_id = d.id\n"
                           "where el.from_kind = ? and el.from_id = ?\n"
                           "order by d.created_at desc");
  if (!stmt || !stmt->bind_text(1, entity_kind) || !stmt->bind_int64(2, entity_id)) {
    return std::nullopt;
  }
  std::vector<decision_row> rows;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::nullopt;
    }
    if (*step != db::step_result::row) {
      return rows;
    }
    rows.push_back(decision_row{
        .id         = stmt->column_int64(0),
        .title      = stmt->column_text(1),
        .status     = stmt->column_text(2),
        .decided_at = stmt->column_text(3),
    });
  }
}

/// @brief The three best-effort fold-ins both trail forms share.
struct fold_ins {
  std::vector<aa::action>     actions; ///< Recent agent actions on the entity.
  std::vector<aa::claim>      claims;  ///< Recent claim transitions on the entity.
  std::vector<sc::commit_row> commits; ///< Commits recorded against the entity's sessions.
};

/// @brief Load the fold-ins, degrading every failure to an empty list.
///
/// SILENT DEGRADE IS THE CONTRACT, not laziness: the oracle wraps each of
/// these in a `catch` that yields an empty slice, so a database missing the
/// agent tables renders a trail with no "Agent activity" section rather
/// than aborting the verb. An empty list and a failed query are
/// indistinguishable in the output, and that is deliberate on both sides.
/// @param conn An open connection.
/// @param entity_kind The entity kind, verbatim.
/// @param entity_id The entity row id.
/// @param sessions The already-loaded sessions, whose ids the commits read
/// spans. Passing them in rather than re-reading matches the link form,
/// which has them in hand and must not run the query twice.
/// @return The three lists.
auto load_fold_ins(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id,
                   std::span<const session_row> sessions) -> fold_ins {
  fold_ins out;
  if (auto actions = aa::recent_actions_for_entity(conn, entity_kind, entity_id, k_fold_in_row_cap)) {
    out.actions = std::move(*actions);
  }
  if (auto claims = aa::claim_transitions_for_entity(conn, entity_kind, entity_id, k_fold_in_row_cap)) {
    out.claims = std::move(*claims);
  }
  std::vector<std::int64_t> session_ids;
  session_ids.reserve(sessions.size());
  for (auto const& s : sessions) {
    session_ids.push_back(s.id);
  }
  if (auto commits = sc::list_for_sessions(conn, session_ids, k_fold_in_row_cap)) {
    out.commits = std::move(*commits);
  }
  return out;
}

// =========================================================================
// `audit trail` — rendering
// =========================================================================

/// @brief Append `,"key":<json-string>` when `value` is set; append nothing
/// when it is not.
///
/// The OMIT convention, used by `entries[]`, `agent_activity` and the link
/// form's `sessions`/`decisions`/`sync_events`. The commits renderer below
/// deliberately does NOT use this — see `append_commits_json`.
auto append_opt_string(std::string& out, std::string_view key, const std::optional<std::string>& value) -> void {
  if (!value.has_value()) {
    return;
  }
  out += std::format(",\"{}\":", key);
  out += json_text::json_string(*value);
}

/// @brief `append_opt_string`, for a value the caller has already narrowed
/// to a possibly-empty string.
auto append_nonempty_string(std::string& out, std::string_view key, std::string_view value) -> void {
  if (value.empty()) {
    return;
  }
  out += std::format(",\"{}\":", key);
  out += json_text::json_string(value);
}

/// @brief Render the `commits` array — the ONE place in this payload that
/// emits explicit `null`.
///
/// Every other list here omits unset optionals. This one does not, because
/// the oracle renders commits through `sessioncommits.writeJson`, a
/// different function with a different convention, and both conventions end
/// up in the same document. Preserved rather than harmonised.
/// @param out The payload under construction.
/// @param rows The commits; an EMPTY list emits nothing at all, not an
/// empty array — the whole key is omitted.
auto append_commits_json(std::string& out, std::span<const sc::commit_row> rows) -> void {
  if (rows.empty()) {
    return;
  }
  auto const opt_str = [](const std::optional<std::string>& v) {
    return v.has_value() ? json_text::json_string(*v) : std::string{"null"};
  };
  out += ",\"commits\":[";
  for (std::size_t i = 0; i < rows.size(); ++i) {
    auto const& r = rows[i];
    if (i > 0) {
      out += ",";
    }
    out += std::format("{{\"id\":{},\"session_id\":{},\"claim_id\":{},\"sha\":", r.id, r.session_id,
                       r.claim_id.has_value() ? std::to_string(*r.claim_id) : std::string{"null"});
    out += json_text::json_string(r.sha);
    out += ",\"repo_root\":" + opt_str(r.repo_root);
    out += ",\"branch\":" + opt_str(r.branch);
    out += ",\"subject\":" + opt_str(r.subject);
    out += ",\"author\":" + opt_str(r.author);
    out += ",\"committed_at\":" + opt_str(r.committed_at);
    out += ",\"recorded_at\":";
    out += json_text::json_string(r.recorded_at);
    out += "}";
  }
  out += "]";
}

/// @brief Render the `agent_activity` object, or NOTHING when both lists
/// are empty.
///
/// The all-empty case omits the KEY — it does not emit
/// `"agent_activity":{"actions":[],"claims":[]}`. But a payload with claims
/// and no actions DOES carry `"actions":[]`, so the emptiness test is on
/// the pair, not on each list. Both halves captured.
auto append_agent_activity_json(std::string& out, std::span<const aa::action> actions, std::span<const aa::claim> claims)
    -> void {
  if (actions.empty() && claims.empty()) {
    return;
  }
  out += ",\"agent_activity\":{\"actions\":[";
  for (std::size_t i = 0; i < actions.size(); ++i) {
    auto const& r = actions[i];
    if (i > 0) {
      out += ",";
    }
    out += std::format("{{\"id\":{},\"session_id\":{},\"action_kind\":", r.id, r.session_id);
    out += json_text::json_string(aa::to_text(r.kind));
    out += ",\"vendor\":";
    out += json_text::json_string(r.vendor);
    out += ",\"started_at\":";
    out += json_text::json_string(r.started_at);
    append_opt_string(out, "ended_at", r.ended_at);
    if (r.result.has_value()) {
      out += ",\"outcome\":";
      out += json_text::json_string(aa::to_text(*r.result));
    }
    append_opt_string(out, "summary", r.summary);
    out += "}";
  }
  out += "],\"claims\":[";
  for (std::size_t i = 0; i < claims.size(); ++i) {
    auto const& r = claims[i];
    if (i > 0) {
      out += ",";
    }
    out += std::format("{{\"id\":{},\"claim_token\":", r.id);
    out += json_text::json_string(r.claim_token);
    out += ",\"status\":";
    out += json_text::json_string(aa::to_text(r.status));
    out += ",\"vendor\":";
    out += json_text::json_string(r.vendor);
    // `role` sits BEFORE `claimed_at`, which is not where a reader
    // alphabetising or grouping-by-required would put it.
    append_opt_string(out, "role", r.role);
    out += ",\"claimed_at\":";
    out += json_text::json_string(r.claimed_at);
    append_opt_string(out, "released_at", r.released_at);
    append_opt_string(out, "release_reason", r.release_reason);
    out += "}";
  }
  out += "]}";
}

/// @brief Render the `commits:` text section, or nothing when empty.
///
/// The displayed timestamp is `committed_at` when present and
/// `recorded_at` otherwise — which is NOT the column the rows are sorted
/// by. See `sessioncommits.cppm`.
auto append_commits_text(std::string& out, std::span<const sc::commit_row> rows) -> void {
  if (rows.empty()) {
    return;
  }
  out += "\ncommits:\n";
  for (auto const& r : rows) {
    out += std::format("  {}  {}  {}\n", r.committed_at.value_or(r.recorded_at), r.sha, r.subject.value_or("(no subject)"));
  }
}

/// @brief Render the `Agent activity:` text section, or nothing when both
/// lists are empty.
///
/// The claim line truncates the token to its first EIGHT characters and
/// appends U+2026 HORIZONTAL ELLIPSIS — one character, not three dots. A
/// token shorter than eight prints whole and still gets the ellipsis.
auto append_agent_activity_text(std::string& out, std::span<const aa::action> actions, std::span<const aa::claim> claims)
    -> void {
  if (actions.empty() && claims.empty()) {
    return;
  }
  out += "\nAgent activity:\n";
  if (!actions.empty()) {
    out += std::format("  actions ({}):\n", actions.size());
    for (auto const& r : actions) {
      out += std::format("    [{}]  {}  {}  outcome={}\n", r.ended_at.value_or(r.started_at), pad_right(aa::to_text(r.kind), 12),
                         pad_right(r.vendor, 8), r.result.has_value() ? aa::to_text(*r.result) : std::string_view{"(in-flight)"});
    }
  }
  if (!claims.empty()) {
    out += std::format("  claims ({}):\n", claims.size());
    for (auto const& r : claims) {
      out += std::format("    [{}]  {}  {}  token:{}…\n", r.released_at.value_or(r.claimed_at),
                         pad_right(aa::to_text(r.status), 10), pad_right(r.vendor, 8),
                         std::string_view{r.claim_token}.substr(0, std::min<std::size_t>(r.claim_token.size(), 8)));
    }
  }
}

} // namespace

auto audit_session(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // Exit 2 for a non-integer id. `entity_id_arg` already produces the
  // oracle's exact wording (`<label> id must be an integer, got '<raw>'`)
  // at `invalid_input`.
  auto const id = entity_id_arg(args, "session-id", "session");
  if (!id) {
    return std::unexpected(id.error());
  }

  auto timeline = at::session_timeline(**conn, *id);
  if (!timeline) {
    switch (timeline.error()) {
    case at::audit_error::not_found:
      // Exit 1, not 2 — a well-formed id that names nothing is NotFound.
      return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("session {} not found", *id)));
    case at::audit_error::query_failed:
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "audit session: QueryFailed"));
    }
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "audit session: QueryFailed"));
  }

  ctx.out() << (cliapp::flag_bool(args, "--json") ? render_json(*timeline) : render_text(*timeline));
  return {};
}

namespace {

/// @brief Render the commit rows as the operator-facing table.
///
/// The HEADER IS UNCONDITIONAL — it prints even when `rows` is empty,
/// which is what makes `audit commits` on an empty table look like a
/// listing with nothing in it rather than a verb that did nothing. The
/// two other output modes disagree (`[]` and zero bytes respectively) and
/// all three are pinned.
///
/// Column widths, transcribed from the oracle's format strings rather
/// than measured: sha left-40, session right-7, claim right-5,
/// committed_at left-25, subject unpadded. `session` and `claim` are
/// exactly seven and five characters, so the HEADER cannot distinguish
/// those two widths from any smaller number — only a data row can.
///
/// The two null columns spell their absence differently from each other's
/// neighbours: a null `claim_id` is `-` right-aligned in five, a null
/// `committed_at` is `-` left-aligned in twenty-five, and a null `subject`
/// is the EMPTY STRING, not `-`. So a row with no subject ends in the
/// gutter's two spaces and nothing else — trailing whitespace that is part
/// of the contract.
/// @param rows The rows, in the order `list_filtered` returned them.
/// @return The complete block including the trailing newline on every line.
auto render_commit_table(std::span<const sc::commit_row> rows) -> std::string {
  auto out = std::format("{:<40}  {:>7}  {:>5}  {:<25}  {}\n", "SHA", "session", "claim", "committed_at", "subject");
  for (auto const& row : rows) {
    out += std::format("{:<40}  {:>7}  ", row.sha, row.session_id);
    out += row.claim_id.has_value() ? std::format("{:>5}", *row.claim_id) : std::format("{:>5}", "-");
    out += "  ";
    out += std::format("{:<25}", row.committed_at.value_or(std::string{"-"}));
    // `""` for a null subject, NOT `-`.
    out += std::format("  {}\n", row.subject.value_or(std::string{}));
  }
  return out;
}

} // namespace

auto audit_commits(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // `ensureDb` FIRST, before the flag-combination check — zig opens with
  // `try runtime.ensureDb()`, so a refused `--json --shas` still creates
  // and migrates the database.
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const json = cliapp::flag_bool(args, "--json");
  auto const shas = cliapp::flag_bool(args, "--shas");
  // BEFORE the id lookups. `--json --shas --session 99` reports THIS, not
  // the missing session. Oracle-captured.
  if (json && shas) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "cannot combine --json with --shas"));
  }

  auto const session_id = cliapp::flag_int(args, "--session");
  auto const task_id    = cliapp::flag_int(args, "--task");

  // Existence checks, SESSION FIRST — `--session 99 --task 99` reports the
  // session. Each is a refusal, not an empty listing; contrast `audit
  // trail 99`, which succeeds with an empty trail. The rows are read only
  // to prove the id resolves; nothing from them reaches the output.
  if (session_id.has_value()) {
    auto row = sess::get_by_id(**conn, *session_id);
    if (!row) {
      if (row.error() == sess::session_error::not_found) {
        return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("session {} not found", *session_id)));
      }
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "audit commits: QueryFailed"));
    }
  }
  if (task_id.has_value()) {
    auto row = pt::show_task(**conn, *task_id);
    if (!row) {
      if (row.error() == pt::task_error::not_found) {
        return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("task {} not found", *task_id)));
      }
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "audit commits: QueryFailed"));
    }
  }

  auto rows = sc::list_filtered(**conn, sc::list_filter{.session_id = session_id, .task_id = task_id});
  if (!rows) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "audit commits: QueryFailed"));
  }

  if (json) {
    ctx.out() << sc::render_json_list(*rows) << '\n';
    return {};
  }
  if (shas) {
    // ZERO BYTES when empty — the only one of the three shapes that
    // prints nothing at all.
    for (auto const& row : *rows) {
      ctx.out() << row.sha << '\n';
    }
    return {};
  }
  ctx.out() << render_commit_table(*rows);
  return {};
}

namespace {

/// @brief `audit trail <entity-id>` — the `audit_log` form.
/// @param ctx The process context.
/// @param conn An open connection.
/// @param kind The `--kind` value, defaulted by the caller.
/// @param id The entity id.
/// @param grep The `--grep` pattern, when supplied.
/// @param json Whether `--json` was passed.
/// @return Success after writing, or the refusal.
auto run_entity_form(context& ctx, db::connection& conn, std::string_view kind, std::int64_t id,
                     const std::optional<std::string>& grep, bool json) -> handler_result {
  // `--grep` SWITCHES THE QUERY; it does not filter the other one's result.
  // `for_entity_grep` does NOT widen through `entity_links`, so a linked
  // entity's rows drop out of the answer entirely. See audit.cppm.
  auto entries = grep.has_value() ? at::for_entity_grep(conn, kind, id, *grep) : at::for_entity_with_links(conn, kind, id);
  if (!entries) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                           grep.has_value() ? "audit trail grep: QueryFailed" : "audit trail: QueryFailed"));
  }

  // The sessions read here feeds ONLY the commits fold-in — the entity form
  // prints no "sessions:" section of its own. Failure degrades to empty.
  auto const sessions = load_sessions_for_entity(conn, kind, id).value_or(std::vector<session_row>{});
  auto const folds    = load_fold_ins(conn, kind, id, sessions);

  std::string out;
  if (json) {
    out = "{\"entity_kind\":";
    out += json_text::json_string(kind);
    out += std::format(",\"entity_id\":{},\"entries\":[", id);
    for (std::size_t i = 0; i < entries->size(); ++i) {
      auto const& e = (*entries)[i];
      if (i > 0) {
        out += ",";
      }
      out += std::format("{{\"id\":{},\"verb\":", e.id);
      out += json_text::json_string(e.verb);
      out += ",\"entity_kind\":";
      out += json_text::json_string(e.entity_kind);
      out += std::format(",\"entity_id\":{},\"recorded_at\":", e.entity_id);
      out += json_text::json_string(e.recorded_at);
      // `recorded_at` sits BEFORE the three optionals, which is not the
      // struct's own field order.
      append_opt_string(out, "actor", e.actor);
      append_opt_string(out, "scope", e.scope);
      append_opt_string(out, "summary", e.summary);
      out += "}";
    }
    out += "]";
    append_commits_json(out, folds.commits);
    append_agent_activity_json(out, folds.actions, folds.claims);
    out += "}\n";
    ctx.out() << out;
    return {};
  }

  // `entries` is never pluralised — `(1 entries)` is the oracle's own output.
  out = std::format("audit trail for {}:{}  ({} entries)\n", kind, id, entries->size());
  if (entries->empty()) {
    out += "  (no audit_log entries)\n";
  } else {
    for (auto const& e : *entries) {
      out += std::format("  [{}]  {}  {}:{}", e.recorded_at, pad_right(e.verb, 14), e.entity_kind, e.entity_id);
      if (e.summary.has_value()) {
        out += std::format("  — {}", *e.summary);
      }
      out += "\n";
    }
  }
  append_commits_text(out, folds.commits);
  append_agent_activity_text(out, folds.actions, folds.claims);
  ctx.out() << out;
  return {};
}

/// @brief `audit trail --link <id>` — the `external_links` form.
/// @param ctx The process context.
/// @param conn An open connection.
/// @param link_id The external-link id.
/// @param json Whether `--json` was passed.
/// @return Success after writing, or the refusal.
auto run_link_form(context& ctx, db::connection& conn, std::int64_t link_id, bool json) -> handler_result {
  auto row = xl::show(conn, link_id);
  if (!row) {
    if (row.error() == xl::link_error::not_found) {
      return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("external link {} not found", link_id)));
    }
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "audit trail --link: load link: QueryFailed"));
  }

  // A VANISHED SYSTEM ROW IS NOT AN ERROR. The header still renders, with a
  // `system:<id>` placeholder in place of the slug — an external link whose
  // system was deleted is exactly the state an operator runs this verb to
  // understand, so refusing would withhold the answer.
  std::string sys_slug = std::format("system:{}", row->system_id);
  if (auto sys = xsys::show_by_id(conn, row->system_id)) {
    sys_slug = sys->slug;
  }

  auto events = xsy::events_for_link(conn, link_id);
  if (!events) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "audit trail --link: load events: QueryFailed"));
  }

  auto const entity_kind_text = std::string{xl::external_entity_kind_to_text(row->entity_kind)};

  // The link form REFUSES on a sessions/decisions failure where the entity
  // form degrades — the oracle's `exit.die` versus its `catch => empty`.
  // Same read, two postures, and this is the one that is loud.
  auto sessions = load_sessions_for_entity(conn, entity_kind_text, row->entity_id);
  if (!sessions) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "audit trail --link: load sessions: QueryFailed"));
  }
  auto decisions = load_decisions_for_entity(conn, entity_kind_text, row->entity_id);
  if (!decisions) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, "audit trail --link: load decisions: QueryFailed"));
  }

  auto const folds = load_fold_ins(conn, entity_kind_text, row->entity_id, *sessions);

  std::string out;
  if (json) {
    out = std::format("{{\"link_id\":{},\"entity_kind\":", row->id);
    out += json_text::json_string(entity_kind_text);
    out += std::format(",\"entity_id\":{},\"external_id\":", row->entity_id);
    out += json_text::json_string(row->external_id);
    out += ",\"system_slug\":";
    out += json_text::json_string(sys_slug);

    out += ",\"sessions\":[";
    for (std::size_t i = 0; i < sessions->size(); ++i) {
      auto const& s = (*sessions)[i];
      if (i > 0) {
        out += ",";
      }
      out += std::format("{{\"id\":{},\"vendor\":", s.id);
      out += json_text::json_string(s.vendor);
      out += ",\"started_at\":";
      out += json_text::json_string(s.started_at);
      append_nonempty_string(out, "summary", s.summary);
      out += "}";
    }
    out += "]";

    out += ",\"decisions\":[";
    for (std::size_t i = 0; i < decisions->size(); ++i) {
      auto const& d = (*decisions)[i];
      if (i > 0) {
        out += ",";
      }
      out += std::format("{{\"id\":{},\"title\":", d.id);
      out += json_text::json_string(d.title);
      out += ",\"status\":";
      out += json_text::json_string(d.status);
      append_nonempty_string(out, "decided_at", d.decided_at);
      out += "}";
    }
    out += "]";

    out += ",\"sync_events\":[";
    for (std::size_t i = 0; i < events->size(); ++i) {
      auto const& e = (*events)[i];
      if (i > 0) {
        out += ",";
      }
      out += std::format("{{\"id\":{},\"direction\":", e.id);
      out += json_text::json_string(e.direction);
      out += ",\"outcome\":";
      out += json_text::json_string(e.event_outcome);
      append_opt_string(out, "fields_changed", e.fields_changed);
      append_opt_string(out, "detail", e.detail);
      if (e.context_json.has_value()) {
        // SPLICED RAW, under the key `evidence` rather than the column's own
        // name. The stored text becomes a JSON VALUE; running it through the
        // string escaper would produce valid JSON of the wrong shape.
        out += ",\"evidence\":";
        out += *e.context_json;
      }
      out += ",\"at\":";
      out += json_text::json_string(e.at);
      out += "}";
    }
    out += "]";
    append_commits_json(out, folds.commits);
    append_agent_activity_json(out, folds.actions, folds.claims);
    out += "}\n";
    ctx.out() << out;
    return {};
  }

  out = std::format("audit trail for link {}  ({}:{} ↔ {}:{})\n", row->id, entity_kind_text, row->entity_id, sys_slug,
                    row->external_id);
  if (!sessions->empty()) {
    out += "\nsessions:\n";
    for (auto const& s : *sessions) {
      out += std::format("  {}  {}  session:{}  \"{}\"\n", s.started_at, s.vendor, s.id,
                         s.summary.empty() ? std::string_view{"(no summary)"} : std::string_view{s.summary});
    }
  }
  if (!decisions->empty()) {
    out += "\ndecisions:\n";
    for (auto const& d : *decisions) {
      // An unset `decided_at` was coalesced to "" upstream, so this line
      // opens with FOUR spaces rather than two and a timestamp. That is the
      // oracle's output, not a formatting slip.
      out += std::format("  {}  \"{}\"  [{}]\n", d.decided_at, d.title, d.status);
    }
  }
  if (!events->empty()) {
    out += "\nsync events:\n";
    for (auto const& e : *events) {
      out += std::format("  {}  {}  {}  {}\n", e.at, pad_right(e.direction, 5), pad_right(e.event_outcome, 10),
                         e.fields_changed.value_or("(no fields)"));
      if (e.context_json.has_value()) {
        out += std::format("    evidence: {}\n", *e.context_json);
      }
    }
  }
  // COMMITS PRINT BEFORE THE ALL-EMPTY FALLBACK, so a link with commits but
  // no sessions, decisions or events prints a "commits:" section AND THEN
  // "(no sessions, decisions, or sync events)". Reproduced; the fallback's
  // condition does not mention commits.
  append_commits_text(out, folds.commits);
  if (sessions->empty() && decisions->empty() && events->empty()) {
    out += "  (no sessions, decisions, or sync events)\n";
  }
  append_agent_activity_text(out, folds.actions, folds.claims);
  ctx.out() << out;
  return {};
}

} // namespace

auto audit_trail(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  bool const json = cliapp::flag_bool(args, "--json");

  // `--link` IS CHECKED FIRST AND RETURNS. `audit trail 1 --link 1` runs the
  // link form and ignores the positional entirely — it does not refuse and
  // it does not merge the two. Captured from the oracle.
  if (auto const link_raw = cliapp::flag_string(args, "--link"); link_raw.has_value()) {
    auto const link_id = cliapp::parse_int64_zig(*link_raw);
    if (!link_id.has_value()) {
      // Its OWN wording, not `entity_id_arg`'s.
      return std::unexpected(
          error_from_body(domain_error_kind::invalid_input, std::format("invalid --link value '{}'", *link_raw)));
    }
    return run_link_form(ctx, **conn, *link_id, json);
  }

  auto const entity_raw = cliapp::positional_string(args, "entity-id");
  if (!entity_raw.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "audit trail requires <entity-id> or --link <id>"));
  }
  auto const id = entity_id_arg(args, "entity-id", "entity");
  if (!id) {
    return std::unexpected(id.error());
  }

  // NOT VALIDATED, and defaulting happens here rather than in the CLI tree
  // so `--kind ""` survives as the empty string and renders `for :1`.
  auto const kind = cliapp::flag_string(args, "--kind").value_or("task");
  return run_entity_form(ctx, **conn, kind, *id, cliapp::flag_string(args, "--grep"), json);
}

namespace {

/// @brief One in-flight task's readiness verdict, in scan order.
struct readiness_row {
  std::int64_t id{};
  std::string  title;
  std::string  status;
  bool         resumable{};
};

} // namespace

auto audit_handoff_readiness(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // The oracle reads `--threshold` straight off the parsed args with no
  // range check at all, so 0, 101 and negatives are all accepted and simply
  // compared against. Pinned rather than "fixed".
  auto const threshold = cliapp::flag_int(args, "--threshold").value_or(90);

  // GLOBAL, NOT SCOPE-FILTERED. This leaf has no `--scope` flag and the
  // oracle's query carries no scope predicate — it scans every in-flight
  // task in the database. Most sibling verbs filter by the cwd-derived
  // scope; this one deliberately does not, and a port that "helpfully"
  // adds the filter answers a different question.
  auto stmt = (*conn)->prepare("select id, coalesce(title,''), coalesce(status,'') from tasks "
                               "where status in ('todo','doing','blocked') order by id");
  if (!stmt) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "audit handoff-readiness: task scan"));
  }

  std::vector<readiness_row> rows;
  std::int64_t               passing = 0;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "audit handoff-readiness: task step"));
    }
    if (*step == db::step_result::done) {
      break;
    }
    auto const id     = stmt->column_int64(0);
    auto       title  = stmt->column_text(1);
    auto       status = stmt->column_text(2);

    // A validate FAILURE counts as not-resumable rather than aborting the
    // scan — the oracle swallows the error into a `resumable=false` row.
    auto const checked   = rck::validate(**conn, id);
    bool const resumable = checked.has_value() && checked->resumable;
    if (resumable) {
      ++passing;
    }
    rows.emplace_back(id, std::move(title), std::move(status), resumable);
  }

  auto const   total   = static_cast<std::int64_t>(rows.size());
  auto const   failing = total - passing;
  double const pct     = total > 0 ? static_cast<double>(passing) / static_cast<double>(total) * 100.0 : 0.0;

  // TRUNCATION, NOT ROUNDING, AND THE DISPLAY DISAGREES WITH IT. The gate
  // is `int64(pct) >= threshold`, which truncates; both rendered forms
  // ROUND (`{d:.2}` / `{d:.0}` in zig). At 2-of-3 the text arm therefore
  // prints the literally self-contradictory `FAIL: threshold not met (67%
  // < 67%)` — 66.67 rounds up for display and truncates down for the
  // comparison. Captured from the oracle and reproduced exactly; it is an
  // oracle defect, not a transcription slip, and is pinned as such.
  //
  // `total == 0` short-circuits to OK even though `pct` is 0.0, so an
  // empty database passes any threshold.
  bool const ok = total == 0 || static_cast<std::int64_t>(pct) >= threshold;

  auto const below = [&] {
    return std::unexpected(error_from_body(domain_error_kind::not_found, "handoff readiness below threshold"));
  };

  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << std::format(R"({{"total":{},"passing":{},"failing":{},"percentage":{:.2f},"threshold":{},"ok":{}}})"
                             "\n",
                             total, passing, failing, pct, threshold, ok ? "true" : "false");
    if (!ok) {
      return below();
    }
    return {};
  }

  ctx.out() << std::format("handoff-readiness: {}/{} tasks pass ({:.0f}%, threshold {}%)\n", passing, total, pct, threshold);
  // The FAIL list is emitted UNCONDITIONALLY — it is not gated on `ok`, so
  // a run that meets a low threshold still lists every failing task above
  // its `OK:` line.
  for (auto const& row : rows) {
    if (!row.resumable) {
      ctx.out() << std::format("  FAIL task:{} \"{}\" [{}]\n", row.id, row.title, row.status);
    }
  }
  if (ok) {
    ctx.out() << "OK: threshold met\n";
    return {};
  }
  ctx.out() << std::format("FAIL: threshold not met ({:.0f}% < {}%)\n", pct, threshold);
  return below();
}

namespace {

// =========================================================================
// `audit publish-decision`
// =========================================================================

/// @brief The Zig error tag for a `system::show_by_id` failure, as
/// `sync_events.detail` records it. Mirrors `adapter::adapter_error_name`'s
/// role for the adapter's own failures — see that function's header.
/// @param err The failure.
/// @return The Zig-spelled tag.
auto system_error_name(xsys::system_error err) -> std::string_view {
  switch (err) {
  case xsys::system_error::not_found:
    return "NotFound";
  case xsys::system_error::slug_exists:
    return "SlugExists";
  case xsys::system_error::query_failed:
    return "QueryFailed";
  }
  return "QueryFailed";
}

/// @brief The Zig error tag for a `build_adapter` failure. Mirrors
/// `zig/src/cmd/planar/handlers/ext/adapter_factory.zig`'s `Error` set,
/// verbatim — these are the tags `@errorName` would have produced.
/// @param err The failure.
/// @return The Zig-spelled tag.
auto factory_error_tag(factory_error err) -> std::string_view {
  switch (err) {
  case factory_error::unsupported_auth_method:
    return "UnsupportedAuthMethod";
  case factory_error::token_env_var_missing:
    return "TokenEnvVarMissing";
  case factory_error::gh_cli_not_found:
    return "GhCliNotFound";
  case factory_error::gh_cli_failed:
    return "GhCliFailed";
  case factory_error::gh_cli_empty_token:
    return "GhCliEmptyToken";
  case factory_error::unsupported_system_kind:
    return "UnsupportedSystemKind";
  }
  return "UnsupportedSystemKind";
}

/// @brief Render the decision comment body. Port of `buildDecisionComment`.
///
/// Title and status on the first line, the body (when non-empty) as its own
/// paragraph, and the rationale (when set and non-empty) as a trailing
/// `_Rationale:_` line with NO training newline after it.
/// @param d The decision.
/// @return The rendered comment.
auto build_decision_comment(const pt::decision& d) -> std::string {
  std::string out = std::format("**Decision: {}** [{}]\n\n", d.title, pt::decision_status_to_text(d.status));
  if (!d.body.empty()) {
    out += std::format("{}\n\n", d.body);
  }
  if (d.rationale.has_value() && !d.rationale->empty()) {
    out += std::format("_Rationale:_ {}", *d.rationale);
  }
  return out;
}

/// @brief Load `sessions.started_at` for `session_id`, or unset. Port of
/// `loadSessionRef`; the caller falls back to the decision's own
/// `created_at` when this returns unset, matching the Zig `catch`.
/// @param conn An open connection.
/// @param session_id The session id, when the decision recorded one.
/// @return The timestamp, or unset when absent or the session vanished.
auto load_session_ref(db::connection& conn, std::optional<std::int64_t> session_id) -> std::optional<std::string> {
  if (!session_id.has_value()) {
    return std::nullopt;
  }
  auto stmt = conn.prepare("select started_at from sessions where id = ?");
  if (!stmt || !stmt->bind_int64(1, *session_id)) {
    return std::nullopt;
  }
  auto step = stmt->step();
  if (!step || *step != db::step_result::row) {
    return std::nullopt;
  }
  return stmt->column_text(0);
}

/// @brief Best-effort record of one posting attempt. Port of
/// `recordResult` — swallows every failure (including a failure to record
/// the failure), matching the Zig original's unconditional `catch return`.
/// @param conn An open connection.
/// @param link_id The `external_links` row this attempt targeted.
/// @param ok Whether the comment posted successfully.
/// @param detail The Zig-spelled outcome tag, or a fixed literal on success.
void record_result(db::connection& conn, std::int64_t link_id, bool ok, std::string_view detail) {
  auto tx = conn.begin_transaction(db::lock_mode::immediate);
  if (!tx) {
    return;
  }
  auto update = conn.prepare("update external_links set last_synced_at = strftime('%Y-%m-%dT%H:%M:%fZ','now'), "
                             "last_sync_status = ? where id = ?");
  if (!update || !update->bind_text(1, ok ? "ok" : "error") || !update->bind_int64(2, link_id) || !update->step()) {
    return;
  }
  auto event = conn.prepare("insert into sync_events (link_id, direction, outcome, detail) values (?, 'push', ?, ?)");
  if (!event || !event->bind_int64(1, link_id) || !event->bind_text(2, ok ? "ok" : "error") || !event->bind_text(3, detail) ||
      !event->step()) {
    return;
  }
  (void)tx->commit();
}

/// @brief Post `comment` to every operational-plane target linked to
/// `(kind_text, entity_id)`. Port of `publishTarget`.
///
/// The `entity_links` READ this needs is done by the caller (for the
/// transitive case) or is implicit in `links_for_entity` (for the direct
/// case); a failure THERE is not this function's problem. Every failure
/// INSIDE the per-link loop — system lookup, adapter build, the post
/// itself — is caught, recorded, and the loop continues. See this file's
/// header on why this function cannot fail loudly the way its caller can.
/// @param ctx The process context, for the credential environment.
/// @param conn An open connection.
/// @param kind_text The entity kind, verbatim (`"decision"`, or an
/// `entity_links.to_kind` value).
/// @param entity_id The local row id.
/// @param comment The rendered comment body (footer NOT yet appended).
/// @param session_ref The session timestamp/label for the footer.
/// @param posted Incremented once per successful post.
void publish_target(context& ctx, db::connection& conn, std::string_view kind_text, std::int64_t entity_id,
                    std::string_view comment, std::string_view session_ref, std::int64_t& posted) {
  auto const kind = xl::external_entity_kind_from_text(kind_text);
  if (!kind.has_value()) {
    return;
  }
  auto links = xl::links_for_entity(conn, *kind, entity_id);
  if (!links) {
    return;
  }

  for (auto const& link : *links) {
    auto system = xsys::show_by_id(conn, link.system_id);
    if (!system) {
      record_result(conn, link.id, false, system_error_name(system.error()));
      continue;
    }

    auto built = build_adapter(*system, default_deps(ctx.env()));
    if (!built) {
      record_result(conn, link.id, false, factory_error_tag(built.error()));
      continue;
    }

    auto const footer =
        std::format("— posted by planar (entity: {}:{}, session: {}, link: ext:{})", kind_text, entity_id, session_ref, link.id);
    auto const body = std::format("{}\n\n{}", comment, footer);

    auto const result = (*built)->post_comment(link.external_id, body);
    if (!result) {
      record_result(conn, link.id, false, adapter::adapter_error_name(result.error()));
      continue;
    }
    record_result(conn, link.id, true, "decision-comment");
    ++posted;
  }
}

} // namespace

auto audit_publish_decision(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const decision_id = entity_id_arg(args, "decision-id", "decision");
  if (!decision_id) {
    return std::unexpected(decision_id.error());
  }

  auto decision = pt::show_decision(**conn, *decision_id);
  if (!decision) {
    if (decision.error() == pt::decision_error::not_found) {
      return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("decision {} not found", *decision_id)));
    }
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, "audit publish-decision: loading decision: QueryFailed"));
  }

  // The scope guard runs BEFORE the comment is built, the session is
  // looked up, or any adapter is touched — a mismatch here issues ZERO
  // HTTP requests. See this leaf's declaration for why that ordering is
  // asserted on the fixture server's request log.
  auto const scope_kind = [&] {
    switch (decision->scope_kind) {
    case pt::decision_scope_kind::global:
      return engine::identity::scope_kind::global;
    case pt::decision_scope_kind::association:
      return engine::identity::scope_kind::association;
    case pt::decision_scope_kind::repo:
      break;
    }
    return engine::identity::scope_kind::repo;
  }();
  auto entity_scope = engine::identity::slug_from_ref(**conn, scope_kind, decision->scope_id);
  if (!entity_scope) {
    return std::unexpected(map_scope_error(entity_scope.error(), "audit publish-decision: resolving decision scope"));
  }

  auto const scope_flag = cliapp::flag_string(args, "--scope");
  auto const scope_view = scope_flag.has_value() ? std::optional<std::string_view>{*scope_flag} : std::nullopt;
  auto       resolved   = resolve_write_scope(ctx, scope_view, "audit publish-decision");
  if (!resolved) {
    return std::unexpected(resolved.error());
  }

  auto const entity_view = entity_scope->has_value() ? std::optional<std::string_view>{**entity_scope} : std::nullopt;
  auto const write_view  = resolved->scope.has_value() ? std::optional<std::string_view>{*resolved->scope} : std::nullopt;
  if (!guard_with_membership(**conn, entity_view, write_view)) {
    return std::unexpected(error_from_body(domain_error_kind::scope_mismatch,
                                           std::format("decision {} belongs to a different scope", *decision_id)));
  }

  auto const comment     = build_decision_comment(*decision);
  auto const session_ref = load_session_ref(**conn, decision->session_id).value_or(decision->created_at);

  std::int64_t posted = 0;
  publish_target(ctx, **conn, "decision", *decision_id, comment, session_ref, posted);

  // The `entity_links` read for the transitive targets is NOT given the
  // same latitude as the per-link loop inside `publish_target` — a
  // failure here aborts the whole verb, matching the oracle's bare `try`
  // on this one query.
  auto stmt =
      (*conn)->prepare("select to_kind, to_id from entity_links where from_kind = 'decision' and from_id = ? order by id");
  if (!stmt || !stmt->bind_int64(1, *decision_id)) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, "audit publish-decision: preparing targets: QueryFailed"));
  }
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, "audit publish-decision: reading targets: QueryFailed"));
    }
    if (*step == db::step_result::done) {
      break;
    }
    auto const to_kind = stmt->column_text(0);
    auto const to_id   = stmt->column_int64(1);
    publish_target(ctx, **conn, to_kind, to_id, comment, session_ref, posted);
  }

  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << std::format(R"({{"ok":true,"decision_id":{},"comments_posted":{}}})", *decision_id, posted) << '\n';
    return {};
  }
  ctx.out() << std::format("decision {} published: {} comment(s) posted\n", *decision_id, posted);
  return {};
}

} // namespace planar::cmd::handlers
