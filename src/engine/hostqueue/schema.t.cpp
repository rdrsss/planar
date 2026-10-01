// @file schema.t.cpp
// @brief Unit tests for `planar.engine.hostqueue.schema` (plan 1089, task
// qp-queue-compat; tech spec 656 § Queue compatibility check, § Enforcement).
//
// Covers the test-spec scenarios:
//  - "Happy path -- the queue check passes at head and on a compatible ahead
//    database";
//  - "Error -- the queue check refuses an incompatible marker and shape drift";
//  - "Error -- a schema constant that disagrees with the chain fails";
//  - "Error -- changing queue SQL without bumping the queue version fails the
//    fingerprint";
//  - "Edge -- whitespace and keyword case do not move the fingerprint, and
//    re-pinning needs a reason".
// The marker-presence scan over the embedded chain lives in
// src/lib/db/migrate.t.cpp.
//
// Every store here is a scratch planar.db built from the real embedded main
// chain; nothing reads the process environment or the real ~/.planar.
//
// The fingerprint reads the hostqueue sources from the source tree
// (PLANAR_HOSTQUEUE_SOURCE_DIR), exactly as rule_methodology.t.cpp reads
// agents/methodology.md.
//
// Include-before-import is deliberate (see core/version.t.cpp / db.t.cpp).
#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;
import planar.engine.hostqueue;
import planar.process.identity;
import planar.sha256;

namespace {

namespace hq = planar::engine::hostqueue;

// ---------------------------------------------------------------------------
// The pinned, append-only fingerprint ledger (tech spec 656 § Re-pinning).
//
// When the fingerprint test fails, choose ONE:
//  - behaviour-neutral change (a refactor, a renamed alias, a reordered but
//    equivalent query): APPEND {same version, new fingerprint, reason}. The
//    reason must be non-empty and the reviewer approves it. Never edit or
//    delete an existing entry.
//  - any change in what is stored or how rows are judged: bump
//    `k_queue_schema_version` and add a `queue_schema` marker migration
//    (compat per docs/architecture.md § The host queue's tables), then append
//    the entry for the new version.
// ---------------------------------------------------------------------------

struct fingerprint_pin {
  std::uint32_t    version = 0;
  std::string_view fingerprint;
  std::string_view reason;
};

constexpr std::array k_fingerprint_ledger{
    fingerprint_pin{1, "0000000000000000000000000000000000000000000000000000000000000000",
                    "queue version 1: queue_entries and queue_history in planar.db (plan 1089, migration 00040)"},
};

// ---------------------------------------------------------------------------
// Scratch databases.
// ---------------------------------------------------------------------------

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_hostqueue_schema_test_{}_{}.db",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
  }

  scratch_db_path(const scratch_db_path&)            = delete;
  scratch_db_path& operator=(const scratch_db_path&) = delete;

  ~scratch_db_path() {
    std::error_code ec;
    for (auto const* suffix : {"", "-journal", "-wal", "-shm"}) {
      std::filesystem::remove(path_.string() + suffix, ec);
    }
  }
};

// A scratch planar.db migrated to the head of the embedded main chain.
auto open_head(const scratch_db_path& scratch) -> planar::db::connection {
  auto conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_all(*conn));
  return std::move(*conn);
}

auto scalar(planar::db::connection& conn, std::string_view sql) -> std::string {
  INFO(sql);
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().value() == planar::db::step_result::row);
  return stmt->is_null(0) ? std::string{"null"} : stmt->column_text(0);
}

auto column_names(planar::db::connection& conn, std::string_view table) -> std::vector<std::string> {
  auto stmt = conn.prepare(std::format("select name from pragma_table_info('{}') order by cid", table));
  REQUIRE(stmt.has_value());
  std::vector<std::string> out;
  while (stmt->step().value() == planar::db::step_result::row) {
    out.push_back(stmt->column_text(0));
  }
  return out;
}

// The quoted values of `column`'s `check (column in (...))` in `table`'s
// stored CREATE TABLE text, joined with `,` in declaration order.
auto check_values(planar::db::connection& conn, std::string_view table, std::string_view column) -> std::string {
  auto const       sql = scalar(conn, std::format("select sql from sqlite_master where type = 'table' and name = '{}'", table));
  std::regex const clause(std::format(R"(check\s*\(\s*{}\s+in\s*\(([^)]*)\))", column), std::regex::icase);
  std::smatch      match;
  INFO(sql);
  REQUIRE(std::regex_search(sql, match, clause));
  std::string const list = match[1].str();
  std::regex const  quoted("'([^']*)'");
  std::string       out;
  for (auto it = std::sregex_iterator(list.begin(), list.end(), quoted); it != std::sregex_iterator(); ++it) {
    out += out.empty() ? "" : ",";
    out += (*it)[1].str();
  }
  return out;
}

auto join(std::span<std::string_view const> parts) -> std::string {
  std::string out;
  for (auto const part : parts) {
    out += out.empty() ? "" : ",";
    out += part;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Constant equals chain.
// ---------------------------------------------------------------------------

// The highest `queue_schema.version` a chain leaves behind: the chain applied
// to a scratch database, then read back.
auto highest_queue_marker(std::span<planar::db::migration_record const> chain) -> std::uint32_t {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  auto const applied = planar::db::apply_all(*conn, chain);
  INFO((applied ? std::string{} : applied.error().message_));
  REQUIRE(applied.has_value());
  return static_cast<std::uint32_t>(std::stoul(scalar(*conn, "select ifnull(max(version), 0) from queue_schema")));
}

// Fails, naming both values, when `constant` is not the chain's highest marker.
auto check_constant_matches_chain(std::span<planar::db::migration_record const> chain, std::uint32_t constant)
    -> std::expected<void, std::string> {
  auto const highest = highest_queue_marker(chain);
  if (highest == constant) {
    return {};
  }
  return std::unexpected(std::format("k_queue_schema_version is {} but the migration chain's highest queue_schema.version is {}; "
                                     "a marker migration must bump the constant with it",
                                     constant, highest));
}

// ---------------------------------------------------------------------------
// The protocol fingerprint (tech spec 656 § Enforcement).
// ---------------------------------------------------------------------------

constexpr std::array<std::string_view, 60> k_sql_keywords{
    "select",  "insert",   "into",   "update", "set",    "delete",    "from",    "where",    "and",     "or",
    "not",     "null",     "is",     "in",     "values", "returning", "order",   "by",       "group",   "having",
    "limit",   "offset",   "as",     "on",     "join",   "left",      "inner",   "create",   "table",   "index",
    "alter",   "add",      "column", "drop",   "count",  "exists",    "primary", "key",      "integer", "text",
    "default", "check",    "unique", "asc",    "desc",   "case",      "when",    "then",     "else",    "end",
    "pragma",  "distinct", "union",  "all",    "max",    "min",       "ifnull",  "coalesce", "like",    "between",
};

auto lower(std::string_view text) -> std::string {
  std::string out(text);
  std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

auto is_ident_char(char c) -> bool {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// Every string literal in C++ source `src`, in source order, with adjacent
// literals (separated only by whitespace or comments) concatenated as the
// compiler does. Comments, character literals and digit separators are
// skipped; raw strings are read verbatim; escapes are kept as written.
auto string_literals(std::string_view src) -> std::vector<std::string> {
  std::vector<std::string> out;
  bool                     adjacent = false;
  std::size_t              i        = 0;
  while (i < src.size()) {
    if (src.substr(i, 2) == "//") {
      i = src.find('\n', i);
      if (i == std::string_view::npos) {
        break;
      }
      continue;
    }
    if (src.substr(i, 2) == "/*") {
      auto const end = src.find("*/", i + 2);
      i              = end == std::string_view::npos ? src.size() : end + 2;
      continue;
    }
    char const c = src[i];
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      ++i;
      continue;
    }
    if (c == '"') {
      std::string body;
      if (i > 0 && src[i - 1] == 'R') {
        auto const open  = src.find('(', i);
        auto const delim = src.substr(i + 1, open - i - 1);
        auto const close = src.find(std::format("){}\"", delim), open);
        REQUIRE(close != std::string_view::npos);
        body = std::string(src.substr(open + 1, close - open - 1));
        i    = close + delim.size() + 2;
      } else {
        std::size_t j = i + 1;
        while (j < src.size() && src[j] != '"') {
          auto const width = (src[j] == '\\' && j + 1 < src.size()) ? 2U : 1U;
          body += src.substr(j, width);
          j += width;
        }
        i = j + 1;
      }
      if (adjacent) {
        out.back() += body;
      } else {
        out.push_back(std::move(body));
      }
      adjacent = true;
      continue;
    }
    if (c == '\'') {
      if (i > 0 && std::isalnum(static_cast<unsigned char>(src[i - 1])) != 0) {
        ++i; // a digit separator: 86'400'000
        continue;
      }
      std::size_t j = i + 1;
      while (j < src.size() && src[j] != '\'') {
        j += (src[j] == '\\') ? 2 : 1;
      }
      i        = j + 1;
      adjacent = false;
      continue;
    }
    if (is_ident_char(c)) {
      auto j = i;
      while (j < src.size() && is_ident_char(src[j])) {
        ++j;
      }
      // A literal's encoding or raw prefix does not break adjacency.
      auto const word = src.substr(i, j - i);
      if (!(j < src.size() && src[j] == '"' && (word == "R" || word == "u8" || word == "u8R" || word == "L" || word == "LR"))) {
        adjacent = false;
      }
      i = j;
      continue;
    }
    adjacent = false;
    ++i;
  }
  return out;
}

const std::regex k_queue_table(R"(\b(queue_entries|queue_history|queue_schema|sqlite_sequence)\b)");
const std::regex
    k_sql_word(R"(\b(select|insert|update|delete|create|alter|drop|from|where|returning|values|pragma|order\s+by)\b)",
               std::regex::icase);
const std::regex k_identifier_list(R"(^\s*[a-z_][a-z0-9_]*(\s*,\s*[a-z_][a-z0-9_]*)+\s*,?\s*$)");

// Whether a literal is SQL for the fingerprint: it contains a SQL keyword
// (the separability rule, plus the fragment keywords a format string carries),
// names a queue table, or is a bare column list.
auto is_sql_literal(std::string_view literal) -> bool {
  std::string const text(literal);
  return std::regex_search(text, k_sql_word) || std::regex_search(text, k_queue_table) ||
         std::regex_match(text, k_identifier_list);
}

// Whitespace runs collapse to one space, the literal is trimmed, and SQL
// keywords outside single-quoted strings are lowercased.
auto normalize_sql(std::string_view literal) -> std::string {
  std::string collapsed;
  bool        space = false;
  for (char const c : literal) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      space = true;
      continue;
    }
    if (space && !collapsed.empty()) {
      collapsed += ' ';
    }
    space = false;
    collapsed += c;
  }
  std::string out;
  bool        quoted = false;
  for (std::size_t i = 0; i < collapsed.size();) {
    char const c = collapsed[i];
    if (c == '\'') {
      quoted = !quoted;
    }
    if (!quoted && is_ident_char(c) && (i == 0 || !is_ident_char(collapsed[i - 1]))) {
      auto j = i;
      while (j < collapsed.size() && is_ident_char(collapsed[j])) {
        ++j;
      }
      auto const word  = std::string_view(collapsed).substr(i, j - i);
      auto const lowed = lower(word);
      out += std::ranges::find(k_sql_keywords, lowed) != k_sql_keywords.end() ? lowed : std::string(word);
      i = j;
      continue;
    }
    out += c;
    ++i;
  }
  return out;
}

// The normalized SQL literals of each source, in source order.
auto sql_literals(std::span<std::string const> sources) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (auto const& source : sources) {
    for (auto const& literal : string_literals(source)) {
      if (is_sql_literal(literal)) {
        out.push_back(normalize_sql(literal));
      }
    }
  }
  return out;
}

auto fingerprint(std::span<std::string const> sources, std::span<std::string_view const> protocol) -> std::string {
  std::string input;
  for (auto const& sql : sql_literals(sources)) {
    input += std::format("sql {}\n", sql);
  }
  for (auto const entry : protocol) {
    input += std::format("protocol {}\n", entry);
  }
  return planar::sha256::hex(input);
}

// `src/engine/hostqueue/*.cpp` minus `*.t.cpp`, sorted by file name.
auto hostqueue_source_paths() -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> out;
  for (auto const& entry : std::filesystem::directory_iterator(PLANAR_HOSTQUEUE_SOURCE_DIR)) {
    auto const name = entry.path().filename().string();
    if (name.ends_with(".cpp") && !name.ends_with(".t.cpp")) {
      out.push_back(entry.path());
    }
  }
  std::ranges::sort(out, {}, [](auto const& p) { return p.filename().string(); });
  return out;
}

auto read_file(std::filesystem::path const& path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  INFO(path.string());
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

auto hostqueue_sources() -> std::vector<std::string> {
  std::vector<std::string> out;
  for (auto const& path : hostqueue_source_paths()) {
    out.push_back(read_file(path));
  }
  return out;
}

// The ledger rules: every entry has a non-empty reason, a 64-hex fingerprint
// and a version no lower than the one before it; and the LAST entry for
// `version` pins `current`. A mismatch names both remedies.
auto verify_ledger(std::span<fingerprint_pin const> ledger, std::uint32_t version, std::string_view current)
    -> std::expected<void, std::string> {
  if (ledger.empty()) {
    return std::unexpected(std::string{"the fingerprint ledger is empty"});
  }
  std::uint32_t                  previous = 0;
  std::optional<fingerprint_pin> last_for_version;
  for (std::size_t i = 0; i < ledger.size(); ++i) {
    auto const& pin = ledger[i];
    if (pin.reason.find_first_not_of(" \t") == std::string_view::npos) {
      return std::unexpected(std::format("fingerprint ledger entry {} (queue version {}) has an empty reason; every re-pin "
                                         "must say why the change is behaviour-neutral",
                                         i, pin.version));
    }
    if (pin.fingerprint.size() != 64 || !std::ranges::all_of(pin.fingerprint, [](char c) { return std::isxdigit(c) != 0; })) {
      return std::unexpected(std::format("fingerprint ledger entry {} is not a 64-character hex digest", i));
    }
    if (pin.version < previous) {
      return std::unexpected(
          std::format("fingerprint ledger entry {} goes back to queue version {}; the ledger is append-only", i, pin.version));
    }
    previous = pin.version;
    if (pin.version == version) {
      last_for_version = pin;
    }
  }
  if (last_for_version && last_for_version->fingerprint == current) {
    return {};
  }
  return std::unexpected(std::format(
      "the hostqueue protocol fingerprint is {} but the ledger pins {} for queue version {}. Either: "
      "(1) behaviour-neutral change (a refactor, a renamed alias, a reordered but equivalent query): append "
      "{{{}, \"{}\", \"<reason>\"}} to k_fingerprint_ledger in src/engine/hostqueue/schema.t.cpp, with a non-empty reason "
      "a reviewer approves; or (2) a change in what is stored or how rows are judged: bump k_queue_schema_version and add "
      "a queue_schema marker migration (compat per docs/architecture.md § The host queue's tables)",
      current, last_for_version ? last_for_version->fingerprint : std::string_view{"nothing"}, version, version, current));
}

// The columns a hostqueue SQL literal names, keyed by the table they must
// belong to. Only statement-shaped literals are checked: one that starts with
// a statement keyword or a `{}` format fragment, or is a bare column list.
struct named_columns {
  std::string              literal;
  std::set<std::string>    allowed;
  std::vector<std::string> names;
};

auto columns_named_in(std::span<std::string const> sql) -> std::vector<named_columns> {
  std::set<std::string> entries(hq::k_queue_entries_columns.begin(), hq::k_queue_entries_columns.end());
  std::set<std::string> history(hq::k_queue_history_columns.begin(), hq::k_queue_history_columns.end());
  std::set<std::string> union_of = entries;
  union_of.insert(history.begin(), history.end());
  std::set<std::string> const marker{"version", "compat", "description"};

  std::regex const statement(R"(^(select|insert|update|delete|\{\})\b)");
  std::regex const quoted("'[^']*'");
  std::regex const word(R"([a-z_][a-z0-9_]*)");

  std::vector<named_columns> out;
  for (auto const& literal : sql) {
    bool const is_list = std::regex_match(literal, k_identifier_list);
    if (!is_list &&
        !(std::regex_search(literal, statement) && (literal.starts_with("{}") || std::regex_search(literal, k_queue_table)))) {
      continue;
    }
    bool const    names_entries = literal.contains("queue_entries");
    bool const    names_history = literal.contains("queue_history");
    bool const    names_marker  = literal.contains("queue_schema");
    named_columns found{.literal = literal, .allowed = union_of, .names = {}};
    if (names_entries && !names_history) {
      found.allowed = entries;
    } else if (names_history && !names_entries) {
      found.allowed = history;
    } else if (names_marker && !names_entries && !names_history) {
      found.allowed = marker;
    }
    auto const stripped = std::regex_replace(literal, quoted, "''");
    for (auto it = std::sregex_iterator(stripped.begin(), stripped.end(), word); it != std::sregex_iterator(); ++it) {
      auto const name = (*it)[0].str();
      if (std::ranges::find(k_sql_keywords, name) != k_sql_keywords.end() || std::regex_match(name, k_queue_table)) {
        continue;
      }
      found.names.push_back(name);
    }
    out.push_back(std::move(found));
  }
  return out;
}

} // namespace

// ===========================================================================
// check_queue_schema
// ===========================================================================

TEST_CASE("check_queue_schema passes on a planar.db at head", "[hostqueue][schema][qp-queue-compat]") {
  scratch_db_path scratch;
  auto            conn    = open_head(scratch);
  auto const      checked = hq::check_queue_schema(conn);
  INFO((checked ? std::string{} : checked.error().message));
  CHECK(checked.has_value());
}

TEST_CASE("check_queue_schema passes on an ahead planar.db whose newer marker keeps compat 1 and adds a nullable column",
          "[hostqueue][schema][qp-queue-compat]") {
  scratch_db_path scratch;
  auto            conn = open_head(scratch);
  auto const      head = planar::db::embedded_max();
  REQUIRE(conn.execute("alter table queue_history add column note text"));
  REQUIRE(conn.execute("insert into queue_schema (version, compat, description) values (2, 1, 'adds queue_history.note')"));
  REQUIRE(conn.execute(std::format("insert into schema_migrations (version, description) values ({}, 'newer')", head + 1)));

  auto const checked = hq::check_queue_schema(conn);
  INFO((checked ? std::string{} : checked.error().message));
  CHECK(checked.has_value());
}

TEST_CASE("check_queue_schema refuses a marker whose compat is above this binary's queue version, naming both versions",
          "[hostqueue][schema][qp-queue-compat]") {
  scratch_db_path scratch;
  auto            conn = open_head(scratch);
  REQUIRE(conn.execute("insert into queue_schema (version, compat, description) values (2, 2, 'incompatible')"));

  auto const checked = hq::check_queue_schema(conn);
  REQUIRE_FALSE(checked.has_value());
  CHECK(checked.error().kind == hq::queue_schema_failure::incompatible_marker);
  CHECK(checked.error().store_version == 2U);
  CHECK(checked.error().store_compat == 2U);
  INFO(checked.error().message);
  CHECK(checked.error().message.contains("queue version 2"));
  CHECK(checked.error().message.contains(std::format("queue version {}", hq::k_queue_schema_version)));
}

TEST_CASE("check_queue_schema reads only the HIGHEST marker row", "[hostqueue][schema][qp-queue-compat]") {
  // An old incompatible row below a newer compatible one does not refuse;
  // a newer incompatible row above an old compatible one does.
  scratch_db_path scratch;
  auto            conn = open_head(scratch);
  REQUIRE(conn.execute("insert into queue_schema (version, compat, description) values (5, 1, 'newest, compatible')"));
  REQUIRE(conn.execute("update queue_schema set compat = 9 where version = 1"));
  CHECK(hq::check_queue_schema(conn).has_value());

  REQUIRE(conn.execute("insert into queue_schema (version, compat, description) values (6, 6, 'newest, incompatible')"));
  auto const checked = hq::check_queue_schema(conn);
  REQUIRE_FALSE(checked.has_value());
  CHECK(checked.error().store_version == 6U);
}

TEST_CASE("check_queue_schema's column guard names a renamed queue_entries column when the marker is unchanged",
          "[hostqueue][schema][qp-queue-compat]") {
  scratch_db_path scratch;
  auto            conn = open_head(scratch);
  REQUIRE(conn.execute("alter table queue_entries rename column child_pgid to child_group"));

  auto const checked = hq::check_queue_schema(conn);
  REQUIRE_FALSE(checked.has_value());
  CHECK(checked.error().kind == hq::queue_schema_failure::missing_column);
  INFO(checked.error().message);
  CHECK(checked.error().message.contains("queue_entries.child_pgid"));
  CHECK_FALSE(checked.error().message.contains("queue_history."));
}

TEST_CASE("check_queue_schema's column guard covers queue_history too", "[hostqueue][schema][qp-queue-compat]") {
  scratch_db_path scratch;
  auto            conn = open_head(scratch);
  REQUIRE(conn.execute("alter table queue_history drop column wait_limit_ms"));

  auto const checked = hq::check_queue_schema(conn);
  REQUIRE_FALSE(checked.has_value());
  CHECK(checked.error().kind == hq::queue_schema_failure::missing_column);
  INFO(checked.error().message);
  CHECK(checked.error().message.contains("queue_history.wait_limit_ms"));
}

TEST_CASE("check_queue_schema refuses when a queue table or the marker is missing, or the marker is empty",
          "[hostqueue][schema][qp-queue-compat]") {
  for (auto const table : {"queue_schema", "queue_entries", "queue_history"}) {
    INFO(table);
    scratch_db_path scratch;
    auto            conn = open_head(scratch);
    REQUIRE(conn.execute(std::format("drop table {}", table)));
    auto const checked = hq::check_queue_schema(conn);
    REQUIRE_FALSE(checked.has_value());
    CHECK(checked.error().kind == hq::queue_schema_failure::missing_table);
    CHECK(checked.error().message.contains(table));
  }

  scratch_db_path scratch;
  auto            conn = open_head(scratch);
  REQUIRE(conn.execute("delete from queue_schema"));
  auto const checked = hq::check_queue_schema(conn);
  REQUIRE_FALSE(checked.has_value());
  CHECK(checked.error().kind == hq::queue_schema_failure::incompatible_marker);
  CHECK(checked.error().message.contains("queue_schema"));
}

TEST_CASE("check_queue_schema runs on a read-only connection and writes nothing", "[hostqueue][schema][qp-queue-compat]") {
  scratch_db_path scratch;
  {
    auto conn = open_head(scratch);
  }
  auto ro = planar::db::connection::open_read_only(scratch.path_.string());
  REQUIRE(ro.has_value());
  auto const checked = hq::check_queue_schema(*ro);
  INFO((checked ? std::string{} : checked.error().message));
  CHECK(checked.has_value());
}

TEST_CASE("an equal-version failure is queue_schema_foreign, an ahead one queue_schema_incompatible, and neither is "
          "schema_version_ahead",
          "[hostqueue][schema][qp-queue-compat]") {
  CHECK(hq::queue_schema_refusal_tag(40, 40) == hq::k_tag_queue_schema_foreign);
  CHECK(hq::queue_schema_refusal_tag(41, 40) == hq::k_tag_queue_schema_incompatible);
  CHECK(hq::queue_schema_refusal_tag(39, 40) == std::nullopt);
  CHECK(hq::k_tag_queue_schema_foreign == "queue_schema_foreign");
  CHECK(hq::k_tag_queue_schema_incompatible == "queue_schema_incompatible");
  CHECK(hq::k_tag_queue_schema_foreign != hq::k_tag_queue_schema_incompatible);
  for (auto const tag : {hq::k_tag_queue_schema_foreign, hq::k_tag_queue_schema_incompatible}) {
    CHECK(tag != "schema_version_ahead");
  }
}

// ===========================================================================
// The column lists
// ===========================================================================

TEST_CASE("k_queue_entries_columns and k_queue_history_columns equal pragma table_info at head",
          "[hostqueue][schema][qp-queue-compat]") {
  scratch_db_path scratch;
  auto            conn = open_head(scratch);
  CHECK(column_names(conn, "queue_entries") ==
        std::vector<std::string>(hq::k_queue_entries_columns.begin(), hq::k_queue_entries_columns.end()));
  CHECK(column_names(conn, "queue_history") ==
        std::vector<std::string>(hq::k_queue_history_columns.begin(), hq::k_queue_history_columns.end()));
}

TEST_CASE("every column a hostqueue SQL literal names is in its table's column list", "[hostqueue][schema][qp-queue-compat]") {
  auto const sources  = hostqueue_sources();
  auto const literals = sql_literals(sources);
  auto const checked  = columns_named_in(literals);
  // Non-vacuity: the scan reaches the statements that matter.
  REQUIRE(checked.size() >= 15);
  CHECK(std::ranges::any_of(checked, [](auto const& c) { return c.literal.starts_with("insert into queue_entries"); }));
  CHECK(std::ranges::any_of(checked, [](auto const& c) { return c.literal.starts_with("insert into queue_history"); }));
  CHECK(std::ranges::any_of(
      checked, [](auto const& c) { return std::ranges::find(c.names, std::string{"wait_deadline_mono"}) != c.names.end(); }));
  for (auto const& found : checked) {
    for (auto const& name : found.names) {
      INFO(found.literal);
      INFO(name);
      CHECK(found.allowed.contains(name));
    }
  }
}

TEST_CASE("the column-literal scan catches a column that is not in its table's list", "[hostqueue][schema][qp-queue-compat]") {
  std::vector<std::string> const literals{
      normalize_sql("update queue_entries set refreshed_mono = ?, outcome = ? where seq = ?"),
      normalize_sql("seq, state, bogus_column"),
  };
  auto const checked = columns_named_in(literals);
  REQUIRE(checked.size() == 2);
  CHECK_FALSE(checked[0].allowed.contains("outcome")); // a history column, named against queue_entries
  CHECK(std::ranges::find(checked[0].names, std::string{"outcome"}) != checked[0].names.end());
  CHECK(std::ranges::find(checked[1].names, std::string{"bogus_column"}) != checked[1].names.end());
  CHECK_FALSE(checked[1].allowed.contains("bogus_column"));
}

// ===========================================================================
// Constant equals chain
// ===========================================================================

TEST_CASE("k_queue_schema_version equals the highest queue_schema.version in the embedded chain",
          "[hostqueue][schema][qp-queue-compat]") {
  auto const checked = check_constant_matches_chain(planar::db::migrations(), hq::k_queue_schema_version);
  INFO((checked ? std::string{} : checked.error()));
  CHECK(checked.has_value());
}

TEST_CASE(
    "a synthetic chain whose newest migration inserts queue_schema (2, 2) fails the constant-equals-chain test, naming both "
    "values",
    "[hostqueue][schema][qp-queue-compat]") {
  auto const        embedded = planar::db::migrations();
  auto const        next     = embedded.back().version_ + 1;
  std::string const up       = std::format("insert into queue_schema (version, compat, description) values (2, 2, 'bump');"
                                           "insert into schema_migrations (version, description) values ({}, 'bump');",
                                           next);
  std::vector<planar::db::migration_record> chain(embedded.begin(), embedded.end());
  chain.push_back(planar::db::migration_record{.version_ = next, .name_ = "queue_marker_bump", .up_sql_ = up, .down_sql_ = ""});

  auto const checked = check_constant_matches_chain(chain, 1);
  REQUIRE_FALSE(checked.has_value());
  INFO(checked.error());
  CHECK(checked.error().contains("k_queue_schema_version is 1"));
  CHECK(checked.error().contains("highest queue_schema.version is 2"));
  // ...and agreeing with it passes.
  CHECK(check_constant_matches_chain(chain, 2).has_value());
}

// ===========================================================================
// The protocol constants
// ===========================================================================

TEST_CASE("the k_queue_protocol entries with a code counterpart equal it", "[hostqueue][schema][qp-queue-compat]") {
  scratch_db_path scratch;
  auto            conn = open_head(scratch);

  SECTION("entry_states: entry_state and the queue_entries.state CHECK") {
    auto const value = hq::protocol_value("entry_states");
    REQUIRE(value.has_value());
    CHECK(*value == std::format("{},{}", hq::to_string(hq::entry_state::waiting), hq::to_string(hq::entry_state::running)));
    CHECK(*value == check_values(conn, "queue_entries", "state"));
  }
  SECTION("terminate_reasons: the queue_entries.terminate_reason CHECK") {
    auto const value = hq::protocol_value("terminate_reasons");
    REQUIRE(value.has_value());
    CHECK(*value == check_values(conn, "queue_entries", "terminate_reason"));
  }
  SECTION("outcomes: every history_outcome's stored text, in order, and the queue_history.outcome CHECK") {
    auto const value = hq::protocol_value("outcomes");
    REQUIRE(value.has_value());
    std::array const outcomes{hq::history_outcome::exited,    hq::history_outcome::signaled,     hq::history_outcome::timeout,
                              hq::history_outcome::cancelled, hq::history_outcome::wait_timeout, hq::history_outcome::not_started,
                              hq::history_outcome::abandoned};
    std::vector<std::string_view> texts;
    for (auto const outcome : outcomes) {
      texts.push_back(hq::to_string(outcome));
      CHECK(hq::parse_history_outcome(hq::to_string(outcome)) == outcome);
    }
    CHECK(*value == join(texts));
    CHECK(*value == check_values(conn, "queue_history", "outcome"));
  }
  SECTION("submitter_identity: queue_entries columns") {
    auto const value = hq::protocol_value("submitter_identity");
    REQUIRE(value.has_value());
    CHECK(*value == "host_id,pid,pid_started");
    for (auto const column : std::views::split(*value, ',')) {
      auto const name = std::string_view(column.begin(), column.end());
      INFO(name);
      CHECK(std::ranges::find(hq::k_queue_entries_columns, name) != hq::k_queue_entries_columns.end());
    }
  }
  SECTION("unknown_host: k_unknown_host_identity") {
    CHECK(hq::protocol_value("unknown_host") == planar::process::identity::k_unknown_host_identity);
  }
  SECTION("seq_floor: the migration's sqlite_sequence row") {
    CHECK(hq::protocol_value("seq_floor") == scalar(conn, "select seq from sqlite_sequence where name = 'queue_entries'"));
  }
  SECTION("history_day_ms: k_ms_per_day") {
    CHECK(hq::protocol_value("history_day_ms") == std::to_string(hq::k_ms_per_day));
  }
}

TEST_CASE("the four fingerprint-only labels are present and non-empty", "[hostqueue][schema][qp-queue-compat]") {
  for (auto const label : {"freshness", "running_live", "slot_rule", "nested_rule"}) {
    INFO(label);
    auto const value = hq::protocol_value(label);
    REQUIRE(value.has_value());
    CHECK_FALSE(value->empty());
  }
  CHECK(hq::k_queue_protocol.size() == 11);
  CHECK_FALSE(hq::protocol_value("no_such_entry").has_value());
}

TEST_CASE("the first enqueue into a fresh planar.db gets seq 1000001", "[hostqueue][schema][qp-migration]") {
  scratch_db_path scratch;
  auto            conn = open_head(scratch);
  auto const      seq  = hq::enqueue(
      conn, hq::enqueue_request{
                .host_id = "h", .pid = 1, .pid_started = 1, .cwd = "/", .argv = {"true"}, .enqueued_at = 0, .refreshed_mono = 0});
  REQUIRE(seq.has_value());
  CHECK(*seq == 1'000'001);
}

// ===========================================================================
// The protocol fingerprint
// ===========================================================================

TEST_CASE("the hostqueue protocol fingerprint matches the ledger's last entry for k_queue_schema_version",
          "[hostqueue][schema][qp-queue-compat][fingerprint]") {
  auto const sources = hostqueue_sources();
  REQUIRE(sources.size() >= 9); // queue, history, liveness, poll, terminate, nested, status, guard, rule, schema
  auto const current  = fingerprint(sources, hq::k_queue_protocol);
  auto const verified = verify_ledger(k_fingerprint_ledger, hq::k_queue_schema_version, current);
  INFO((verified ? std::string{} : verified.error()));
  CHECK(verified.has_value());
}

TEST_CASE("mutating one hostqueue SQL literal or one k_queue_protocol value fails the fingerprint, naming both remedies",
          "[hostqueue][schema][qp-queue-compat][fingerprint]") {
  auto const paths   = hostqueue_source_paths();
  auto       sources = hostqueue_sources();
  auto const pinned  = fingerprint(sources, hq::k_queue_protocol);

  auto const expect_both_remedies = [&](std::string_view moved) {
    std::array const pins{fingerprint_pin{hq::k_queue_schema_version, pinned, "pinned"}};
    auto const       verified = verify_ledger(pins, hq::k_queue_schema_version, moved);
    REQUIRE_FALSE(verified.has_value());
    INFO(verified.error());
    CHECK(verified.error().contains("behaviour-neutral"));
    CHECK(verified.error().contains("non-empty reason"));
    CHECK(verified.error().contains("bump k_queue_schema_version"));
    CHECK(verified.error().contains("queue_schema marker migration"));
  };

  SECTION("a SQL literal") {
    auto const at = std::ranges::find_if(paths, [](auto const& p) { return p.filename() == "queue.cpp"; });
    REQUIRE(at != paths.end());
    auto&      queue_cpp = sources[static_cast<std::size_t>(std::distance(paths.begin(), at))];
    auto const literal   = std::string{"\"{} order by seq\""};
    auto const where     = queue_cpp.find(literal);
    REQUIRE(where != std::string::npos);
    queue_cpp.replace(where, literal.size(), "\"{} order by seq desc\"");
    auto const moved = fingerprint(sources, hq::k_queue_protocol);
    CHECK(moved != pinned);
    expect_both_remedies(moved);

    // Restoring the literal restores the fingerprint.
    queue_cpp.replace(where, std::string_view{"\"{} order by seq desc\""}.size(), literal);
    CHECK(fingerprint(sources, hq::k_queue_protocol) == pinned);
  }
  SECTION("a protocol value") {
    std::vector<std::string_view> protocol(hq::k_queue_protocol.begin(), hq::k_queue_protocol.end());
    auto const                    at = std::ranges::find(protocol, std::string_view{"seq_floor=1000000"});
    REQUIRE(at != protocol.end());
    *at              = "seq_floor=1";
    auto const moved = fingerprint(sources, protocol);
    CHECK(moved != pinned);
    expect_both_remedies(moved);
  }
  SECTION("a fingerprint-only label") {
    std::vector<std::string_view> protocol(hq::k_queue_protocol.begin(), hq::k_queue_protocol.end());
    auto const at = std::ranges::find_if(protocol, [](std::string_view e) { return e.starts_with("slot_rule="); });
    REQUIRE(at != protocol.end());
    *at = "slot_rule=first_n_live_by_seq";
    CHECK(fingerprint(sources, protocol) != pinned);
  }
}

TEST_CASE("reflowing a SQL literal or changing keyword case leaves the fingerprint unchanged; a column rename moves it",
          "[hostqueue][schema][qp-queue-compat][fingerprint]") {
  std::vector<std::string> const base{
      "auto f() { return conn.prepare(\"SELECT seq, state FROM queue_entries WHERE seq = ? ORDER BY seq\"); }\n"};
  std::vector<std::string> const reflowed{"// a comment between pieces does not split the literal\n"
                                          "auto f() {\n  return conn.prepare(\"select   seq, state \"\n"
                                          "                      /* gap */ \"from queue_entries\n"
                                          "  \"  \"where seq = ?  order by seq\");\n}\n"};
  std::vector<std::string> const renamed{
      "auto f() { return conn.prepare(\"select seq, status from queue_entries where seq = ? order by seq\"); }\n"};
  std::vector<std::string> const quoted_case{
      "auto f() { return conn.prepare(\"select seq from queue_entries where state = 'WAITING'\"); }\n"};
  std::vector<std::string> const quoted_lower{
      "auto f() { return conn.prepare(\"select seq from queue_entries where state = 'waiting'\"); }\n"};
  std::vector<std::string> const digit_separator{"constexpr auto x = 86'400'000; auto s = \"select seq from queue_entries\";\n"};

  auto const protocol = std::span<std::string_view const>(hq::k_queue_protocol);
  CHECK(sql_literals(base) == std::vector<std::string>{"select seq, state from queue_entries where seq = ? order by seq"});
  CHECK(fingerprint(reflowed, protocol) == fingerprint(base, protocol));
  CHECK(fingerprint(renamed, protocol) != fingerprint(base, protocol));
  // A single-quoted value is data, not a keyword: its case is significant.
  CHECK(fingerprint(quoted_case, protocol) != fingerprint(quoted_lower, protocol));
  CHECK(sql_literals(digit_separator) == std::vector<std::string>{"select seq from queue_entries"});
  // A bare column list counts as SQL; a diagnostic without a keyword does not.
  CHECK(is_sql_literal("seq, state, host_id"));
  CHECK_FALSE(is_sql_literal("hostqueue: stored argv is not a JSON array"));
}

TEST_CASE("the fingerprint ledger requires a reason, and an appended same-version entry re-pins",
          "[hostqueue][schema][qp-queue-compat][fingerprint]") {
  auto const        sources = hostqueue_sources();
  auto const        current = fingerprint(sources, hq::k_queue_protocol);
  std::string const other(64, 'a');

  SECTION("the real ledger is well-formed") {
    for (auto const& pin : k_fingerprint_ledger) {
      INFO(pin.version);
      CHECK_FALSE(pin.reason.empty());
    }
    CHECK(k_fingerprint_ledger.back().version == hq::k_queue_schema_version);
  }
  SECTION("an entry with an empty reason fails") {
    std::array const pins{fingerprint_pin{1, other, "initial"}, fingerprint_pin{1, current, ""}};
    auto const       verified = verify_ledger(pins, 1, current);
    REQUIRE_FALSE(verified.has_value());
    CHECK(verified.error().contains("empty reason"));
  }
  SECTION("a stale pin fails, and appending {1, <new fingerprint>, \"refactor: ...\"} passes at the same version") {
    std::array const stale{fingerprint_pin{1, other, "initial"}};
    CHECK_FALSE(verify_ledger(stale, 1, current).has_value());
    std::array const appended{fingerprint_pin{1, other, "initial"},
                              fingerprint_pin{1, current, "refactor: reflowed the enqueue insert"}};
    CHECK(verify_ledger(appended, 1, current).has_value());
  }
  SECTION("only the LAST entry for the version counts") {
    std::array const superseded{fingerprint_pin{1, current, "initial"}, fingerprint_pin{1, other, "refactor: later"}};
    CHECK_FALSE(verify_ledger(superseded, 1, current).has_value());
  }
  SECTION("a bumped version with no entry of its own fails") {
    std::array const pins{fingerprint_pin{1, current, "initial"}};
    CHECK_FALSE(verify_ledger(pins, 2, current).has_value());
  }
  SECTION("an entry going back a version fails") {
    std::array const pins{fingerprint_pin{2, other, "bump"}, fingerprint_pin{1, current, "refactor"}};
    CHECK_FALSE(verify_ledger(pins, 1, current).has_value());
  }
}
