/// @file sessioncommits.cpp
/// @brief Implementation of `planar.engine.runtime.sessioncommits` (plan
/// 996, task 6262). See sessioncommits.cppm for what this module is NOT.

module planar.engine.runtime.sessioncommits;

import std;
import planar.db;
import planar.json_text;

namespace planar::engine::runtime::sessioncommits {

using json_text::json_string;

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

/// @brief The column list both queries select, in `read_rows`'s index
/// order. Shared so the two statements cannot drift into disagreeing about
/// which column is which — a drift that produces well-formed rows with
/// swapped fields rather than an error.
constexpr std::string_view k_select_columns = "select sc.id, sc.session_id, sc.claim_id, sc.sha, sc.repo_root,\n"
                                              "       sc.branch, sc.subject, sc.author, sc.committed_at, sc.recorded_at\n"
                                              "from session_commits sc";

/// @brief Drain a prepared, bound statement into rows.
/// @param stmt The statement, already bound.
/// @return The rows, or `commits_error::query_failed`.
auto read_rows(db::statement& stmt) -> std::expected<std::vector<commit_row>, commits_error> {
  std::vector<commit_row> rows;
  while (true) {
    auto step = stmt.step();
    if (!step) {
      return std::unexpected(commits_error::query_failed);
    }
    if (*step != db::step_result::row) {
      return rows;
    }
    rows.push_back(commit_row{
        .id           = stmt.column_int64(0),
        .session_id   = stmt.column_int64(1),
        .claim_id     = opt_int(stmt, 2),
        .sha          = stmt.column_text(3),
        .repo_root    = opt_text(stmt, 4),
        .branch       = opt_text(stmt, 5),
        .subject      = opt_text(stmt, 6),
        .author       = opt_text(stmt, 7),
        .committed_at = opt_text(stmt, 8),
        .recorded_at  = stmt.column_text(9),
    });
  }
}

/// @brief Append `,"<key>":<value-or-null>` for an optional string.
/// @param out The buffer to append to.
/// @param key The JSON key.
/// @param value The value, or unset for a literal `null`.
void append_string_opt(std::string& out, std::string_view key, const std::optional<std::string>& value) {
  out += std::format(",\"{}\":{}", key, value.has_value() ? json_string(*value) : std::string{"null"});
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

  std::string sql = std::format("{}\nwhere sc.session_id in (", k_select_columns);
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

  return read_rows(*stmt);
}

auto list_filtered(db::connection& conn, const list_filter& filter) -> std::expected<std::vector<commit_row>, commits_error> {
  std::string sql{k_select_columns};
  // The join is conditional. Adding it unconditionally would turn every
  // unfiltered listing into an inner join that drops NULL-`claim_id` rows
  // — a silent narrowing that looks like "there were no commits".
  if (filter.task_id.has_value()) {
    sql += "\njoin agent_work_claims awc on awc.id = sc.claim_id";
  }
  sql += "\nwhere 1 = 1";
  if (filter.session_id.has_value()) {
    sql += "\n  and sc.session_id = ?";
  }
  if (filter.task_id.has_value()) {
    // `entity_kind` is part of the predicate, not decoration — a claim on
    // a different entity kind with the same numeric id must not match.
    sql += "\n  and awc.entity_kind = 'task'\n  and awc.entity_id = ?";
  }
  sql += "\norder by sc.recorded_at desc, sc.id desc";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(commits_error::query_failed);
  }
  // Bound in the order the terms were appended: session first, then task.
  int index = 1;
  if (filter.session_id.has_value() && !stmt->bind_int64(index++, *filter.session_id)) {
    return std::unexpected(commits_error::query_failed);
  }
  if (filter.task_id.has_value() && !stmt->bind_int64(index, *filter.task_id)) {
    return std::unexpected(commits_error::query_failed);
  }
  return read_rows(*stmt);
}

auto render_json(const commit_row& row) -> std::string {
  std::string out = std::format("{{\"id\":{},\"session_id\":{}", row.id, row.session_id);
  // Explicit `null`, never an omitted key.
  out += row.claim_id.has_value() ? std::format(",\"claim_id\":{}", *row.claim_id) : std::string{",\"claim_id\":null"};
  out += std::format(",\"sha\":{}", json_string(row.sha));
  append_string_opt(out, "repo_root", row.repo_root);
  append_string_opt(out, "branch", row.branch);
  append_string_opt(out, "subject", row.subject);
  append_string_opt(out, "author", row.author);
  append_string_opt(out, "committed_at", row.committed_at);
  out += std::format(",\"recorded_at\":{}}}", json_string(row.recorded_at));
  return out;
}

auto render_json_list(std::span<const commit_row> rows) -> std::string {
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

} // namespace planar::engine::runtime::sessioncommits
