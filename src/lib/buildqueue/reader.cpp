/// @file reader.cpp
/// @brief Implementation of `planar.buildqueue.reader` (see reader.cppm).

module;

module planar.buildqueue.reader;

import std;
import planar.db;
import planar.json_dom;

namespace planar::buildqueue {

namespace {

/// @brief How long a read waits for a competing lock before it reports `busy`.
constexpr int k_busy_timeout_ms = 2000;

/// @brief The retention a database without a usable `tagged_history_days` row reads as.
constexpr int k_default_tagged_history_days = 90;

/// @brief The smallest stored `tagged_history_days` the reader accepts (705 § Settings).
constexpr int k_min_tagged_history_days = 1;

/// @brief The largest stored `tagged_history_days` the reader accepts (705 § Settings).
constexpr int k_max_tagged_history_days = 36500;

/// @brief The identity value every build queue database stores in `queue_identity.kind`.
constexpr std::string_view k_identity_kind = "planar-build-queue";

/// @brief One table the reader reads, and the columns it reads from it.
struct table_need {
  std::string_view              table;   ///< The table name.
  std::vector<std::string_view> columns; ///< Every column the reader selects from `table`.
};

/// @brief What a header read needs: the identity row and the settings table.
/// @return The table needs for `read_queue_facts`.
auto header_needs() -> std::vector<table_need> {
  return {
      {"queue_identity", {"kind", "created_at"}},
      {"queue_settings", {"key", "value"}},
  };
}

/// @brief What a run read needs: the header tables plus the history columns.
/// @return The table needs for `read_tagged_runs`.
auto run_needs() -> std::vector<table_need> {
  auto needs = header_needs();
  needs.push_back({"queue_history", {"seq", "tag", "argv", "cwd", "outcome", "exit_code", "ended_at"}});
  return needs;
}

/// @brief The header fields read from a database that has the identity row.
struct header {
  std::string created_at;                           ///< `queue_identity.created_at`, as stored.
  int         days = k_default_tagged_history_days; ///< The tagged retention.
};

/// @brief Whether the database file exists, as a read error when that cannot be
/// told (a parent directory that denies search).
/// @param path The database file.
/// @return `true` when the file exists, `false` when it does not, or the stat failure.
auto probe_present(const std::filesystem::path& path) -> std::expected<bool, reader_error> {
  std::error_code ec;
  bool const      present = std::filesystem::exists(path, ec);
  if (ec) {
    return std::unexpected(
        reader_error{reader_error_kind::unreadable, std::format("{}: cannot stat the file: {}", path.string(), ec.message())});
  }
  return present;
}

/// @brief Maps a SQLite failure on `path` to the reader's error kinds: a lock
/// past the bound is `busy`, anything else is `unreadable`.
/// @param err The driver failure.
/// @param path The database file the statement ran against.
/// @return The reader error.
auto from_db(const planar::db::db_error& err, const std::filesystem::path& path) -> reader_error {
  if (planar::db::is_busy(err)) {
    return {reader_error_kind::busy,
            std::format("{} is still locked by another process after {} seconds", path.string(), k_busy_timeout_ms / 1000)};
  }
  return {reader_error_kind::unreadable, std::format("{}: {}", path.string(), err.message_)};
}

/// @brief Checks that each needed table exists and has each needed column.
/// @param conn The read-only connection.
/// @param path The database file, for the reason text.
/// @param needs The tables and columns to require.
/// @return Success, or `incompatible` naming the first missing table or column.
auto check_tables(planar::db::connection& conn, const std::filesystem::path& path, const std::vector<table_need>& needs)
    -> std::expected<void, reader_error> {
  for (auto const& need : needs) {
    auto stmt = conn.prepare(std::format("pragma table_info({})", need.table));
    if (!stmt) {
      return std::unexpected(from_db(stmt.error(), path));
    }
    std::set<std::string, std::less<>> present;
    while (true) {
      auto step = stmt->step();
      if (!step) {
        return std::unexpected(from_db(step.error(), path));
      }
      if (*step == planar::db::step_result::done) {
        break;
      }
      present.insert(stmt->column_text(1));
    }
    if (present.empty()) {
      return std::unexpected(
          reader_error{reader_error_kind::incompatible,
                       std::format("{}: table {} is missing, so this is not a build queue database", path.string(), need.table)});
    }
    for (auto const& column : need.columns) {
      if (!present.contains(std::string{column})) {
        return std::unexpected(reader_error{reader_error_kind::incompatible,
                                            std::format("{}: table {} has no column {}", path.string(), need.table, column)});
      }
    }
  }
  return {};
}

/// @brief Reads the identity row, which names the database and records its creation time.
/// @param conn The read-only connection.
/// @param path The database file, for the reason text.
/// @return The header's creation time, or `incompatible` when the row is missing or names another kind.
auto read_identity(planar::db::connection& conn, const std::filesystem::path& path) -> std::expected<std::string, reader_error> {
  auto stmt = conn.prepare("select kind, created_at from queue_identity");
  if (!stmt) {
    return std::unexpected(from_db(stmt.error(), path));
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(from_db(step.error(), path));
  }
  if (*step == planar::db::step_result::done) {
    return std::unexpected(
        reader_error{reader_error_kind::incompatible,
                     std::format("{}: queue_identity has no row, so this is not a build queue database", path.string())});
  }
  if (stmt->column_text(0) != k_identity_kind) {
    return std::unexpected(
        reader_error{reader_error_kind::incompatible, std::format("{}: queue_identity names '{}', not a build queue database",
                                                                  path.string(), stmt->column_text(0))});
  }
  return stmt->column_text(1);
}

/// @brief Parses a stored retention value: a decimal integer in 1..36500 days, nothing else.
/// @param text The stored value.
/// @return The number of days, or nullopt when the text is not one in that range.
auto parse_days(std::string_view text) -> std::optional<int> {
  int value            = 0;
  auto const [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || ptr != text.data() + text.size() || value < k_min_tagged_history_days ||
      value > k_max_tagged_history_days) {
    return std::nullopt;
  }
  return value;
}

/// @brief Reads `tagged_history_days`. A missing row or an invalid value reads as the default.
/// @param conn The read-only connection.
/// @param path The database file, for the reason text.
/// @return The retention in days.
auto read_retention(planar::db::connection& conn, const std::filesystem::path& path) -> std::expected<int, reader_error> {
  auto stmt = conn.prepare("select value from queue_settings where key = 'tagged_history_days'");
  if (!stmt) {
    return std::unexpected(from_db(stmt.error(), path));
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(from_db(step.error(), path));
  }
  if (*step == planar::db::step_result::done) {
    return k_default_tagged_history_days;
  }
  return parse_days(stmt->column_text(0)).value_or(k_default_tagged_history_days);
}

/// @brief Reads the identity row and the retention setting.
/// @param conn The read-only connection, already checked for the needed columns.
/// @param path The database file, for the reason text.
/// @return The creation time and the retention.
auto read_header(planar::db::connection& conn, const std::filesystem::path& path) -> std::expected<header, reader_error> {
  auto created_at = read_identity(conn, path);
  if (!created_at) {
    return std::unexpected(created_at.error());
  }
  auto days = read_retention(conn, path);
  if (!days) {
    return std::unexpected(days.error());
  }
  return header{.created_at = std::move(*created_at), .days = *days};
}

// --- argv decoding ----------------------------------------------------------
//
// `queue_history.argv` stores a JSON array of strings, written by the engine's
// `encode_argv` (src/engine/hostqueue/queue.cpp). The reader decodes it with
// `planar.json_dom`, the parser the engine's `decode_argv` uses, so both read
// the same grammar.

/// @brief Decodes a stored argv: a JSON array whose every element is a string.
/// @param text The stored argv text.
/// @return The strings, or nullopt when the text is not such an array.
auto decode_argv(std::string_view text) -> std::optional<std::vector<std::string>> {
  auto parsed = json_dom::parse_json(text);
  if (!parsed || parsed->kind != json_dom::json_kind::array) {
    return std::nullopt;
  }
  std::vector<std::string> argv;
  argv.reserve(parsed->array.size());
  for (auto& element : parsed->array) {
    if (element.kind != json_dom::json_kind::string) {
      return std::nullopt;
    }
    argv.push_back(std::move(element.string));
  }
  return argv;
}

/// @brief Reads the runs tagged `tag`, in `ended_at`, then sequence, order.
/// @param conn The read-only connection, already checked for the needed columns.
/// @param path The database file, for the reason text.
/// @param tag The tag to match exactly.
/// @return The runs, or a `reader_error` for a query failure or an undecodable argv.
auto read_runs(planar::db::connection& conn, const std::filesystem::path& path, std::string_view tag)
    -> std::expected<std::vector<tagged_run>, reader_error> {
  auto stmt = conn.prepare("select argv, cwd, outcome, exit_code, ended_at from queue_history where tag = ?1 "
                           "order by ended_at, seq");
  if (!stmt) {
    return std::unexpected(from_db(stmt.error(), path));
  }
  if (auto bound = stmt->bind_text(1, tag); !bound) {
    return std::unexpected(from_db(bound.error(), path));
  }
  std::vector<tagged_run> runs;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(from_db(step.error(), path));
    }
    if (*step == planar::db::step_result::done) {
      break;
    }
    auto argv = decode_argv(stmt->column_text(0));
    if (!argv) {
      return std::unexpected(
          reader_error{reader_error_kind::unreadable,
                       std::format("{}: a stored argv in queue_history is not a JSON array of strings", path.string())});
    }
    tagged_run run;
    run.argv    = std::move(*argv);
    run.cwd     = stmt->column_text(1);
    run.outcome = stmt->column_text(2);
    if (!stmt->is_null(3)) {
      run.exit_code = stmt->column_int64(3);
    }
    run.ended_at = stmt->column_int64(4);
    runs.push_back(std::move(run));
  }
  return runs;
}

} // namespace

auto open_reader(const std::filesystem::path& path) -> std::expected<planar::db::connection, reader_error> {
  auto conn = planar::db::connection::open_existing(path.string(), k_busy_timeout_ms);
  if (!conn) {
    return std::unexpected(from_db(conn.error(), path));
  }
  if (auto set = conn->execute("pragma query_only = on"); !set) {
    return std::unexpected(from_db(set.error(), path));
  }
  return std::move(*conn);
}

auto read_queue_facts(const std::filesystem::path& path) -> std::expected<queue_facts, reader_error> {
  queue_facts out;
  auto        present = probe_present(path);
  if (!present) {
    return std::unexpected(present.error());
  }
  if (!*present) {
    return out;
  }
  auto conn = open_reader(path);
  if (!conn) {
    return std::unexpected(conn.error());
  }
  if (auto checked = check_tables(*conn, path, header_needs()); !checked) {
    return std::unexpected(checked.error());
  }
  auto head = read_header(*conn, path);
  if (!head) {
    return std::unexpected(head.error());
  }
  out.state               = reader_state::present;
  out.created_at          = std::move(head->created_at);
  out.tagged_history_days = head->days;
  return out;
}

auto read_tagged_runs(const std::filesystem::path& path, std::string_view tag) -> std::expected<tagged_runs, reader_error> {
  tagged_runs out;
  auto        present = probe_present(path);
  if (!present) {
    return std::unexpected(present.error());
  }
  if (!*present) {
    return out;
  }
  auto conn = open_reader(path);
  if (!conn) {
    return std::unexpected(conn.error());
  }
  if (auto checked = check_tables(*conn, path, run_needs()); !checked) {
    return std::unexpected(checked.error());
  }
  auto head = read_header(*conn, path);
  if (!head) {
    return std::unexpected(head.error());
  }
  auto runs = read_runs(*conn, path, tag);
  if (!runs) {
    return std::unexpected(runs.error());
  }
  out.state               = reader_state::present;
  out.created_at          = std::move(head->created_at);
  out.tagged_history_days = head->days;
  out.runs                = std::move(*runs);
  return out;
}

} // namespace planar::buildqueue
