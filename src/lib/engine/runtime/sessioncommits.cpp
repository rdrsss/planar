/// @file sessioncommits.cpp
/// @brief Implementation of `planar.engine.runtime.sessioncommits` (plan
/// 996, task 6262). See sessioncommits.cppm for what this module is NOT.

module planar.engine.runtime.sessioncommits;

import std;
import planar.db;

namespace planar::engine::runtime::sessioncommits {

namespace {

auto opt_text(const db::statement& stmt, int index) -> std::optional<std::string> {
  if (stmt.is_null(index)) {
    return std::nullopt;
  }
  return stmt.column_text(index);
}

auto opt_int(const db::statement& stmt, int index) -> std::optional<std::int64_t> {
  if (stmt.is_null(index)) {
    return std::nullopt;
  }
  return stmt.column_int64(index);
}

} // namespace

auto list_for_sessions(db::connection& conn, std::span<const std::int64_t> session_ids, std::optional<std::int64_t> limit)
    -> std::expected<std::vector<commit_row>, commits_error> {
  // The no-query arm, mirroring the oracle's own early return. NOT a
  // correctness requirement: SQLite accepts the empty `in ()` this would
  // otherwise compose and evaluates it to false, so deleting these three
  // lines changes nothing observable (measured — the break-probe for it is
  // a SURVIVOR, and the module header says so rather than pretending
  // otherwise). Kept because the oracle has it and because hand-composing a
  // query that leans on a non-standard SQLite extension is a needless bet.
  if (session_ids.empty()) {
    return std::vector<commit_row>{};
  }

  std::string sql = "select sc.id, sc.session_id, sc.claim_id, sc.sha, sc.repo_root,\n"
                    "       sc.branch, sc.subject, sc.author, sc.committed_at, sc.recorded_at\n"
                    "from session_commits sc\n"
                    "where sc.session_id in (";
  for (std::size_t i = 0; i < session_ids.size(); ++i) {
    sql += (i > 0) ? ", ?" : "?";
  }
  // `recorded_at`, NOT `committed_at`. See the module header.
  sql += ")\norder by sc.recorded_at desc, sc.id desc";
  if (limit.has_value()) {
    sql += "\nlimit ?";
  }

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(commits_error::query_failed);
  }
  int index = 1;
  for (auto const id : session_ids) {
    if (!stmt->bind_int64(index++, id)) {
      return std::unexpected(commits_error::query_failed);
    }
  }
  if (limit.has_value() && !stmt->bind_int64(index, *limit)) {
    return std::unexpected(commits_error::query_failed);
  }

  std::vector<commit_row> rows;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(commits_error::query_failed);
    }
    if (*step != db::step_result::row) {
      return rows;
    }
    rows.push_back(commit_row{
        .id           = stmt->column_int64(0),
        .session_id   = stmt->column_int64(1),
        .claim_id     = opt_int(*stmt, 2),
        .sha          = stmt->column_text(3),
        .repo_root    = opt_text(*stmt, 4),
        .branch       = opt_text(*stmt, 5),
        .subject      = opt_text(*stmt, 6),
        .author       = opt_text(*stmt, 7),
        .committed_at = opt_text(*stmt, 8),
        .recorded_at  = stmt->column_text(9),
    });
  }
}

} // namespace planar::engine::runtime::sessioncommits
