// @file capability.t.cpp
// @brief LEVEL 1 of `planar-watch`'s read-only guarantee: the verb set
// (plan 996, task 6107).
//
// Port target: zig/integration_tests/capability_boundary_test.zig's
// planar-watch half — "a watcher configured with only planar-watch on PATH
// cannot touch anything at all."
//
// Level 2 (the SQLITE_OPEN_READONLY handle) is `context.t.cpp`. Neither
// subsumes the other and both are required: the verb set means no code path
// composes a mutating statement, the handle means even a mistake in the
// verb set cannot write. The binary's own `--help` page advertises exactly
// this pairing to operators.
//
// As with planar-agent, the FORBIDDEN half is fully ported and is the
// security contract; the EXACT-SET half is a completeness check asserted
// against the ported subset, with the oracle's full twelve recorded rather
// than falsely claimed. And as there, the walk is over `cliapp::all_nodes` —
// the WHOLE tree at any depth — so a write verb smuggled in as a
// subcommand is caught, which the Zig version's root-`--help` parse
// would miss.
//
// ## Break-probes run against this file
//
//   - Added `.name = "claim"` (a planar-agent write verb) to the root ->
//     `refuses every write verb` FAILS. Restored -> green.
//   - Added `.name = "task"` as a child of `completion` -> same test
//     FAILS. Restored -> green.
//   - Registered a handler under the misspelled key "verison" -> `no
//     planar-watch handler is unreachable from argv` FAILS. Restored ->
//     green.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.walk;
import planar.db;
import planar.db.migrate;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.dispatch;
import planar.cmd.planar_watch.exit;
import planar.cmd.planar_watch.handler;
import planar.cmd.planar_watch.surface;
import planar.cmd.planar_watch.main;

namespace {

/// @brief Every node name in the tree except the root's own, at any depth.
/// @param root The tree to walk.
/// @return The set of names.
auto all_node_names(const CLI::App& root) -> std::set<std::string, std::less<>> {
  std::set<std::string, std::less<>> names;
  for (auto const& node : planar::cliapp::all_nodes(root)) {
    if (node.path.empty()) {
      continue;
    }
    names.insert(node.node->get_name());
  }
  return names;
}

// ---- the queue signaller boundary (task 7096) ------------------------------
//
// `planar-watch` links `engine_hostqueue`, whose `terminate` module can signal
// a process group. No watch verb may reach it. The verb-set and read-only
// handle checks above cannot see this: a handler that called
// `hostqueue::begin_terminate` would register no write verb and touch no
// SQLite write path. Two independent checks hold it, one at the source and one
// at the link, and each has a positive control so it cannot pass vacuously.

/// @brief The names that mean "send a signal to a process group", or reach the
/// module that does. `kill(` covers a direct call; the rest are the exports of
/// `planar.engine.hostqueue.terminate` and the identity primitive under them.
const std::vector<std::string_view> k_signaller_names{"hostqueue.terminate",
                                                      "signal_child_group",
                                                      "system_group_signaller",
                                                      "group_signaller",
                                                      "begin_terminate",
                                                      "advance_terminations",
                                                      "poll_and_stop",
                                                      "cancel_waiting",
                                                      "signal_group",
                                                      "killpg",
                                                      "kill("};

/// @brief The names in `text` that `k_signaller_names` forbids, ignoring `//`
/// and `///` comments and block-comment continuation lines (prose about the
/// boundary is not a call).
auto signaller_hits(std::string_view text) -> std::vector<std::string> {
  std::vector<std::string> hits;
  std::size_t              line_no = 0;
  while (!text.empty()) {
    auto const end  = text.find('\n');
    auto       line = text.substr(0, end);
    text            = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    ++line_no;
    if (auto const comment = line.find("//"); comment != std::string_view::npos) {
      line = line.substr(0, comment);
    }
    if (auto const first = line.find_first_not_of(" \t"); first != std::string_view::npos && line[first] == '*') {
      continue;
    }
    for (auto const name : k_signaller_names) {
      if (line.contains(name)) {
        hits.push_back(std::format("line {}: {}", line_no, name));
      }
    }
  }
  return hits;
}

/// @brief Spellings that reach the retired agent database: its module, the
/// viewer's old store wrapper, its environment variable and its file name.
constexpr std::array<std::string_view, 5> k_agent_database_names{"agentdb", "agentstore", "agent_store", "PLANAR_AGENT_DB",
                                                                 "agent.db"};

/// @brief Whether `text` names the agent database outside a `//` comment or a
/// block-comment continuation line (prose about the retirement is not a use).
auto names_agent_database(std::string_view text) -> bool {
  while (!text.empty()) {
    auto const end  = text.find('\n');
    auto       line = text.substr(0, end);
    text            = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    if (auto const comment = line.find("//"); comment != std::string_view::npos) {
      line = line.substr(0, comment);
    }
    if (auto const first = line.find_first_not_of(" \t"); first != std::string_view::npos && line[first] == '*') {
      continue;
    }
    if (std::ranges::any_of(k_agent_database_names, [&](std::string_view name) { return line.contains(name); })) {
      return true;
    }
  }
  return false;
}

auto read_file(const std::filesystem::path& path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// @brief `src/cmd/planar-watch`, found from this file's own path.
auto watch_source_dir() -> std::filesystem::path {
  return std::filesystem::path{__FILE__}.parent_path();
}

/// @brief Every first-party, non-test source file under `dir`.
auto production_sources(const std::filesystem::path& dir) -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> files;
  REQUIRE(std::filesystem::is_directory(dir));
  for (auto const& entry : std::filesystem::recursive_directory_iterator(dir)) {
    auto const ext  = entry.path().extension().string();
    auto const name = entry.path().filename().string();
    if (entry.is_regular_file() && (ext == ".cpp" || ext == ".cppm") && !name.ends_with(".t.cpp")) {
      files.push_back(entry.path());
    }
  }
  std::ranges::sort(files);
  return files;
}

} // namespace

TEST_CASE("planar-watch refuses every write verb from both other binaries, at any depth", "[cmd][watch][capability]") {
  auto const root  = planar::cmd::watch::root_app();
  auto const names = all_node_names(*root);

  // Twelve planar-agent write verbs + seventeen planar planning-entity
  // verbs, transcribed from the Zig suite's two assertContainsNone lists.
  REQUIRE(planar::cmd::watch::forbidden_verbs().size() == 29);
  for (auto const& forbidden : planar::cmd::watch::forbidden_verbs()) {
    INFO("forbidden verb leaked into planar-watch's tree: " << forbidden);
    CHECK_FALSE(names.contains(forbidden));
  }
}

TEST_CASE("planar-watch's declared verb set is exactly the oracle's", "[cmd][watch][capability]") {
  auto const root  = planar::cmd::watch::root_app();
  auto const names = all_node_names(*root);

  // The oracle registers twelve top-level verbs: feed, ps, claims,
  // actions, plans, log, tree, run, sync-events, version, completion,
  // schema. Task 6120 landed the six read verbs that rest on
  // `engine.runtime.agentactivity`; task 6065 DECLARED three more
  // (`sync-events`, `run list`, `run show`) so that
  // `zig/tools/cli_usage_lint` can resolve them, each refusing at exit 64
  // — proved by name in parity.t.cpp. Task 6039 landed `feed` for real —
  // see handlers/feed.cppm. Declaring without landing is not implementing,
  // and this binary keeps the difference loud for what remains unported.
  //
  // THE EXACT-SET FORM IS LOAD-BEARING, not a stylistic choice. A
  // `contains` check would let a write verb in; equality means adding ANY
  // node to this binary's tree — including an innocent-looking one — has to
  // come through this line, which is a deliberate stop for a binary whose
  // whole contract is what it cannot do. That is why widening it to the
  // oracle's full surface is written out verb by verb rather than derived
  // from the generated table it is checking.
  //
  // Note `plans` and `tree` sit one character from `plan` and one word from
  // planar's own `tree` verb, and NEITHER is forbidden: `forbidden_verbs`
  // lists `plan` (the planning-entity verb), and membership is tested by
  // whole-name equality, not by prefix. `planar-watch plans` LISTS plans;
  // `planar plan` mutates them. The test above would fail on `plan` and
  // passes on `plans`, which is the distinction actually intended.
  //
  // `run` is now in the tree and is NOT in `forbidden_verbs` — the
  // forbidden list names `planar-agent`'s write verbs and `planar`'s
  // planning-entity verbs, and neither has a `run`. `planar-watch run`
  // OBSERVES workflow runs; the case above would still fail if `ingest`,
  // `pull` or `capture` appeared here.
  CHECK(names == std::set<std::string, std::less<>>{"actions", "claims", "completion", "feed", "list", "log", "plans", "ps",
                                                    "queue", "run", "schema", "show", "sync-events", "tree", "version",
                                                    "history"});
}

TEST_CASE("every planar-watch verb is either implemented or refuses at 64", "[cmd][watch][capability]") {
  // Structural companion to parity.t.cpp's behavioural case: every leaf is
  // in the handler table, and every leaf is in EXACTLY ONE of the two
  // populations — the hand-registered handlers or the generated unported
  // inventory. A bulk registration's failure mode is swallowing a verb
  // that already had a real handler, so that direction is asserted too.
  auto const root  = planar::cmd::watch::root_app();
  auto const table = planar::cmd::watch::handlers(*root);

  std::set<std::string, std::less<>> unported;
  for (auto const& verb : planar::cmd::watch::unported_paths()) {
    unported.emplace(verb);
  }
  // Task 6448 landed real handlers for the three that used to be the whole
  // contents of this set. Nothing on this binary remains declared-but-
  // unported — pinned as an EMPTY set rather than deleting the assertion,
  // so a future addition to `unported_paths()` still has a test noticing it.
  CHECK(unported.empty());

  auto const leaves = planar::cliapp::leaf_keys(*root);
  // `queue` stopped being a leaf when `queue history` was added beneath it
  // (task hq-watch-history): the leaf count is unchanged, its membership is not.
  CHECK(leaves.size() == 14);
  CHECK(std::ranges::find(leaves, "queue") == leaves.end());
  CHECK(std::ranges::find(leaves, "queue history") != leaves.end());
  // It is still a verb of its own: a group with a handler, deliberately.
  CHECK(table.contains("queue"));
  for (auto const& leaf : leaves) {
    INFO("leaf: " << leaf);
    CHECK(table.contains(leaf));
  }
  for (auto const& implemented : {"feed", "ps", "claims", "actions", "plans", "log", "tree", "version", "schema", "completion",
                                  "run list", "run show", "sync-events", "queue", "queue history"}) {
    INFO("implemented verb wrongly listed as unported: " << implemented);
    CHECK_FALSE(unported.contains(implemented));
  }
}

TEST_CASE("planar-watch's root advertises the read-only invariant to operators", "[cmd][watch][capability]") {
  auto const root = planar::cmd::watch::root_app();
  CHECK(root->get_name() == "planar-watch");
  // The description is not decoration: it is where an operator reading
  // `--help` learns the handle is SQLITE_OPEN_READONLY and that the verb
  // set is the first line of defense. Losing it in transcription would
  // quietly delete the binary's own statement of its contract.
  auto const description = root->get_description();
  CHECK(description.contains("strict read-only mode"));
  CHECK(description.contains("SQLITE_OPEN_READONLY"));
  CHECK(description.contains("no write verbs registered"));
  // Task 6123 note: `cli::cmd` carried `desc` (one-line summary) and
  // `long_desc` (the prose block) as SEPARATE fields, and this case used to
  // assert they differed. `CLI::App` carries ONE description string, so
  // the longer operator-facing block is what survives and the one-line
  // summary is gone from this binary's surface — see
  // `planar.cliapp.schema`'s header, divergence 1. That is a real, named
  // loss of the swap, recorded here rather than silently dropped: the
  // schema catalog now reports the same string for "summary" and
  // "description". Note the ORACLE's catalog could not distinguish them
  // either (its emitter falls back to `desc` when `long_desc` is empty) —
  // only the rendered help page could, via indentation.
  CHECK(description.starts_with("planar-watch is the human-facing live cockpit"));
}

TEST_CASE("every planar-watch leaf is wired to a handler", "[cmd][watch][dispatch]") {
  auto const root    = planar::cmd::watch::root_app();
  auto const table   = planar::cmd::watch::handlers(*root);
  auto const missing = planar::cmd::watch::unregistered_leaves(*root, table);
  INFO("leaves with no handler: " << missing.size());
  CHECK(missing.empty());
}

TEST_CASE("no planar-watch handler is unreachable from argv", "[cmd][watch][dispatch]") {
  auto const root  = planar::cmd::watch::root_app();
  auto const table = planar::cmd::watch::handlers(*root);
  auto const dead  = planar::cmd::watch::unreachable_handlers(*root, table);
  INFO("handlers no argv can reach: " << dead.size());
  CHECK(dead.empty());
}

TEST_CASE("planar-watch applies the general dual-node rule: every node is reachable and only a handlerless group prints help",
          "[cmd][watch][dispatch][hq-watch-dispatch-general-rule]") {
  auto const root  = planar::cmd::watch::root_app();
  auto       table = planar::cmd::watch::handlers(*root);
  REQUIRE(planar::cmd::watch::unreachable_handlers(*root, table).empty());

  planar::cmd::watch::handler_fn const noop =
      [](planar::cmd::watch::context&, const planar::cliapp::parsed_args&) -> planar::cmd::watch::handler_result { return {}; };

  // Every node counts as reachable, not only the childless ones: a handler
  // registered on ANY group is dual, the way `planar` treats `handoff`, so no
  // list of allowed groups exists to be edited when the next one appears.
  auto with_group = table;
  with_group.emplace("run", noop);
  CHECK(planar::cmd::watch::unreachable_handlers(*root, with_group).empty());

  // A key that names no node at all is still dead, groups and leaves alike.
  auto with_typo = table;
  with_typo.emplace("queue histroy", noop);
  with_typo.emplace("verison", noop);
  CHECK(planar::cmd::watch::unreachable_handlers(*root, with_typo) == std::vector<std::string>{"queue histroy", "verison"});

  // The group `run` has no handler in the shipped table, so naming it prints
  // help; giving it one makes it run that handler instead of help.
  auto const run_group = [&](const planar::cmd::watch::handler_table& t) {
    std::ostringstream          out;
    std::ostringstream          err;
    planar::cmd::watch::context ctx{{"planar-watch", "run"},
                                    planar::cmd::watch::map_env({}),
                                    std::filesystem::path{},
                                    std::make_shared<planar::cmd::watch::database>(std::filesystem::path{}, err),
                                    out,
                                    err};
    auto const                  fresh = planar::cmd::watch::root_app();
    auto const                  code  = planar::cmd::watch::run(ctx, *fresh, t);
    return std::pair{code, out.str()};
  };
  auto const helped = run_group(table);
  CHECK(helped.first == 0);
  CHECK(helped.second.contains("list"));
  CHECK(helped.second.contains("show"));
  bool ran          = false;
  with_group["run"] = [&ran](planar::cmd::watch::context&,
                             const planar::cliapp::parsed_args&) -> planar::cmd::watch::handler_result {
    ran = true;
    return {};
  };
  auto const dual = run_group(with_group);
  CHECK(dual.first == 0);
  CHECK(ran);
  CHECK_FALSE(dual.second.contains("Subcommands"));
}

TEST_CASE("no planar-watch source names the queue group signaller or the terminate module",
          "[cmd][watch][capability][hq-watch-no-signaller]") {
  // The scanner is exercised before it is trusted.
  CHECK(signaller_hits("auto r = hq::signal_child_group(e, sig, host, probe, signaller);").size() == 1);
  CHECK(signaller_hits("import planar.engine.hostqueue.terminate;").size() == 1);
  CHECK(signaller_hits("::killpg(pgid, SIGTERM);").size() == 1);
  CHECK(signaller_hits("::kill(pid, SIGKILL);").size() == 1);
  CHECK(signaller_hits("// signal_child_group is never called here\n/// begin_terminate neither\n * cancel_waiting").empty());
  CHECK(signaller_hits("auto probe = hq::system_process_probe();").empty());

  // Positive control on real code: the planar-agent handler DOES call it.
  auto const agent_queue = watch_source_dir().parent_path() / "planar-agent" / "handlers" / "queue" / "queue.cpp";
  CHECK_FALSE(signaller_hits(read_file(agent_queue)).empty());

  auto const files = production_sources(watch_source_dir());
  // Not vacuous: the whole binary's sources are here, the queue handlers included.
  REQUIRE(files.size() >= 20);
  CHECK(std::ranges::any_of(files, [](const auto& f) { return f.filename() == "history.cpp"; }));
  for (auto const& file : files) {
    auto const hits = signaller_hits(read_file(file));
    INFO("planar-watch must not reach the group signaller; " << file.string() << " has: " << (hits.empty() ? "" : hits.front()));
    CHECK(hits.empty());
  }
}

TEST_CASE("the planar-watch binary does not link the queue terminate module", "[cmd][watch][capability][hq-watch-no-signaller]") {
  // The link-level half. `engine_hostqueue` is a static archive, so a member
  // object is linked only when something references it: if any handler (or
  // anything it calls) used `terminate`, the object's symbols, whose mangled
  // names carry these identifiers, would be in the binary. Read as bytes so no
  // external tool (nm) has to exist on the machine that runs the suite.
  //
  // What CANNOT be locked this way: `planar::process::identity::signal_group`
  // is in the binary, as dead code, because it shares an object with the
  // liveness probes (`process_exists`, `group_has_members`) the queue view
  // uses. Those send signal 0 only (`kill(pid, 0)`, `kill(-pgid, 0)`: existence
  // checks that deliver nothing); `signal_group` is the only call in that
  // module that sends anything else, and the source scan above and the case
  // below are what stop a watch handler from reaching it.
  constexpr std::array<std::string_view, 6> k_names{"signal_child_group",   "system_group_signaller", "begin_terminate",
                                                    "advance_terminations", "poll_and_stop",          "cancel_waiting"};
  auto const                                watch_bytes = read_file(std::filesystem::path{PLANAR_CPP_BIN});
  auto const                                agent_bytes = read_file(std::filesystem::path{PLANAR_AGENT_CPP_BIN});
  REQUIRE(watch_bytes.size() > 100'000);
  for (auto const name : k_names) {
    // Positive control: planar-agent DOES link the module, and this reading of
    // a binary sees it. Were symbols stripped or renamed, the control would
    // fail here rather than let the absence below mean nothing.
    INFO("control: planar-agent must contain " << name << " for this check to mean anything");
    CHECK(agent_bytes.contains(name));
    INFO("planar-watch links the queue signaller: " << name);
    CHECK_FALSE(watch_bytes.contains(name));
  }
}

namespace {

/// @brief Whether `line` calls `kill(` or `killpg(`, qualified or not: the
/// name must not be the tail of a longer identifier (`skill(`, `do_kill(`).
auto calls_kill(std::string_view line) -> bool {
  for (std::string_view const name : {"kill(", "killpg("}) {
    for (auto at = line.find(name); at != std::string_view::npos; at = line.find(name, at + 1)) {
      auto const before = at == 0 ? ' ' : line[at - 1];
      if (std::isalnum(static_cast<unsigned char>(before)) == 0 && before != '_') {
        return true;
      }
    }
  }
  return false;
}

/// @brief How many signal calls in `text` pass the literal signal 0, and the
/// lines of the rest.
struct kill_census {
  std::size_t              zero = 0;
  std::vector<std::string> other;
};

auto count_kills(std::string_view text) -> kill_census {
  kill_census      census;
  std::string_view rest = text;
  while (!rest.empty()) {
    auto const end  = rest.find('\n');
    auto       line = rest.substr(0, end);
    rest            = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
    if (auto const c = line.find("//"); c != std::string_view::npos) {
      line = line.substr(0, c);
    }
    if (!calls_kill(line)) {
      continue;
    }
    if (line.contains(", 0)") && !line.contains("killpg(")) {
      ++census.zero;
    } else {
      census.other.emplace_back(line);
    }
  }
  return census;
}

} // namespace

TEST_CASE("the liveness probe planar-watch uses sends signal 0 only", "[cmd][watch][capability][hq-watch-no-signaller]") {
  // The census is exercised before it is trusted: qualified, unqualified and
  // `killpg` spellings all count, an identifier that merely ends in `kill` does not.
  CHECK(count_kills("rc = ::kill(pid, 0);").zero == 1);
  CHECK(count_kills("rc = kill(pid, 0);").zero == 1);
  CHECK(count_kills("rc = std::kill(pid, 0);").zero == 1);
  CHECK(count_kills("::kill(pid, SIGKILL);").other.size() == 1);
  CHECK(count_kills("kill(pid, SIGKILL);").other.size() == 1);
  CHECK(count_kills("if (kill(-pgid, sig) == 0) {").other.size() == 1);
  CHECK(count_kills("killpg(pgid, 0);").other.size() == 1);
  CHECK(count_kills("auto x = skill(a, 0); do_kill(b, 1);").zero == 0);
  CHECK(count_kills("// kill(pid, SIGKILL)").other.empty());

  // `system_process_probe` forwards to `process_exists` and `group_has_members`
  // in `planar.process.identity`. Every signal call there but one passes the
  // literal signal 0 (existence check); the one exception is `signal_group`.
  auto const identity = watch_source_dir().parent_path().parent_path() / "lib" / "process" / "identity.cpp";
  auto const census   = count_kills(read_file(identity));
  CHECK(census.zero == 2);
  REQUIRE(census.other.size() == 1);
  CHECK(census.other.front().contains("sig)"));
}

// ---- the queue views and the main database (plan 1089, task qp-watch-queue) ----
//
// Level 2 for the `queue` and `queue history` verbs. They no longer have a
// store of their own: they read `planar.db` through the same lazily opened
// read-only handle as every other viewer verb. Four things are pinned, each
// by a case that runs the real verbs:
//  - the handle they read through is the main database's, read-only;
//  - nothing is written, created, or reaped;
//  - no second database file is opened or created, whatever the environment
//    still says about the retired agent database;
//  - no first-party planar-watch source reaches the agent database again.

/// @brief A scratch directory holding a `planar.db` at the head of the main
/// chain with one dead waiting entry and one history row.
struct seeded_main_database {
  /// @brief The scratch directory, removed on destruction.
  std::filesystem::path dir;
  /// @brief The `planar.db` inside it.
  std::filesystem::path db;

  /// @brief Creates the directory and seeds the database.
  seeded_main_database() {
    dir = std::filesystem::temp_directory_path() /
          std::format("planar_watch_cap_queue_{}", std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(dir);
    db          = dir / "planar.db";
    auto seeded = planar::db::connection::open(db.string());
    REQUIRE(seeded.has_value());
    REQUIRE(planar::db::apply_all(*seeded).has_value());
    // A waiting entry whose submitter cannot exist (pid far above any real one).
    REQUIRE(seeded
                ->execute("insert into queue_entries (state, host_id, pid, pid_started, cwd, argv, enqueued_at, refreshed_mono) "
                          "values ('waiting', 'unknown', 2147483646, 1, '/w', '[\"make\"]', 1000, 5000)")
                .has_value());
    REQUIRE(seeded
                ->execute("insert into queue_history (seq, outcome, exit_code, cwd, argv, enqueued_at, ended_at, waited_ms) "
                          "values (3, 'exited', 0, '/w', '[\"make\"]', 1000, 2000, 5)")
                .has_value());
  }
  /// @brief Not copyable: the fixture owns its directory.
  seeded_main_database(const seeded_main_database&) = delete;
  /// @brief Not copy-assignable: the fixture owns its directory.
  /// @return Never returns; deleted.
  seeded_main_database& operator=(const seeded_main_database&) = delete;
  /// @brief Removes the scratch directory.
  ~seeded_main_database() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }

  /// @brief The database file's bytes.
  /// @return The file content.
  [[nodiscard]] auto bytes() const -> std::string {
    std::ifstream in(db, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }
  /// @brief The names of everything in the scratch directory except SQLite's
  /// own `-wal` / `-shm` sidecars, which any read-only open of a WAL database
  /// may leave beside it, exactly as for every other viewer verb.
  /// @return The file names found.
  [[nodiscard]] auto listing() const -> std::set<std::string> {
    std::set<std::string> names;
    for (auto const& entry : std::filesystem::directory_iterator(dir)) {
      auto const name = entry.path().filename().string();
      if (!name.ends_with("-wal") && !name.ends_with("-shm")) {
        names.insert(name);
      }
    }
    return names;
  }
};

TEST_CASE("planar-watch queue and queue history read planar.db through the read-only main handle and write nothing",
          "[cmd][watch][capability][queue][qp-watch-queue]") {
  seeded_main_database fixture;
  auto const           before_bytes   = fixture.bytes();
  auto const           before_time    = std::filesystem::last_write_time(fixture.db);
  auto const           before_listing = fixture.listing();
  // The retired agent database's variable still names a path. It must be
  // neither read nor created.
  auto const decoy = fixture.dir / "decoy" / "agent.db";

  auto const tree  = planar::cmd::watch::root_app();
  auto const table = planar::cmd::watch::handlers(*tree);
  for (auto const& verb : std::vector<std::vector<std::string>>{{"planar-watch", "queue", "--json"},
                                                                {"planar-watch", "queue"},
                                                                {"planar-watch", "queue", "history", "--json"},
                                                                {"planar-watch", "queue", "history"}}) {
    std::ostringstream          out;
    std::ostringstream          err;
    planar::cmd::watch::context ctx{
        verb,        planar::cmd::watch::map_env({{"PLANAR_AGENT_DB", decoy.string()}, {"PLANAR_DB", fixture.db.string()}}),
        fixture.dir, std::make_shared<planar::cmd::watch::database>(fixture.db, err),
        out,         err};
    auto const fresh = planar::cmd::watch::root_app();
    INFO("verb: " << verb.back());
    REQUIRE(planar::cmd::watch::run(ctx, *fresh, table) == 0);
    CHECK(err.str().empty());
    // The handle the verb read through is the main database's own, opened
    // read-only; a write through it is refused.
    REQUIRE(ctx.db().opened());
    auto conn = ctx.db().ensure_db();
    REQUIRE(conn.has_value());
    CHECK((*conn)->is_read_only());
    CHECK_FALSE((*conn)->execute("delete from queue_entries").has_value());
    CHECK_FALSE((*conn)->execute("delete from queue_history").has_value());
    CHECK_FALSE((*conn)->execute("create table watch_cap_probe (id integer)").has_value());
    CHECK(ctx.db().path() == fixture.db);
  }

  CHECK(fixture.bytes() == before_bytes);
  CHECK((std::filesystem::last_write_time(fixture.db) == before_time));
  CHECK(fixture.listing() == before_listing);
  CHECK_FALSE(std::filesystem::exists(decoy.parent_path()));

  // The dead waiting entry is still there: the view judges, it never reaps.
  auto reopened = planar::db::connection::open_read_only(fixture.db.string());
  REQUIRE(reopened.has_value());
  auto count = reopened->prepare("select (select count(*) from queue_entries), (select count(*) from queue_history)");
  REQUIRE(count.has_value());
  REQUIRE(count->step().value() == planar::db::step_result::row);
  CHECK(count->column_int64(0) == 1);
  CHECK(count->column_int64(1) == 1);
}

TEST_CASE("no planar-watch source reaches the agent database", "[cmd][watch][capability][queue][qp-watch-queue]") {
  // The scanner is exercised before it is trusted.
  CHECK(names_agent_database("import planar.db.agentdb;"));
  CHECK(names_agent_database("auto s = open_agent_store(env);"));
  CHECK(names_agent_database("env.get(\"PLANAR_AGENT_DB\")"));
  CHECK(names_agent_database("import planar.cmd.planar_watch.agentstore;"));
  CHECK(names_agent_database("auto p = home / \".planar\" / \"agent.db\";"));
  CHECK_FALSE(names_agent_database("auto conn = ctx.db().ensure_db();"));

  auto const files = production_sources(watch_source_dir());
  REQUIRE(files.size() >= 20);
  CHECK(std::ranges::any_of(files, [](const auto& f) { return f.filename() == "history.cpp"; }));
  for (auto const& file : files) {
    INFO("planar-watch must not name the agent database; " << file.string());
    CHECK_FALSE(names_agent_database(read_file(file)));
  }
  CHECK_FALSE(std::filesystem::exists(watch_source_dir() / "agentstore.cppm"));
  CHECK_FALSE(std::filesystem::exists(watch_source_dir() / "agentstore.cpp"));
}
