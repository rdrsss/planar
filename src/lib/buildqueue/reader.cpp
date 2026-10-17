/// @file reader.cpp
/// @brief Implementation of `planar.buildqueue.reader` (see reader.cppm).

module;

module planar.buildqueue.reader;

import std;
import planar.db;

namespace planar::buildqueue {

namespace {

/// @brief How long a read waits for a competing lock before it reports `busy`.
constexpr int k_busy_timeout_ms = 2000;

/// @brief The retention a database without a usable `tagged_history_days` row reads as.
constexpr int k_default_tagged_history_days = 90;

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

/// @brief Parses a stored retention value: a non-negative decimal integer, nothing else.
/// @param text The stored value.
/// @return The number of days, or nullopt when the text is not one.
auto parse_days(std::string_view text) -> std::optional<int> {
  int value            = 0;
  auto const [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || ptr != text.data() + text.size() || value < 0) {
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
// `encode_argv` (src/engine/hostqueue/queue.cpp). Layer 1 cannot import the
// engine, so the decoder is a copy of the engine's `decode_argv` shape. It
// accepts the same grammar: a JSON array whose every element is a string, with
// the escapes JSON defines.

/// @brief A read position in the stored text.
struct cursor {
  std::string_view text;    ///< The stored text.
  std::size_t      pos = 0; ///< The next byte to read.
};

/// @brief Advances past JSON insignificant whitespace.
/// @param cur The read position, moved in place.
void skip_space(cursor& cur) {
  while (cur.pos < cur.text.size()) {
    char const ch = cur.text[cur.pos];
    if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
      return;
    }
    ++cur.pos;
  }
}

/// @brief Reads four hexadecimal digits at `at`.
/// @param text The stored text.
/// @param at The offset of the first digit.
/// @return The code unit, or nullopt when fewer than four hex digits are there.
auto parse_hex4(std::string_view text, std::size_t at) -> std::optional<std::uint32_t> {
  if (at + 4 > text.size()) {
    return std::nullopt;
  }
  std::uint32_t value = 0;
  for (std::size_t i = at; i < at + 4; ++i) {
    char const    ch    = text[i];
    std::uint32_t digit = 0;
    if (ch >= '0' && ch <= '9') {
      digit = static_cast<std::uint32_t>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      digit = static_cast<std::uint32_t>(ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
      digit = static_cast<std::uint32_t>(ch - 'A' + 10);
    } else {
      return std::nullopt;
    }
    value = (value << 4) | digit;
  }
  return value;
}

/// @brief Appends one code point as UTF-8.
/// @param out The string to append to.
/// @param cp The code point, at most U+10FFFF and not a surrogate.
void append_utf8(std::string& out, std::uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

/// @brief Reads one JSON string, starting at its opening quote.
/// @param cur The read position, moved past the closing quote on success.
/// @return The decoded string, or nullopt when the text is not a JSON string.
auto parse_string(cursor& cur) -> std::optional<std::string> {
  if (cur.pos >= cur.text.size() || cur.text[cur.pos] != '"') {
    return std::nullopt;
  }
  ++cur.pos;
  std::string out;
  while (cur.pos < cur.text.size()) {
    char const ch = cur.text[cur.pos++];
    if (ch == '"') {
      return out;
    }
    if (static_cast<unsigned char>(ch) < 0x20) {
      return std::nullopt;
    }
    if (ch != '\\') {
      out.push_back(ch);
      continue;
    }
    if (cur.pos >= cur.text.size()) {
      return std::nullopt;
    }
    char const esc = cur.text[cur.pos++];
    switch (esc) {
    case '"':
    case '\\':
    case '/':
      out.push_back(esc);
      break;
    case 'b':
      out.push_back('\b');
      break;
    case 'f':
      out.push_back('\f');
      break;
    case 'n':
      out.push_back('\n');
      break;
    case 'r':
      out.push_back('\r');
      break;
    case 't':
      out.push_back('\t');
      break;
    case 'u': {
      auto unit = parse_hex4(cur.text, cur.pos);
      if (!unit) {
        return std::nullopt;
      }
      cur.pos += 4;
      std::uint32_t cp = *unit;
      if (cp >= 0xD800 && cp <= 0xDBFF) {
        // A high surrogate must be followed by a low one, and the pair names one code point.
        if (cur.pos + 2 > cur.text.size() || cur.text[cur.pos] != '\\' || cur.text[cur.pos + 1] != 'u') {
          return std::nullopt;
        }
        auto low = parse_hex4(cur.text, cur.pos + 2);
        if (!low || *low < 0xDC00 || *low > 0xDFFF) {
          return std::nullopt;
        }
        cur.pos += 6;
        cp = 0x10000 + ((cp - 0xD800) << 10) + (*low - 0xDC00);
      } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
        return std::nullopt;
      }
      append_utf8(out, cp);
      break;
    }
    default:
      return std::nullopt;
    }
  }
  return std::nullopt;
}

/// @brief Decodes a stored `argv` column.
/// @param text The column text.
/// @return The arguments, or nullopt when the text is not a JSON array of strings.
auto decode_argv(std::string_view text) -> std::optional<std::vector<std::string>> {
  cursor cur{.text = text};
  skip_space(cur);
  if (cur.pos >= text.size() || text[cur.pos] != '[') {
    return std::nullopt;
  }
  ++cur.pos;
  std::vector<std::string> argv;
  skip_space(cur);
  if (cur.pos < text.size() && text[cur.pos] == ']') {
    ++cur.pos;
  } else {
    while (true) {
      skip_space(cur);
      auto element = parse_string(cur);
      if (!element) {
        return std::nullopt;
      }
      argv.push_back(std::move(*element));
      skip_space(cur);
      if (cur.pos >= text.size()) {
        return std::nullopt;
      }
      if (text[cur.pos] == ',') {
        ++cur.pos;
        continue;
      }
      if (text[cur.pos] == ']') {
        ++cur.pos;
        break;
      }
      return std::nullopt;
    }
  }
  skip_space(cur);
  if (cur.pos != text.size()) {
    return std::nullopt;
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
