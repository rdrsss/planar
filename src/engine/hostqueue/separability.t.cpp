// separability.t.cpp: the host queue stays a separable unit inside planar.db
// (plan 1089, task qp-separability; decision 1219; tech spec 656 §
// Separability; test spec 658 scenario "Edge -- queue SQL and planning SQL
// stay disjoint").
//
// The rules, each enforced over SQL STRING LITERALS only, never identifiers:
//  - hostqueue SQL names no planning table (it may name `queue_entries`,
//    `queue_history`, `queue_schema`, `sqlite_sequence`, and introspect with
//    `sqlite_master` and `pragma_table_info`, which the schema module does);
//  - no SQL literal outside the hostqueue directory, the migrations and the
//    tests names a queue table;
//  - the queue tables have no foreign key to or from any other table, and no
//    trigger or view mentions them.
// A literal counts as SQL when it contains `select`, `insert`, `update`,
// `delete`, `create`, `alter` or `drop` as a whole word. That is why the
// planar-watch handler function `queue_history` (an identifier, never a
// literal) cannot trip the guard.
//
// "No transaction writes both queue tables and planning tables" is the
// consequence of the first two rules: a transaction that did so would need
// SQL naming both, and no literal may. A literal-only scan cannot see two
// separate statements issued inside one transaction by a caller that imports
// both APIs, so that clause is held by the rules above plus review, not by a
// further test.
//
// The scanners are pure functions over source text. The self-tests plant a
// violation in a string and require it to be found; the tree tests then run
// the same functions over the real sources, with controls proving each scan
// saw what it claims to. The tests read the source tree from
// PLANAR_SOURCE_ROOT, exactly as schema.t.cpp reads PLANAR_HOSTQUEUE_SOURCE_DIR.
//
// Include-before-import is deliberate (see core/version.t.cpp / db.t.cpp).
#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;

#include "scratch_store.hpp"
#include "sql_scan.hpp"

namespace {

using hostqueue_scan::string_literals;

// The three queue tables, and the names the hostqueue directory's own SQL may
// use besides planning tables: the sequence counter and SQLite's own
// introspection, which the schema module uses to check its tables.
const std::set<std::string, std::less<>> k_queue_tables{"queue_entries", "queue_history", "queue_schema"};

const std::regex k_sql_verb(R"(\b(select|insert|update|delete|create|alter|drop)\b)", std::regex::icase);
const std::regex k_word(R"([A-Za-z_][A-Za-z0-9_]*)");

auto lower(std::string text) -> std::string {
  std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

// Whether a literal is SQL under the separability rule.
auto is_sql(const std::string& literal) -> bool {
  return std::regex_search(literal, k_sql_verb);
}

// The whole words of `literal`, lowercased, in order.
auto words_of(const std::string& literal) -> std::vector<std::string> {
  std::vector<std::string> words;
  for (auto it = std::sregex_iterator(literal.begin(), literal.end(), k_word); it != std::sregex_iterator(); ++it) {
    words.push_back(lower(it->str()));
  }
  return words;
}

auto excerpt(const std::string& literal) -> std::string {
  auto const flat = literal.substr(0, 240);
  return flat.size() < literal.size() ? flat + "..." : flat;
}

// Rule 1. Every SQL literal in `source` (a hostqueue file) that names a word
// in `planning`. `planning` is the set of non-queue tables and views of the
// head schema, read from a real migrated database, so a join against any of
// them is caught whatever its syntax: `from a, b`, a subquery, a `join`.
auto hostqueue_violations(std::string_view source, const std::set<std::string, std::less<>>& planning)
    -> std::vector<std::string> {
  std::vector<std::string> out;
  for (auto const& literal : string_literals(source)) {
    if (!is_sql(literal)) {
      continue;
    }
    for (auto const& word : words_of(literal)) {
      if (planning.contains(word)) {
        out.push_back(std::format("names planning table '{}': {}", word, excerpt(literal)));
      }
    }
  }
  return out;
}

// Rule 2. Every SQL literal in `source` (any file outside hostqueue, the
// migrations and the tests) that names a queue table.
auto queue_name_violations(std::string_view source) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (auto const& literal : string_literals(source)) {
    if (!is_sql(literal)) {
      continue;
    }
    for (auto const& word : words_of(literal)) {
      if (k_queue_tables.contains(word)) {
        out.push_back(std::format("names queue table '{}': {}", word, excerpt(literal)));
      }
    }
  }
  return out;
}

auto read_text(const std::filesystem::path& path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

auto source_root() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_SOURCE_ROOT};
}

auto is_test_file(const std::filesystem::path& path) -> bool {
  return path.filename().string().ends_with(".t.cpp");
}

auto is_source_file(const std::filesystem::path& path) -> bool {
  auto const name = path.filename().string();
  return name.ends_with(".cpp") || name.ends_with(".cppm") || name.ends_with(".hpp");
}

// The file names (last path component) a source quotes in `#include "..."`.
auto quoted_includes(std::string_view text) -> std::set<std::string, std::less<>> {
  static const std::regex            include_line(R"re(^[ \t]*#[ \t]*include[ \t]+"([^"]+)")re", std::regex::multiline);
  std::string const                  owned(text);
  std::set<std::string, std::less<>> out;
  for (auto it = std::sregex_iterator(owned.begin(), owned.end(), include_line); it != std::sregex_iterator(); ++it) {
    out.insert(std::filesystem::path{(*it)[1].str()}.filename().string());
  }
  return out;
}

// Every first-party source under `root` that is production code, by rule and
// not by name. A header is test-only, and so exempt from the separability
// rules, exactly when it has at least one includer and every includer is a
// `*.t.cpp` or itself a test-only header (a fixpoint, so a helper header
// included only by a test header qualifies). A header nobody includes is
// production, so is one any non-test file includes, and so is every `.cpp`
// and `.cppm` that is not a `*.t.cpp`. A new test-support header therefore
// needs no entry anywhere.
auto production_sources(const std::filesystem::path& root) -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path>                                  files;
  std::map<std::filesystem::path, std::set<std::string, std::less<>>> includes;
  for (auto const& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (entry.is_regular_file() && is_source_file(entry.path())) {
      files.push_back(entry.path());
      includes[entry.path()] = quoted_includes(read_text(entry.path()));
    }
  }
  std::ranges::sort(files);

  std::set<std::filesystem::path> test_only;
  for (bool grew = true; grew;) {
    grew = false;
    for (auto const& header : files) {
      if (header.extension() != ".hpp" || test_only.contains(header)) {
        continue;
      }
      auto const name      = header.filename().string();
      bool       any       = false;
      bool       all_tests = true;
      for (auto const& file : files) {
        if (file == header || !includes[file].contains(name)) {
          continue;
        }
        any       = true;
        all_tests = all_tests && (is_test_file(file) || test_only.contains(file));
      }
      if (any && all_tests) {
        test_only.insert(header);
        grew = true;
      }
    }
  }

  std::vector<std::filesystem::path> out;
  for (auto const& file : files) {
    if (!is_test_file(file) && !test_only.contains(file)) {
      out.push_back(file);
    }
  }
  return out;
}

auto in_hostqueue(const std::filesystem::path& path) -> bool {
  return path.parent_path().lexically_normal() == (source_root() / "engine" / "hostqueue").lexically_normal();
}

// Production sources of the hostqueue directory.
auto hostqueue_sources() -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> out;
  for (auto const& file : production_sources(source_root())) {
    if (in_hostqueue(file)) {
      out.push_back(file);
    }
  }
  return out;
}

// Every production source under src/ outside the hostqueue directory.
auto other_sources() -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> out;
  for (auto const& file : production_sources(source_root())) {
    if (!in_hostqueue(file)) {
      out.push_back(file);
    }
  }
  return out;
}

constexpr std::string_view k_remedy_outside =
    "remedy: call the planar.engine.hostqueue API instead; queue SQL belongs in src/engine/hostqueue (tech spec 656 § "
    "Separability)";
constexpr std::string_view k_remedy_inside =
    "remedy: move the planning query out of hostqueue; hostqueue SQL may name only the queue tables, sqlite_sequence and "
    "SQLite introspection (tech spec 656 § Separability)";

// One message for every violation `scan` finds in `files`: file (relative to
// the source root), the table named and the whole SQL literal, then the
// remedy. Empty when there is nothing to report. Built before the CHECK so
// the offending SQL is still in hand when the check fails.
template <class Scan>
auto violation_report(const std::vector<std::filesystem::path>& files, const std::filesystem::path& root, Scan&& scan,
                      std::string_view remedy) -> std::string {
  std::string out;
  for (auto const& file : files) {
    for (auto const& violation : scan(read_text(file))) {
      out += std::format("  {}: {}\n", std::filesystem::relative(file, root).string(), violation);
    }
  }
  if (!out.empty()) {
    out = "separability violation(s):\n" + out + "  " + std::string{remedy} + "\n";
  }
  return out;
}

// A scratch planar.db at the head of the embedded main chain, and the names of
// every non-queue table and view in it.
struct head_database {
  std::filesystem::path  path;
  planar::db::connection conn;

  static auto make() -> head_database {
    auto const dir = std::filesystem::temp_directory_path() /
                     std::format("planar_hq_sep_{}", std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(dir);
    auto opened = open_main_store_at(dir / "planar.db");
    REQUIRE(opened.has_value());
    return head_database{dir, std::move(*opened)};
  }
  head_database(std::filesystem::path p, planar::db::connection c) : path(std::move(p)), conn(std::move(c)) {
  }
  head_database(head_database&&) noexcept            = default;
  head_database& operator=(head_database&&) noexcept = default;
  head_database(const head_database&)                = delete;
  head_database& operator=(const head_database&)     = delete;
  ~head_database() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

auto column_of(planar::db::connection& conn, std::string_view sql) -> std::vector<std::string> {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  std::vector<std::string> out;
  while (stmt->step().value() == planar::db::step_result::row) {
    out.push_back(stmt->column_text(0));
  }
  return out;
}

auto planning_names(planar::db::connection& conn) -> std::set<std::string, std::less<>> {
  std::set<std::string, std::less<>> out;
  for (auto const& name : column_of(conn, "select name from sqlite_master where type in ('table', 'view') "
                                          "and name not like 'sqlite\\_%' escape '\\'")) {
    if (!k_queue_tables.contains(name)) {
      out.insert(name);
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// The scanners find what they claim to, and nothing else.
// ---------------------------------------------------------------------------

TEST_CASE("the separability scanners find a planted violation and ignore identifiers",
          "[hostqueue][separability][qp-separability]") {
  auto       db       = head_database::make();
  auto const planning = planning_names(db.conn);
  // Not vacuous: the names the rule must cover are in the set, and the queue's
  // own are not.
  REQUIRE(planning.contains("tasks"));
  REQUIRE(planning.contains("plans"));
  REQUIRE(planning.contains("schema_migrations"));
  REQUIRE_FALSE(planning.contains("queue_entries"));
  REQUIRE_FALSE(planning.contains("sqlite_sequence"));
  REQUIRE_FALSE(planning.contains("sqlite_master"));

  SECTION("rule 1: a hostqueue query that joins a planning table is found") {
    CHECK(hostqueue_violations(R"sql(auto q = "select e.seq from queue_entries e join tasks t on t.id = e.seq";)sql", planning)
              .size() == 1);
    // A comma join and a subquery are found too, whatever the syntax.
    CHECK(hostqueue_violations(R"sql(auto q = "select seq from queue_entries, plans";)sql", planning).size() == 1);
    CHECK(hostqueue_violations(R"sql(auto q = "delete from queue_entries where seq in (select id from tasks)";)sql", planning)
              .size() == 1);
    // A literal split across lines is one literal.
    CHECK(hostqueue_violations("auto q = \"select seq from queue_entries \"\n \"join plans on 1\";", planning).size() == 1);
    // A hostqueue file that names `schema_migrations` is reaching past the queue.
    CHECK(hostqueue_violations(R"sql(auto q = "select max(version) from schema_migrations";)sql", planning).size() == 1);
  }
  SECTION("rule 1: the queue's own tables, the counter and introspection are allowed") {
    CHECK(hostqueue_violations(R"sql(auto q = "select seq, state from queue_entries where seq = ?";)sql", planning).empty());
    CHECK(hostqueue_violations(R"sql(auto q = "select max(version) from queue_schema";)sql", planning).empty());
    CHECK(hostqueue_violations(R"sql(auto q = "select seq from sqlite_sequence where name = 'queue_entries'";)sql", planning)
              .empty());
    // The schema module's introspection of its own tables.
    CHECK(hostqueue_violations(R"sql(auto q = "select count(*) from sqlite_master where type = 'table' and name = ?";)sql",
                               planning)
              .empty());
    CHECK(hostqueue_violations(R"sql(auto q = "select name from pragma_table_info('queue_entries')";)sql", planning).empty());
  }
  SECTION("a planning word in a literal that is not SQL, in a comment or in an identifier is not a violation") {
    CHECK(hostqueue_violations(R"sql(auto m = "the tasks and plans are not touched";)sql", planning).empty());
    CHECK(hostqueue_violations("// select * from tasks\nauto x = 1;", planning).empty());
    CHECK(hostqueue_violations("auto tasks = 3; auto plans = tasks;", planning).empty());
  }
  SECTION("rule 2: a literal outside hostqueue that names a queue table is found") {
    CHECK(queue_name_violations(R"sql(auto q = "select count(*) from queue_entries";)sql").size() == 1);
    CHECK(queue_name_violations(R"sql(auto q = "update queue_schema set compat = 9";)sql").size() == 1);
    CHECK(queue_name_violations(R"sql(auto q = "delete from queue_history";)sql").size() == 1);
    CHECK(queue_name_violations(R"sql(auto q = "insert into queue_entries (state) values ('waiting')";)sql").size() == 1);
    CHECK(queue_name_violations(R"sql(auto q = "SELECT * FROM Queue_Entries";)sql").size() == 1);
  }
  SECTION("rule 2: the planar-watch handler named queue_history is an identifier, not SQL") {
    // The exact shapes at dispatch.cpp:88 and history.cpp:138.
    CHECK(queue_name_violations(R"sql(table.emplace("queue history", handlers::queue_history);)sql").empty());
    CHECK(queue_name_violations("auto queue_history(context& ctx, const cliapp::parsed_args& args) -> handler_result {\n"
                                "  return {};\n}")
              .empty());
    // A literal that names the table without any SQL verb is not SQL either.
    CHECK(queue_name_violations(R"sql(auto k = "queue_history";)sql").empty());
    // ...and the same words inside a comment.
    CHECK(queue_name_violations("// select * from queue_history\nauto x = 1;").empty());
  }
}

// ---------------------------------------------------------------------------
// Which files are production code: by rule, not by name.
// ---------------------------------------------------------------------------

void write_file(const std::filesystem::path& path, std::string_view text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << text;
}

TEST_CASE("a header is exempt from the separability rules only when every includer is a test, and a production header "
          "that names a queue table is caught",
          "[hostqueue][separability][qp-separability]") {
  auto const root = std::filesystem::temp_directory_path() /
                    std::format("planar_hq_sep_tree_{}", std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::create_directories(root);
  // A production module that includes a header whose SQL names a queue table.
  write_file(root / "prod" / "handler.cpp", "#include \"prod_helper.hpp\"\n#include \"mixed.hpp\"\nint handler();\n");
  write_file(root / "prod" / "prod_helper.hpp", "inline const char* k = \"select * from queue_history where seq = 1\";\n");
  // A header both a test and a production file include is production.
  write_file(root / "prod" / "mixed.hpp", "inline const char* m = \"delete from queue_entries\";\n");
  // A header nothing includes is production.
  write_file(root / "prod" / "orphan.hpp", "inline const char* o = \"update queue_schema set compat = 2\";\n");
  // A test-only header, and a helper only that header includes (the fixpoint).
  write_file(root / "tests" / "x.t.cpp", "#include \"test_store.hpp\"\n#include \"../prod/mixed.hpp\"\n");
  write_file(root / "tests" / "test_store.hpp",
             "#include \"chain.hpp\"\ninline const char* t = \"insert into queue_entries (state) values ('waiting')\";\n");
  write_file(root / "tests" / "chain.hpp", "inline const char* c = \"select count(*) from queue_history\";\n");

  auto const            production = production_sources(root);
  std::set<std::string> names;
  for (auto const& file : production) {
    names.insert(file.filename().string());
  }
  CHECK(names == std::set<std::string>{"handler.cpp", "prod_helper.hpp", "mixed.hpp", "orphan.hpp"});

  // The control the all-headers-exempt shortcut cannot survive: the production
  // headers' queue SQL is reported, with file, table, literal and remedy, and
  // the test-only headers' is not.
  auto const report = violation_report(production, root, queue_name_violations, k_remedy_outside);
  INFO(report);
  CHECK(report.contains("prod/prod_helper.hpp: names queue table 'queue_history': select * from queue_history where seq = 1"));
  CHECK(report.contains("prod/mixed.hpp: names queue table 'queue_entries': delete from queue_entries"));
  CHECK(report.contains("prod/orphan.hpp: names queue table 'queue_schema': update queue_schema set compat = 2"));
  CHECK_FALSE(report.contains("test_store.hpp"));
  CHECK_FALSE(report.contains("chain.hpp"));
  CHECK(report.contains("call the planar.engine.hostqueue API instead"));
  CHECK(report.contains("tech spec 656"));

  // The inside-hostqueue remedy names the other direction.
  auto const planning = std::set<std::string, std::less<>>{"tasks"};
  write_file(root / "hq" / "bad.cpp", "auto q = \"select seq from queue_entries join tasks on 1\";\n");
  auto const inside = violation_report(
      std::vector<std::filesystem::path>{root / "hq" / "bad.cpp"}, root,
      [&](std::string_view text) { return hostqueue_violations(text, planning); }, k_remedy_inside);
  CHECK(inside.contains("hq/bad.cpp: names planning table 'tasks': select seq from queue_entries join tasks on 1"));
  CHECK(inside.contains("move the planning query out of hostqueue"));

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

TEST_CASE("the real tree classifies the in-tree test harness headers as test-only and a production header as production",
          "[hostqueue][separability][qp-separability]") {
  auto const            production = production_sources(source_root());
  std::set<std::string> names;
  for (auto const& file : production) {
    names.insert(file.filename().string());
  }
  CHECK_FALSE(names.contains("parity_harness.hpp"));
  CHECK_FALSE(names.contains("catalog_parity.hpp"));
  CHECK_FALSE(names.contains("scratch_store.hpp"));
  CHECK_FALSE(names.contains("sql_scan.hpp"));
  // Not vacuous: headers are not all dropped.
  CHECK(std::ranges::any_of(production, [](const auto& f) { return f.extension() == ".hpp"; }));
}

// ---------------------------------------------------------------------------
// The real sources.
// ---------------------------------------------------------------------------

TEST_CASE("every SQL literal under src/engine/hostqueue names only queue tables, the counter and introspection",
          "[hostqueue][separability][qp-separability]") {
  auto       db       = head_database::make();
  auto const planning = planning_names(db.conn);
  auto const files    = hostqueue_sources();

  // Not vacuous: the engine's own files are here, and between them they carry
  // plenty of SQL, including the schema module's introspection (the allowance
  // is exercised by real code, not only by the self-test).
  REQUIRE(files.size() >= 10);
  std::size_t sql_literals = 0;
  bool        saw_master   = false;
  bool        saw_pragma   = false;
  for (auto const& file : files) {
    auto const text = read_text(file);
    for (auto const& literal : string_literals(text)) {
      if (is_sql(literal)) {
        ++sql_literals;
        saw_master = saw_master || lower(literal).contains("sqlite_master");
        saw_pragma = saw_pragma || lower(literal).contains("pragma_table_info");
      }
    }
  }
  auto const report = violation_report(
      files, source_root(), [&](std::string_view text) { return hostqueue_violations(text, planning); }, k_remedy_inside);
  INFO(report);
  CHECK(report.empty());
  CHECK(sql_literals >= 30);
  CHECK(saw_master);
  CHECK(saw_pragma);
}

TEST_CASE("no SQL literal outside hostqueue, the migrations and the tests names a queue table",
          "[hostqueue][separability][qp-separability]") {
  auto const files = other_sources();
  // Not vacuous: the whole tree is here, the planar-watch handler whose
  // function is named `queue_history` and a planning module included.
  REQUIRE(files.size() >= 300);
  bool saw_watch_history = false;
  for (auto const& file : files) {
    auto const text = read_text(file);
    if (file.filename() == "history.cpp" && file.parent_path().filename() == "queue" &&
        text.contains("auto queue_history(context& ctx")) {
      saw_watch_history = true;
    }
  }
  auto const report = violation_report(files, source_root(), queue_name_violations, k_remedy_outside);
  INFO(report);
  CHECK(report.empty());
  CHECK(saw_watch_history);

  // Control: the same scan, pointed at the hostqueue directory it exempts,
  // does see queue tables. A scan that found nothing there would prove nothing
  // about the files above.
  std::size_t hits = 0;
  for (auto const& file : hostqueue_sources()) {
    hits += queue_name_violations(read_text(file)).size();
  }
  CHECK(hits >= 10);
}

// ---------------------------------------------------------------------------
// The schema: no foreign key, trigger or view ties the queue to the rest.
// ---------------------------------------------------------------------------

TEST_CASE("the queue tables have no foreign key to or from any other table, and no trigger or view names them",
          "[hostqueue][separability][qp-separability]") {
  auto  db   = head_database::make();
  auto& conn = db.conn;

  for (auto const& table : k_queue_tables) {
    INFO("table: " << table);
    CHECK(column_of(conn, std::format("select \"table\" from pragma_foreign_key_list('{}')", table)).empty());
  }
  // Nothing else points at a queue table either.
  auto const everything =
      column_of(conn, "select name from sqlite_master where type = 'table' and name not like 'sqlite\\_%' escape '\\'");
  REQUIRE(everything.size() > 30);
  for (auto const& table : everything) {
    for (auto const& target : column_of(conn, std::format("select \"table\" from pragma_foreign_key_list('{}')", table))) {
      INFO(table << " references " << target);
      CHECK_FALSE(k_queue_tables.contains(target));
    }
  }
  // No trigger or view mentions a queue table (an index on one is fine).
  for (auto const& sql : column_of(conn, "select sql from sqlite_master where type in ('trigger', 'view')")) {
    for (auto const& word : words_of(sql)) {
      INFO(sql);
      CHECK_FALSE(k_queue_tables.contains(word));
    }
  }
}

} // namespace
