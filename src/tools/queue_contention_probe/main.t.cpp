// main.t.cpp: black-box tests for `queue_contention_probe` (plan 1089, task
// qp-contention-measure; test spec 658 scenarios "the contention probe refuses
// to touch a live database" and the R1/R3/R4/R5 report shapes).
//
// The probe's five measurement runs take minutes on an operator-size copy and
// are recorded on the plan, not gated. What IS gated here is the part that
// protects the operator's database and the shape of what the runs report:
//  - the three refusals the spec names (destination resolves to the source,
//    lies under `$HOME/.planar`, equals `$PLANAR_DB`) plus an existing
//    destination, each proven to exit 2 with the source byte-for-byte
//    unchanged and nothing created;
//  - a real short run of each of R1, R3, R4 and R5 against a scratch source,
//    with the source's hash unchanged afterwards. R1 uses a WRITABLE source
//    (the others a mode-0444 one): a read-write flag on a file the process may
//    not write silently degrades to read-only, so only a writable source can
//    show that the probe's handle on it is read-only by construction.
// Short runs use the probe's own duration and hold flags; the full-length
// defaults are the spec's.
//
// Include-before-import is deliberate (see core/version.t.cpp / db.t.cpp).
#include <catch2/catch_test_macros.hpp>
#include <sys/wait.h> // WIFEXITED/WEXITSTATUS
#include <unistd.h>

import std;
import planar.db;
import planar.db.migrate;
import planar.sha256;

namespace {

namespace fs = std::filesystem;

/// A scratch directory under the temp root named `planar_qcp_*`, which the
/// arena-sweep listener reaps.
auto make_scratch(std::string_view tag) -> fs::path {
  static std::atomic<int> counter{0};
  auto const              root = fs::temp_directory_path() / std::format("planar_qcp_{}_{}_{}", tag, ::getpid(), counter++);
  fs::create_directories(root);
  return root;
}

auto read_file(const fs::path& path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// A source at the head schema, closed, in a directory of its own; mode 0444
/// unless `writable`.
auto make_source(const fs::path& dir, bool writable = false) -> fs::path {
  fs::create_directories(dir);
  auto const path = dir / "source.db";
  {
    auto conn = planar::db::connection::open(path.string());
    REQUIRE(conn.has_value());
    REQUIRE(planar::db::apply_all(*conn).has_value());
    REQUIRE(conn->execute("insert into tasks (scope_kind, title, status) values ('global', 'seed', 'todo')").has_value());
  }
  // The connection's close checkpoints the WAL away; remove stray sidecars so
  // the directory listing below is exactly the source.
  std::error_code ec;
  fs::remove(fs::path(path.string() + "-wal"), ec);
  fs::remove(fs::path(path.string() + "-shm"), ec);
  if (!writable) {
    fs::permissions(path, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read, fs::perm_options::replace);
  }
  return path;
}

struct outcome {
  std::string output; // stdout and stderr together
  int         code = -1;
};

/// Marks a variable to be removed from the probe's environment, not set.
const std::string k_unset = "\x01unset";

/// The test seam standing in for the password database's home directory.
constexpr auto k_passwd_seam = "QUEUE_CONTENTION_PROBE_PASSWD_HOME";

/// Runs the probe with an environment prefix, capturing stdout and stderr.
auto run_probe(const std::map<std::string, std::string>& env, const std::vector<std::string>& args) -> outcome {
  auto const  out_path = fs::temp_directory_path() /
                         std::format("planar_qcp_capture_{}.txt", std::chrono::steady_clock::now().time_since_epoch().count());
  std::string command  = "env";
  for (auto const& [k, v] : env) {
    command += v == k_unset ? std::format(" -u '{}'", k) : std::format(" '{}={}'", k, v);
  }
  command += std::format(" '{}'", PLANAR_QCP_BIN);
  for (auto const& arg : args) {
    command += std::format(" '{}'", arg);
  }
  command += std::format(" > '{}' 2>&1", out_path.string());
  int const status = std::system(command.c_str());
  outcome   result;
  result.output = read_file(out_path);
  result.code   = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  std::error_code ec;
  fs::remove(out_path, ec);
  return result;
}

/// The directory entries' names, sorted: proves nothing was created.
auto listing(const fs::path& dir) -> std::vector<std::string> {
  std::vector<std::string> names;
  for (auto const& entry : fs::directory_iterator(dir)) {
    names.push_back(entry.path().filename().string());
  }
  std::ranges::sort(names);
  return names;
}

/// The integer that follows `"key":` after the first occurrence of `anchor`.
auto number_after(const std::string& text, std::string_view anchor, std::string_view key) -> std::optional<long long> {
  auto const start = text.find(anchor);
  if (start == std::string::npos) {
    return std::nullopt;
  }
  auto const marker = std::format("\"{}\":", key);
  auto const at     = text.find(marker, start);
  if (at == std::string::npos) {
    return std::nullopt;
  }
  long long   value    = 0;
  auto const* first    = text.data() + at + marker.size();
  auto const [ptr, ec] = std::from_chars(first, text.data() + text.size(), value);
  if (ec != std::errc{}) {
    return std::nullopt;
  }
  return value;
}

/// What a refused run must leave behind: the source untouched, nothing new.
void require_refusal(const outcome& result, std::string_view reason, const fs::path& source, const std::string& source_hash,
                     const std::vector<std::string>& listing_before, const fs::path& watched_dir) {
  INFO(result.output);
  CHECK(result.code == 2);
  CHECK(result.output.contains("refused"));
  CHECK(result.output.contains(reason));
  CHECK(planar::sha256::hex(read_file(source)) == source_hash);
  CHECK(listing(watched_dir) == listing_before);
}

} // namespace

TEST_CASE("queue_contention_probe refuses a destination that resolves to its source", "[queue_contention_probe][safety]") {
  auto const root   = make_scratch("same");
  auto const source = make_source(root / "live");
  auto const hash   = planar::sha256::hex(read_file(source));
  fs::create_directory_symlink(root / "live", root / "alias");
  auto const before = listing(root / "live");

  // The destination names the source through a symlinked directory.
  auto const result = run_probe({{"HOME", (root / "home").string()}},
                                {"--source", source.string(), "--dest", (root / "alias" / "source.db").string(), "--run", "r1"});
  require_refusal(result, "resolves to the source", source, hash, before, root / "live");

  // A dot-dot spelling of the same file resolves to it too.
  auto const dotted =
      run_probe({{"HOME", (root / "home").string()}},
                {"--source", source.string(), "--dest", (root / "live" / ".." / "live" / "source.db").string(), "--run", "r1"});
  require_refusal(dotted, "resolves to the source", source, hash, before, root / "live");
}

TEST_CASE("queue_contention_probe refuses a destination under $HOME/.planar", "[queue_contention_probe][safety]") {
  auto const root   = make_scratch("home");
  auto const source = make_source(root / "src");
  auto const hash   = planar::sha256::hex(read_file(source));
  fs::create_directories(root / "home" / ".planar");
  auto const before = listing(root / "home" / ".planar");

  auto const result =
      run_probe({{"HOME", (root / "home").string()}},
                {"--source", source.string(), "--dest", (root / "home" / ".planar" / "copy.db").string(), "--run", "r1"});
  require_refusal(result, ".planar", source, hash, before, root / "home" / ".planar");

  // A not-yet-existing subdirectory of ~/.planar is refused too, and the
  // refusal happens before any directory is created for the copy.
  auto const nested =
      run_probe({{"HOME", (root / "home").string()}},
                {"--source", source.string(), "--dest", (root / "home" / ".planar" / "new" / "copy.db").string(), "--run", "r1"});
  require_refusal(nested, ".planar", source, hash, before, root / "home" / ".planar");
}

TEST_CASE("queue_contention_probe refuses a destination equal to $PLANAR_DB", "[queue_contention_probe][safety]") {
  auto const root   = make_scratch("env");
  auto const source = make_source(root / "src");
  auto const hash   = planar::sha256::hex(read_file(source));
  fs::create_directories(root / "live");
  auto const live   = root / "live" / "planar.db";
  auto const before = listing(root / "live");

  auto const result = run_probe({{"HOME", (root / "home").string()}, {"PLANAR_DB", live.string()}},
                                {"--source", source.string(), "--dest", live.string(), "--run", "r1"});
  require_refusal(result, "$PLANAR_DB", source, hash, before, root / "live");
}

TEST_CASE("queue_contention_probe refuses a destination that already exists and leaves it untouched",
          "[queue_contention_probe][safety]") {
  auto const root   = make_scratch("exists");
  auto const source = make_source(root / "src");
  auto const hash   = planar::sha256::hex(read_file(source));
  fs::create_directories(root / "out");
  {
    std::ofstream(root / "out" / "copy.db") << "precious";
  }
  auto const before = listing(root / "out");

  auto const result = run_probe({{"HOME", (root / "home").string()}},
                                {"--source", source.string(), "--dest", (root / "out" / "copy.db").string(), "--run", "r1"});
  require_refusal(result, "already exists", source, hash, before, root / "out");
  CHECK(read_file(root / "out" / "copy.db") == "precious");
}

TEST_CASE("queue_contention_probe rejects a bad invocation as a usage error", "[queue_contention_probe][safety]") {
  auto const root   = make_scratch("usage");
  auto const source = make_source(root / "src");
  auto const none   = run_probe({}, {"--source", source.string(), "--dest", (root / "o.db").string(), "--run", "r9"});
  CHECK(none.code == 2);
  auto const r5 = run_probe({}, {"--source", source.string(), "--dest", (root / "o.db").string(), "--run", "r5"});
  CHECK(r5.code == 2);
  CHECK(r5.output.contains("--watch-bin"));
  CHECK_FALSE(fs::exists(root / "o.db"));
}

TEST_CASE("queue_contention_probe R1 run on a writable source reports a bounded baseline and leaves the source unchanged",
          "[queue_contention_probe][run]") {
  auto const root   = make_scratch("r1");
  auto const source = make_source(root / "src", true); // writable: only the probe's own checks keep it read-only
  auto const hash   = planar::sha256::hex(read_file(source));
  auto const copy   = root / "out" / "copy.db";

  auto const result =
      run_probe({{"HOME", (root / "home").string()}}, {"--source", source.string(), "--dest", copy.string(), "--run", "r1",
                                                       "--duration-s", "4", "--submitters", "3", "--poll-ms", "200"});
  INFO(result.output);
  REQUIRE(result.code == 0);
  CHECK(result.output.contains(R"("run":"r1")"));
  CHECK(result.output.contains(R"("verdict":"within_bounds")"));
  CHECK(result.output.contains(R"("unchanged":true)"));
  auto const polls = number_after(result.output, R"("queue":)", "polls");
  REQUIRE(polls.has_value());
  CHECK(*polls > 10);
  auto const planning = number_after(result.output, R"("planning":)", "transactions");
  REQUIRE(planning.has_value());
  CHECK(*planning > 10);
  CHECK(number_after(result.output, R"("planning":)", "busy_failures") == 0);
  CHECK(planar::sha256::hex(read_file(source)) == hash);
  CHECK_FALSE(fs::exists(copy)); // the copy is removed unless --keep
}

TEST_CASE("queue_contention_probe R3 run reports polls inside a held transaction as skipped and starts late submitters",
          "[queue_contention_probe][run]") {
  auto const root   = make_scratch("r3");
  auto const source = make_source(root / "src");
  auto const copy   = root / "out" / "copy.db";

  // A 6s hold exceeds the 5s busy timeout, so a poll that begins inside it is skipped.
  auto const result = run_probe({{"HOME", (root / "home").string()}},
                                {"--source", source.string(), "--dest", copy.string(), "--run", "r3", "--duration-s", "13",
                                 "--warmup-s", "1", "--holds", "6", "--gap-s", "2", "--submitters", "2", "--poll-ms", "200"});
  INFO(result.output);
  REQUIRE(result.code == 0);
  CHECK(result.output.contains(R"("verdict":"within_bounds")"));
  auto const inside = number_after(result.output, R"("r3":)", "polls_inside_holds");
  REQUIRE(inside.has_value());
  CHECK(*inside >= 1);
  CHECK(number_after(result.output, R"("r3":)", "inside_other") == 0);
  CHECK(number_after(result.output, R"("r3":)", "inside_skipped") == *inside);
  CHECK(number_after(result.output, R"("queue":)", "skipped").value_or(0) >= 1);
  CHECK(result.output.contains(
      R"("name":"late_submitters_started","bound":"every late submitter's command started","observed":"2 of 2","pass":true)"));
}

TEST_CASE("queue_contention_probe R4 run rebuilds a table inside one write transaction and reports its duration",
          "[queue_contention_probe][run]") {
  auto const root   = make_scratch("r4");
  auto const source = make_source(root / "src");
  auto const copy   = root / "out" / "copy.db";

  auto const missing = run_probe({{"HOME", (root / "home").string()}},
                                 {"--source", source.string(), "--dest", copy.string(), "--run", "r4", "--duration-s", "3"});
  CHECK(missing.code == 2); // a usage error, found before any backup is taken
  CHECK(missing.output.contains("--rebuild-table"));
  CHECK_FALSE(fs::exists(root / "out"));

  auto const result = run_probe({{"HOME", (root / "home").string()}},
                                {"--source", source.string(), "--dest", copy.string(), "--run", "r4", "--duration-s", "5",
                                 "--rebuild-at-s", "1", "--rebuild-table", "tasks", "--submitters", "2", "--poll-ms", "200"});
  INFO(result.output);
  REQUIRE(result.code == 0);
  CHECK(result.output.contains(R"("table":"tasks","started":true,"done":true)"));
  CHECK(result.output.contains(R"("step":"create")"));
  CHECK(result.output.contains(R"("step":"rename")"));
  CHECK(result.output.contains(
      R"j("name":"abandonments","bound":"== 0 (the rebuild took under 25s)","observed":"0","pass":true)j"));
}

TEST_CASE("queue_contention_probe R5 run starts the reader on the copy, stops it, and watches for the checkpoint",
          "[queue_contention_probe][run]") {
  auto const root   = make_scratch("r5");
  auto const source = make_source(root / "src");
  auto const copy   = root / "out" / "copy.db";
  // A stand-in for `planar-watch feed --follow`: records the PLANAR_DB it was
  // given, then waits to be interrupted.
  auto const stub = root / "stub-watch.sh";
  {
    std::ofstream out(stub);
    out << "#!/bin/sh\necho \"$PLANAR_DB\" > '" << (root / "reader-db.txt").string() << "'\nexec sleep 60\n";
  }
  fs::permissions(stub, fs::perms::owner_all, fs::perm_options::replace);

  auto const result =
      run_probe({{"HOME", (root / "home").string()}},
                {"--source", source.string(), "--dest", copy.string(), "--run", "r5", "--duration-s", "3", "--submitters", "2",
                 "--poll-ms", "200", "--watch-bin", stub.string(), "--checkpoint-wait-s", "5", "--keep"});
  INFO(result.output);
  REQUIRE(result.code == 0);
  CHECK(result.output.contains(R"("verdict":"within_bounds")"));
  CHECK(result.output.contains(R"("name":"wal_max_bytes")"));
  CHECK(result.output.contains(R"("name":"checkpoint_completes_within_wait")"));
  CHECK(result.output.contains(
      R"("name":"reader_alive_at_stop","bound":"the feed --follow reader ran for the whole run","observed":"yes","pass":true)"));
  // The reader ran against the copy, never the source.
  auto reader_db = read_file(root / "reader-db.txt");
  while (!reader_db.empty() && (reader_db.back() == '\n' || reader_db.back() == '\r')) {
    reader_db.pop_back();
  }
  CHECK(fs::weakly_canonical(reader_db) == fs::weakly_canonical(copy));
  CHECK(fs::weakly_canonical(reader_db) != fs::weakly_canonical(source));
}

// ---- home resolution, --report vetting and exit statuses (review iteration 1) ----

TEST_CASE(
    "queue_contention_probe refuses a destination under .planar when HOME is unset or empty, using the password-database home",
    "[queue_contention_probe][safety]") {
  auto const root   = make_scratch("nohome");
  auto const source = make_source(root / "src");
  auto const hash   = planar::sha256::hex(read_file(source));
  // The fake home stands in for the password database's home through the seam.
  fs::create_directories(root / "fakehome" / ".planar");
  fs::create_directory_symlink(root / "fakehome" / ".planar", root / "alias");
  auto const before = listing(root / "fakehome" / ".planar");

  for (auto const& home : {k_unset, std::string{}}) {
    INFO("HOME is " << (home == k_unset ? "unset" : "empty"));
    auto const direct =
        run_probe({{"HOME", home}, {k_passwd_seam, (root / "fakehome").string()}},
                  {"--source", source.string(), "--dest", (root / "fakehome" / ".planar" / "nohome.db").string(), "--run", "r1"});
    require_refusal(direct, ".planar", source, hash, before, root / "fakehome" / ".planar");
    // Reached through a symlink into the fake .planar.
    auto const linked =
        run_probe({{"HOME", home}, {k_passwd_seam, (root / "fakehome").string()}},
                  {"--source", source.string(), "--dest", (root / "alias" / "nohome.db").string(), "--run", "r1"});
    require_refusal(linked, ".planar", source, hash, before, root / "fakehome" / ".planar");
  }
}

TEST_CASE("queue_contention_probe refuses to run when no home directory can be determined", "[queue_contention_probe][safety]") {
  auto const root   = make_scratch("nohomeatall");
  auto const source = make_source(root / "src");
  auto const hash   = planar::sha256::hex(read_file(source));
  auto const before = listing(root);

  // HOME is unset and the password-database lookup yields nothing (the seam
  // set to empty): the .planar refusal cannot be evaluated, so the run is refused.
  auto const result = run_probe({{"HOME", k_unset}, {k_passwd_seam, ""}},
                                {"--source", source.string(), "--dest", (root / "out" / "c.db").string(), "--run", "r1"});
  require_refusal(result, "home directory", source, hash, before, root);
  CHECK_FALSE(fs::exists(root / "out"));
}

TEST_CASE("queue_contention_probe vets --report before the backup and never truncates what it refuses",
          "[queue_contention_probe][safety]") {
  auto const root   = make_scratch("report");
  auto const source = make_source(root / "src");
  auto const hash   = planar::sha256::hex(read_file(source));
  auto const home   = root / "home";
  fs::create_directories(home / ".planar");
  fs::create_directories(root / "elsewhere");
  auto const fake_live = home / ".planar" / "planar.db";
  auto const other_db  = root / "elsewhere" / "other.db";
  {
    std::ofstream(fake_live) << "precious live";
    std::ofstream(other_db) << "precious other";
    std::ofstream(root / "elsewhere" / "notes.txt") << "precious notes";
  }
  auto const                               dest = root / "out" / "copy.db";
  std::map<std::string, std::string> const env{
      {"HOME", home.string()}, {"PLANAR_DB", other_db.string()}, {k_passwd_seam, home.string()}};
  auto const attempt = [&](const fs::path& report) {
    return run_probe(env, {"--source", source.string(), "--dest", dest.string(), "--run", "r1", "--duration-s", "2", "--report",
                           report.string()});
  };
  auto const untouched = [&](const outcome& result, std::string_view reason) {
    INFO(result.output);
    CHECK(result.code == 2);
    CHECK(result.output.contains("refused"));
    CHECK(result.output.contains(reason));
    CHECK_FALSE(fs::exists(root / "out")); // vetted before the backup began
    CHECK(planar::sha256::hex(read_file(source)) == hash);
  };

  SECTION("under the home's .planar") {
    untouched(attempt(fake_live), ".planar");
    CHECK(read_file(fake_live) == "precious live");
    untouched(attempt(home / ".planar" / "new-report.json"), ".planar");
    CHECK_FALSE(fs::exists(home / ".planar" / "new-report.json"));
  }
  SECTION("resolving to the source") {
    untouched(attempt(source), "source");
    CHECK(planar::sha256::hex(read_file(source)) == hash);
  }
  SECTION("equal to $PLANAR_DB") {
    untouched(attempt(other_db), "PLANAR_DB");
    CHECK(read_file(other_db) == "precious other");
  }
  SECTION("an existing file that is not a prior report") {
    untouched(attempt(root / "elsewhere" / "notes.txt"), "not a report");
    CHECK(read_file(root / "elsewhere" / "notes.txt") == "precious notes");
  }
  SECTION("the destination copy itself") {
    untouched(attempt(dest), "destination copy");
  }
  SECTION("an existing path that is not a regular file") {
    untouched(attempt(root / "elsewhere"), "regular file");
  }
}

TEST_CASE("queue_contention_probe overwrites only a prior report named by --report", "[queue_contention_probe][run]") {
  auto const root   = make_scratch("rereport");
  auto const source = make_source(root / "src");
  auto const report = root / "report.json";
  {
    std::ofstream(report) << R"({"run":"r1","verdict":"within_bounds"})" << "\n";
  }
  auto const result = run_probe({{"HOME", (root / "home").string()}, {k_passwd_seam, (root / "home").string()}},
                                {"--source", source.string(), "--dest", (root / "out" / "c.db").string(), "--run", "r1",
                                 "--duration-s", "2", "--submitters", "2", "--poll-ms", "200", "--report", report.string()});
  INFO(result.output);
  REQUIRE(result.code == 0);
  CHECK(read_file(report).contains(R"("copy":{"path")"));
}

TEST_CASE("queue_contention_probe exits non-zero when its workers fail, even for an informational run",
          "[queue_contention_probe][run]") {
  auto const root   = make_scratch("fatal");
  auto const source = make_source(root / "src", true);
  {
    // A trigger that refuses every insert makes each submitter's enqueue fail
    // with a non-busy error: a fatal worker.
    auto conn = planar::db::connection::open(source.string());
    REQUIRE(conn.has_value());
    REQUIRE(
        conn->execute("create trigger refuse_enqueue before insert on queue_entries begin select raise(abort, 'refused'); end")
            .has_value());
  }
  auto const env =
      std::map<std::string, std::string>{{"HOME", (root / "home").string()}, {k_passwd_seam, (root / "home").string()}};

  auto const informational = run_probe(env, {"--source", source.string(), "--dest", (root / "o2.db").string(), "--run", "r2",
                                             "--duration-s", "2", "--submitters", "2", "--poll-ms", "200"});
  INFO(informational.output);
  CHECK(informational.output.contains(R"("verdict":"error")"));
  CHECK(informational.code == 1);

  auto const bounded = run_probe(env, {"--source", source.string(), "--dest", (root / "o1.db").string(), "--run", "r1",
                                       "--duration-s", "2", "--submitters", "2", "--poll-ms", "200"});
  INFO(bounded.output);
  CHECK(bounded.output.contains(R"("verdict":"breached")"));
  CHECK(bounded.code == 3);
}
