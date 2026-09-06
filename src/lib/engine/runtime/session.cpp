/// @file session.cpp
/// @brief Implementation of `planar.engine.runtime.session` (plan 996,
/// task 6094). See session.cppm for scope and omissions.

module planar.engine.runtime.session;

import std;
import planar.db;
import planar.policy;

namespace planar::engine::runtime::session {

namespace audit = planar::policy::audit;

namespace {

constexpr std::string_view k_select_columns = "select id, task_id, project_id, agent_id, vendor, vendor_session_id, "
                                              "model, started_at, ended_at, summary, repo_root, head_sha_at_start "
                                              "from sessions";

auto opt_int(const db::statement& stmt, int index) -> std::optional<std::int64_t> {
  if (stmt.is_null(index)) {
    return std::nullopt;
  }
  return stmt.column_int64(index);
}

auto opt_text(const db::statement& stmt, int index) -> std::optional<std::string> {
  if (stmt.is_null(index)) {
    return std::nullopt;
  }
  return stmt.column_text(index);
}

auto read_session_row(const db::statement& stmt) -> session {
  return session{
      .id                = stmt.column_int64(0),
      .task_id           = opt_int(stmt, 1),
      .project_id        = opt_int(stmt, 2),
      .agent_id          = opt_int(stmt, 3),
      .vendor            = stmt.column_text(4),
      .vendor_session_id = opt_text(stmt, 5),
      .model             = opt_text(stmt, 6),
      .started_at        = stmt.column_text(7),
      .ended_at          = opt_text(stmt, 8),
      .summary           = opt_text(stmt, 9),
      .repo_root         = opt_text(stmt, 10),
      .head_sha_at_start = opt_text(stmt, 11),
  };
}

auto read_entry_row(const db::statement& stmt) -> session_entry {
  return session_entry{
      .id         = stmt.column_int64(0),
      .session_id = stmt.column_int64(1),
      .ordinal    = stmt.column_int64(2),
      .prefix     = stmt.column_text(3),
      .body       = stmt.column_text(4),
      .created_at = stmt.column_text(5),
  };
}

/// @brief Read an environment variable, treating unset and empty alike as
/// absent — the rule both `vendorFromEnv` and `vendorSessionIdFromEnv`
/// apply (`if (v.len > 0) ...`).
auto env_non_empty(const char* name) -> std::optional<std::string> {
  const char* raw = std::getenv(name);
  if (raw == nullptr) {
    return std::nullopt;
  }
  std::string value{raw};
  if (value.empty()) {
    return std::nullopt;
  }
  return value;
}

} // namespace

auto vendor_from_env() -> std::string {
  return env_non_empty("PLANAR_VENDOR").value_or(std::string{"cli"});
}

auto vendor_session_id_from_env() -> std::optional<std::string> {
  return env_non_empty("PLANAR_VENDOR_SESSION_ID");
}

auto get_by_id(db::connection& conn, std::int64_t id) -> std::expected<session, session_error> {
  auto stmt = conn.prepare(std::format("{} where id = ?", k_select_columns));
  if (!stmt) {
    return std::unexpected(session_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, id); !b) {
    return std::unexpected(session_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(session_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(session_error::not_found);
  }
  return read_session_row(*stmt);
}

auto start_session(db::connection& conn, const start_args& args) -> std::expected<session, session_error> {
  const auto has_vsid = args.vendor_session_id.has_value();
  auto       stmt     = conn.prepare(has_vsid ? "select id, task_id from sessions where vendor = ? and "
                                                "vendor_session_id = ? order by id asc limit 1"
                                              : "select id, task_id from sessions where vendor = ? and "
                                                "vendor_session_id is null order by id asc limit 1");
  if (!stmt) {
    return std::unexpected(session_error::query_failed);
  }
  if (auto b = stmt->bind_text(1, args.vendor); !b) {
    return std::unexpected(session_error::query_failed);
  }
  if (has_vsid) {
    if (auto b = stmt->bind_text(2, *args.vendor_session_id); !b) {
      return std::unexpected(session_error::query_failed);
    }
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(session_error::query_failed);
  }

  if (*step == db::step_result::row) {
    const auto existing_id   = stmt->column_int64(0);
    const auto existing_task = opt_int(*stmt, 1);
    if (args.task_id.has_value()) {
      if (existing_task.has_value()) {
        if (*existing_task != *args.task_id) {
          return std::unexpected(session_error::task_conflict);
        }
      } else {
        auto upd = conn.prepare("update sessions set task_id = ? where id = ?");
        if (!upd) {
          return std::unexpected(session_error::query_failed);
        }
        if (auto b = upd->bind_int64(1, *args.task_id); !b) {
          return std::unexpected(session_error::query_failed);
        }
        if (auto b = upd->bind_int64(2, existing_id); !b) {
          return std::unexpected(session_error::query_failed);
        }
        if (auto s = upd->step(); !s) {
          return std::unexpected(session_error::query_failed);
        }
      }
    }
    return get_by_id(conn, existing_id);
  }

  auto ins = conn.prepare("insert into sessions (vendor, vendor_session_id, task_id, model) "
                          "values (?, ?, ?, ?) returning id");
  if (!ins) {
    return std::unexpected(session_error::query_failed);
  }
  if (auto b = ins->bind_text(1, args.vendor); !b) {
    return std::unexpected(session_error::query_failed);
  }
  auto b2 = has_vsid ? ins->bind_text(2, *args.vendor_session_id) : ins->bind_null(2);
  if (!b2) {
    return std::unexpected(session_error::query_failed);
  }
  auto b3 = args.task_id.has_value() ? ins->bind_int64(3, *args.task_id) : ins->bind_null(3);
  if (!b3) {
    return std::unexpected(session_error::query_failed);
  }
  auto b4 = args.model.has_value() ? ins->bind_text(4, *args.model) : ins->bind_null(4);
  if (!b4) {
    return std::unexpected(session_error::query_failed);
  }
  auto ins_step = ins->step();
  if (!ins_step || *ins_step != db::step_result::row) {
    return std::unexpected(session_error::query_failed);
  }
  const auto id = ins->column_int64(0);
  if (auto recorded = audit::record(conn, audit::record_args{.verb    = audit::verb::create,
                                                             .entity  = {.kind = "session", .id = id},
                                                             .summary = std::format("start session vendor={}", args.vendor)});
      !recorded) {
    return std::unexpected(session_error::query_failed);
  }
  return get_by_id(conn, id);
}

auto end_session(db::connection& conn, std::int64_t session_id, std::optional<std::string_view> summary_text)
    -> std::expected<void, session_error> {
  auto check = conn.prepare("select ended_at from sessions where id = ?");
  if (!check) {
    return std::unexpected(session_error::query_failed);
  }
  if (auto b = check->bind_int64(1, session_id); !b) {
    return std::unexpected(session_error::query_failed);
  }
  auto step = check->step();
  if (!step) {
    return std::unexpected(session_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(session_error::not_found);
  }
  if (!check->is_null(0)) {
    return std::unexpected(session_error::already_ended);
  }

  auto stmt =
      conn.prepare(summary_text.has_value() ? "update sessions set ended_at = strftime('%Y-%m-%dT%H:%M:%fZ','now'), "
                                              "summary = ? where id = ?"
                                            : "update sessions set ended_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?");
  if (!stmt) {
    return std::unexpected(session_error::query_failed);
  }
  int idx = 1;
  if (summary_text.has_value()) {
    if (auto b = stmt->bind_text(idx++, *summary_text); !b) {
      return std::unexpected(session_error::query_failed);
    }
  }
  if (auto b = stmt->bind_int64(idx, session_id); !b) {
    return std::unexpected(session_error::query_failed);
  }
  if (auto s = stmt->step(); !s) {
    return std::unexpected(session_error::query_failed);
  }
  if (auto recorded = audit::record(conn, audit::record_args{.verb    = audit::verb::status_change,
                                                             .entity  = {.kind = "session", .id = session_id},
                                                             .summary = std::format("end session id={}", session_id)});
      !recorded) {
    return std::unexpected(session_error::query_failed);
  }
  return {};
}

auto active_for_vendor(db::connection& conn, std::string_view vendor, std::optional<std::string_view> vendor_session_id)
    -> std::expected<std::optional<session>, session_error> {
  const auto has_vsid = vendor_session_id.has_value();
  auto       stmt     = conn.prepare(has_vsid ? "select id from sessions where vendor = ? and vendor_session_id = ? "
                                                "and ended_at is null order by id asc limit 1"
                                              : "select id from sessions where vendor = ? and vendor_session_id is null "
                                                "and ended_at is null order by id asc limit 1");
  if (!stmt) {
    return std::unexpected(session_error::query_failed);
  }
  if (auto b = stmt->bind_text(1, vendor); !b) {
    return std::unexpected(session_error::query_failed);
  }
  if (has_vsid) {
    if (auto b = stmt->bind_text(2, *vendor_session_id); !b) {
      return std::unexpected(session_error::query_failed);
    }
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(session_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::optional<session>{};
  }
  auto found = get_by_id(conn, stmt->column_int64(0));
  if (!found) {
    return std::unexpected(found.error());
  }
  return std::optional<session>{std::move(*found)};
}

auto ensure_active(db::connection& conn, std::string_view vendor, std::optional<std::string_view> vendor_session_id)
    -> std::expected<std::int64_t, session_error> {
  auto existing = active_for_vendor(conn, vendor, vendor_session_id);
  if (!existing) {
    return std::unexpected(existing.error());
  }
  if (existing->has_value()) {
    return (*existing)->id;
  }
  auto created = start_session(
      conn, start_args{.vendor = vendor, .vendor_session_id = vendor_session_id, .task_id = std::nullopt, .model = std::nullopt});
  if (!created) {
    return std::unexpected(created.error());
  }
  return created->id;
}

auto append_entry(db::connection& conn, std::int64_t session_id, std::string_view prefix, std::string_view body)
    -> std::expected<void, session_error> {
  std::int64_t max_ord = 0;
  {
    auto stmt = conn.prepare("select coalesce(max(ordinal), 0) from session_entries where session_id = ?");
    if (!stmt) {
      return std::unexpected(session_error::query_failed);
    }
    if (auto b = stmt->bind_int64(1, session_id); !b) {
      return std::unexpected(session_error::query_failed);
    }
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(session_error::query_failed);
    }
    if (*step == db::step_result::row) {
      max_ord = stmt->column_int64(0);
    }
  }

  auto stmt = conn.prepare("insert into session_entries (session_id, ordinal, prefix, body) values (?, ?, ?, ?)");
  if (!stmt) {
    return std::unexpected(session_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, session_id); !b) {
    return std::unexpected(session_error::query_failed);
  }
  if (auto b = stmt->bind_int64(2, max_ord + 1); !b) {
    return std::unexpected(session_error::query_failed);
  }
  if (auto b = stmt->bind_text(3, prefix); !b) {
    return std::unexpected(session_error::query_failed);
  }
  if (auto b = stmt->bind_text(4, body); !b) {
    return std::unexpected(session_error::query_failed);
  }
  if (auto s = stmt->step(); !s) {
    return std::unexpected(session_error::query_failed);
  }
  return {};
}

auto set_start_git_context_if_unset(db::connection& conn, std::int64_t session_id, std::string_view repo_root,
                                    std::string_view head_sha_at_start) -> std::expected<void, session_error> {
  auto stmt = conn.prepare("update sessions set "
                           "repo_root = case when repo_root is null then ? else repo_root end, "
                           "head_sha_at_start = case when head_sha_at_start is null then ? else head_sha_at_start end "
                           "where id = ?");
  if (!stmt) {
    return std::unexpected(session_error::query_failed);
  }
  if (auto b = stmt->bind_text(1, repo_root); !b) {
    return std::unexpected(session_error::query_failed);
  }
  if (auto b = stmt->bind_text(2, head_sha_at_start); !b) {
    return std::unexpected(session_error::query_failed);
  }
  if (auto b = stmt->bind_int64(3, session_id); !b) {
    return std::unexpected(session_error::query_failed);
  }
  if (auto s = stmt->step(); !s) {
    return std::unexpected(session_error::query_failed);
  }
  return {};
}

auto list_entries_for_session(db::connection& conn, std::int64_t session_id)
    -> std::expected<std::vector<session_entry>, session_error> {
  auto stmt = conn.prepare("select id, session_id, ordinal, prefix, body, created_at from session_entries "
                           "where session_id = ? order by ordinal");
  if (!stmt) {
    return std::unexpected(session_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, session_id); !b) {
    return std::unexpected(session_error::query_failed);
  }
  std::vector<session_entry> out;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(session_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    out.push_back(read_entry_row(*stmt));
  }
  return out;
}

auto recent_entries_for_task(db::connection& conn, std::int64_t task_id, std::int64_t limit)
    -> std::expected<std::vector<session_entry>, session_error> {
  auto stmt = conn.prepare("select se.id, se.session_id, se.ordinal, se.prefix, se.body, se.created_at "
                           "from session_entries se "
                           "join sessions s on s.id = se.session_id "
                           "where s.task_id = ? "
                           "order by se.created_at desc "
                           "limit ?");
  if (!stmt) {
    return std::unexpected(session_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, task_id); !b) {
    return std::unexpected(session_error::query_failed);
  }
  if (auto b = stmt->bind_int64(2, limit); !b) {
    return std::unexpected(session_error::query_failed);
  }
  std::vector<session_entry> out;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(session_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    out.push_back(read_entry_row(*stmt));
  }
  return out;
}

auto recent_sessions_for_task(db::connection& conn, std::int64_t task_id, std::int64_t limit)
    -> std::expected<std::vector<session>, session_error> {
  auto stmt = conn.prepare(std::format("{} where task_id = ? order by started_at desc limit ?", k_select_columns));
  if (!stmt) {
    return std::unexpected(session_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, task_id); !b) {
    return std::unexpected(session_error::query_failed);
  }
  if (auto b = stmt->bind_int64(2, limit); !b) {
    return std::unexpected(session_error::query_failed);
  }
  std::vector<session> out;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(session_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    out.push_back(read_session_row(*stmt));
  }
  return out;
}

} // namespace planar::engine::runtime::session
