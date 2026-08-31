/// @file sessioncommits.cpp
/// @brief Implementation of `planar.engine.runtime.sessioncommits` (plan
/// 996, tasks 6262 and 6358). See sessioncommits.cppm for what this module
/// is NOT.

module planar.engine.runtime.sessioncommits;

import std;
import planar.db;
import planar.git;
import planar.json_text;

namespace planar::engine::runtime::sessioncommits {

using json_text::json_string;

namespace {

/// @brief The `-z`-delimited log/show format zig's `sessioncommits.zig`
/// uses verbatim: sha, author, subject, ISO-8601 committer date, each
/// NUL-terminated. `git log -z`/`git show -z` additionally NUL-terminate
/// each COMMIT's formatted output, so a multi-commit run reads as
/// `sha\0author\0subject\0date\0\0sha\0...` — the double NUL between
/// commits is why `parse_commits` skips a RUN of leading NULs, not just
/// one. (Measured: with this format string, one of the pair is already
/// consumed as field 4's own terminator, so `if` and `while` are observably
/// EQUIVALENT here — a genuine survivor, see sessioncommits.t.cpp's header.
/// `while` is kept because it is the correct general contract, not because
/// this fixture discriminates it.)
constexpr std::string_view k_log_format = "%H%x00%an%x00%s%x00%cI%x00";

/// @brief Trim ASCII whitespace from both ends, mirroring zig's
/// `std.mem.trim(u8, s, " \t\r\n")`.
/// @param s The string to trim.
/// @return The trimmed view, empty when `s` is all whitespace.
auto trim(std::string_view s) -> std::string_view {
  constexpr std::string_view ws    = " \t\r\n";
  auto const                 begin = s.find_first_not_of(ws);
  if (begin == std::string_view::npos) {
    return {};
  }
  auto const end = s.find_last_not_of(ws);
  return s.substr(begin, end - begin + 1);
}

/// @brief Read one NUL-terminated field starting at `index`, advancing
/// `index` past the terminator.
/// @param raw The full buffer.
/// @param index The read cursor; advanced in place.
/// @return The field text, or `malformed_output` when no terminating NUL
/// remains.
auto next_field(std::string_view raw, std::size_t& index) -> std::expected<std::string, commits_error> {
  auto const pos = raw.find('\0', index);
  if (pos == std::string_view::npos) {
    return std::unexpected(commits_error::malformed_output);
  }
  std::string field{raw.substr(index, pos - index)};
  index = pos + 1;
  return field;
}

/// @brief Parse `-z`-delimited `git log`/`git show` output into commits.
/// Port of zig's `parseCommits`.
/// @param repo_root Stamped onto every returned commit.
/// @param branch Stamped onto every returned commit, when known.
/// @param raw The raw stdout.
/// @return The commits, in the order git emitted them, or
/// `malformed_output` when a field run is truncated.
auto parse_commits(std::string_view repo_root, std::optional<std::string_view> branch, std::string_view raw)
    -> std::expected<std::vector<commit_meta>, commits_error> {
  std::vector<commit_meta> out;
  std::size_t              index = 0;
  while (index < raw.size()) {
    while (index < raw.size() && raw[index] == '\0') {
      ++index;
    }
    if (index >= raw.size()) {
      break;
    }
    auto sha = next_field(raw, index);
    if (!sha) {
      return std::unexpected(sha.error());
    }
    auto author = next_field(raw, index);
    if (!author) {
      return std::unexpected(author.error());
    }
    auto subject = next_field(raw, index);
    if (!subject) {
      return std::unexpected(subject.error());
    }
    auto committed_at = next_field(raw, index);
    if (!committed_at) {
      return std::unexpected(committed_at.error());
    }
    out.push_back(commit_meta{
        .sha          = std::move(*sha),
        .repo_root    = std::string{repo_root},
        .branch       = branch.has_value() ? std::optional<std::string>{std::string{*branch}} : std::nullopt,
        .subject      = std::move(*subject),
        .author       = std::move(*author),
        .committed_at = std::move(*committed_at),
    });
  }
  return out;
}

/// @brief `git -C dir symbolic-ref --short HEAD`, best-effort. Port of
/// zig's `branchForRepo`: any failure, or an exit-0 blank answer, reports
/// unset rather than an error — a detached HEAD must not fail the walk.
/// @param dir The `-C` directory.
/// @return The branch name, or unset.
auto branch_for_repo(const std::filesystem::path& dir) -> std::optional<std::string> {
  static constexpr std::array<std::string_view, 3> k_args{"symbolic-ref", "--short", "HEAD"};
  return git::run_trimmed(dir, k_args);
}

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

auto resolve_repo_root_strict(const std::filesystem::path& dir) -> std::expected<std::string, commits_error> {
  static constexpr std::array<std::string_view, 2> k_args{"rev-parse", "--show-toplevel"};
  auto const                                       raw = git::run(dir, k_args);
  if (!raw.has_value()) {
    return std::unexpected(commits_error::not_git);
  }
  auto const trimmed = trim(*raw);
  if (trimmed.empty()) {
    return std::unexpected(commits_error::malformed_output);
  }
  return std::string{trimmed};
}

auto walk_strict(const std::filesystem::path& dir, std::string_view base_sha, std::optional<std::string_view> repo_root)
    -> std::expected<std::vector<commit_meta>, commits_error> {
  std::string const                   range       = std::format("{}..HEAD", base_sha);
  std::string const                   format_flag = std::format("--format={}", k_log_format);
  std::vector<std::string_view> const argv{"log", "-z", format_flag, range};

  auto const raw = git::run(dir, argv);
  if (!raw.has_value()) {
    return std::unexpected(commits_error::git_failed);
  }

  auto const        branch         = branch_for_repo(dir);
  std::string const effective_root = repo_root.has_value() ? std::string{*repo_root} : dir.string();
  auto const        branch_view    = branch.has_value() ? std::optional<std::string_view>{*branch} : std::nullopt;
  return parse_commits(effective_root, branch_view, *raw);
}

auto resolve_shas(const std::filesystem::path& dir, std::span<const std::string_view> shas,
                  std::optional<std::string_view> repo_root) -> std::expected<std::vector<commit_meta>, commits_error> {
  if (shas.empty()) {
    return std::vector<commit_meta>{};
  }

  std::string const             format_flag = std::format("--format={}", k_log_format);
  std::vector<std::string_view> argv{"show", "--no-patch", "-z", format_flag};
  argv.insert(argv.end(), shas.begin(), shas.end());

  auto const raw = git::run(dir, argv);
  if (!raw.has_value()) {
    return std::unexpected(commits_error::git_failed);
  }

  auto const        branch         = branch_for_repo(dir);
  std::string const effective_root = repo_root.has_value() ? std::string{*repo_root} : dir.string();
  auto const        branch_view    = branch.has_value() ? std::optional<std::string_view>{*branch} : std::nullopt;
  return parse_commits(effective_root, branch_view, *raw);
}

auto record_count(db::connection& conn, std::int64_t session_id, std::optional<std::int64_t> claim_id,
                  std::span<const commit_meta> commits) -> std::expected<std::size_t, commits_error> {
  static constexpr std::string_view k_insert_sql =
      "insert or ignore into session_commits (\n"
      "  session_id, claim_id, sha, repo_root, branch, subject, author, committed_at\n"
      ") values (?, ?, ?, ?, ?, ?, ?, ?)";
  static constexpr std::string_view k_changes_sql = "select changes()";

  auto bind_opt = [](db::statement& stmt, int index,
                     const std::optional<std::string>& value) -> std::expected<void, db::db_error> {
    return value.has_value() ? stmt.bind_text(index, *value) : stmt.bind_null(index);
  };

  std::size_t inserted = 0;
  for (auto const& commit : commits) {
    auto stmt = conn.prepare(k_insert_sql);
    if (!stmt) {
      return std::unexpected(commits_error::query_failed);
    }
    if (auto b = stmt->bind_int64(1, session_id); !b) {
      return std::unexpected(commits_error::query_failed);
    }
    auto b2 = claim_id.has_value() ? stmt->bind_int64(2, *claim_id) : stmt->bind_null(2);
    if (!b2) {
      return std::unexpected(commits_error::query_failed);
    }
    if (auto b = stmt->bind_text(3, commit.sha); !b) {
      return std::unexpected(commits_error::query_failed);
    }
    if (auto b = bind_opt(*stmt, 4, commit.repo_root); !b) {
      return std::unexpected(commits_error::query_failed);
    }
    if (auto b = bind_opt(*stmt, 5, commit.branch); !b) {
      return std::unexpected(commits_error::query_failed);
    }
    if (auto b = bind_opt(*stmt, 6, commit.subject); !b) {
      return std::unexpected(commits_error::query_failed);
    }
    if (auto b = bind_opt(*stmt, 7, commit.author); !b) {
      return std::unexpected(commits_error::query_failed);
    }
    if (auto b = bind_opt(*stmt, 8, commit.committed_at); !b) {
      return std::unexpected(commits_error::query_failed);
    }
    if (auto s = stmt->step(); !s) {
      return std::unexpected(commits_error::query_failed);
    }

    // `select changes()` rather than a `sqlite3_changes` accessor — see
    // agentactivity.cpp's `changes()` helper for why. Not disturbed by the
    // SELECT that reads it (SQLite excludes statements that modify no
    // rows), so this is safe to call immediately after the INSERT.
    auto changes_stmt = conn.prepare(k_changes_sql);
    if (!changes_stmt) {
      return std::unexpected(commits_error::query_failed);
    }
    auto changes_step = changes_stmt->step();
    if (!changes_step || *changes_step != db::step_result::row) {
      return std::unexpected(commits_error::query_failed);
    }
    inserted += static_cast<std::size_t>(changes_stmt->column_int64(0));
  }
  return inserted;
}

} // namespace planar::engine::runtime::sessioncommits
