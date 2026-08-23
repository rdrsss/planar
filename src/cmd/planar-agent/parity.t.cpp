// @file parity.t.cpp
// @brief Differential tests: run the built C++ `planar-agent` and the Zig
// reference over identical argv in identical pinned scratch environments,
// and require identical stdout, stderr and exit code (plan 996, task 6107).
//
// The harness lives in `../parity_harness.hpp` — a plain header, included
// rather than linked, because D18 forbids a `cmd_* -> cmd_*` edge and a
// shared layer-1 library for test scaffolding would be worse. That file
// documents the database-safety rules it enforces (`cd` then `env`, never
// `VAR=x cd dir && binary`; per-invocation capture files because Zig's
// writer uses POSITIONAL writes).
//
// A live diff is stronger evidence than a transcribed expectation, because
// a transcription can be wrong and a diff cannot. M9's parity gate rests on
// exactly this shape.
//
// ## What is compared, and what deliberately is not
//
// COMPARED: every argv shape whose output is derived entirely from a ported
// node — leaf help pages, and every parse-failure path (which is where this
// binary's exit-code divergence lives).
//
// NOT COMPARED, each for a stated reason:
//
//   `version`   The `cxx <compiler>` vs `zig <version>` divergence is
//               inherited from `planar.cli.version` and shared with the
//               operator binary. Shape is pinned in handlers.t.cpp.
//   `--help`    The ROOT page lists this tree's FOURTEEN verbs against the
//               oracle's eighteen — `ingest`, `run`, `dispatch` and
//               `context` are not ported. Not comparable until they land.
//   `schema`    Same: the catalog is a description of the tree.
//
// SKIP, not fail, when the oracle is absent: `zig/zig-out/bin/planar-agent`
// is a build artifact, not a checked-in file (D6).

#include <catch2/catch_test_macros.hpp>

// The claim-ritual cases below normalise volatile fields with a regex, and
// read the two arenas' final row state back through `planar.db`.
#include <regex>

import std;
import planar.db;

#include "parity_harness.hpp"

namespace {

using planar::cmd::parity::capture;
using planar::cmd::parity::make_arena;
using planar::cmd::parity::run_pinned;

/// @brief Path to the built C++ binary (set by this target's CMakeLists).
/// @return The path.
auto cpp_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

/// @brief Path to the Zig reference binary.
/// @return The path.
auto zig_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_ZIG_BIN};
}

/// @brief True when the reference binary is present to diff against.
/// @return `true` if the oracle exists.
auto oracle_available() -> bool {
  return std::filesystem::exists(zig_bin());
}

/// @brief Run both binaries over `args` in freshly-seeded, separately-pinned
/// scratch roots.
/// @param tag A short discriminator naming the case.
/// @param args The arguments (excluding argv[0]).
/// @return The two captures, C++ first.
auto both(std::string_view tag, std::vector<std::string> args) -> std::pair<capture, capture> {
  auto const arena = make_arena(tag);
  return {run_pinned(cpp_bin(), args, arena.cpp_root, "cpp"), run_pinned(zig_bin(), args, arena.zig_root, "zig")};
}

} // namespace

TEST_CASE("the pinned environment actually reaches planar-agent", "[cmd][agent][parity][safety]") {
  // A guard on the HARNESS, not on the binary, and it exists because this
  // harness's ancestor got it wrong once and wrote ten rows into the
  // operator's live database. If the environment stopped reaching the
  // child, `$PLANAR_DB` would fall back to `$HOME/.planar/planar.db` — so
  // this asserts the child SEES the pinned value.
  auto const                     arena = make_arena("envguard");
  std::vector<std::string> const args{"-c", "printf '%s' \"$PLANAR_DB\""};
  auto const                     got = run_pinned("/bin/sh", args, arena.cpp_root, "probe");
  CHECK(got.code == 0);
  CHECK(got.out == (arena.cpp_root / "planar.db").string());
}

TEST_CASE("planar-agent parity: parse failures match byte for byte, exit 1 included", "[cmd][agent][parity][exitcode]") {
  if (!oracle_available()) {
    SKIP("Zig oracle not built (zig/zig-out/bin/planar-agent)");
  }

  // The whole reason this binary needed its own exit module. If the port
  // had reused `planar`'s policy these would come back 2 and this case
  // would fail on the code alone, before any byte comparison.
  auto const [cpp_verb, zig_verb] = both("unknownverb", {"nosuchverb"});
  CHECK(cpp_verb.code == zig_verb.code);
  CHECK(cpp_verb.code == 1);
  CHECK(cpp_verb.out == zig_verb.out);
  CHECK(cpp_verb.err == zig_verb.err);

  auto const [cpp_flag, zig_flag] = both("unknownflag", {"version", "--badflag"});
  CHECK(cpp_flag.code == zig_flag.code);
  CHECK(cpp_flag.code == 1);
  CHECK(cpp_flag.out == zig_flag.out);
  CHECK(cpp_flag.err == zig_flag.err);
}

TEST_CASE("planar-agent parity: leaf help pages match byte for byte", "[cmd][agent][parity]") {
  if (!oracle_available()) {
    SKIP("Zig oracle not built (zig/zig-out/bin/planar-agent)");
  }

  // A leaf's help page is derived entirely from its own node — name, desc,
  // long_desc, flags, positionals — so a leaf ported at all is ported
  // completely, and a transcription slip in `tree.cpp` shows up here.
  for (auto const& leaf : {"version", "schema", "pull", "peek", "claim", "heartbeat", "claim-associate", "complete", "fail",
                           "release", "block", "reconcile", "abort", "action"}) {
    auto const [cpp, zig] = both(leaf, {leaf, "--help"});
    INFO("leaf: " << leaf);
    CHECK(cpp.code == zig.code);
    CHECK(cpp.code == 0);
    CHECK(cpp.out == zig.out);
    CHECK(cpp.err == zig.err);
  }
}

TEST_CASE("planar-agent parity: version diverges only in the runtime tag", "[cmd][agent][parity]") {
  if (!oracle_available()) {
    SKIP("Zig oracle not built (zig/zig-out/bin/planar-agent)");
  }

  auto const [cpp, zig] = both("version", {"version"});
  CHECK(cpp.code == zig.code);
  CHECK(cpp.code == 0);
  CHECK(cpp.err.empty());
  CHECK(zig.err.empty());

  // Not byte-equal, and this pins exactly HOW MUCH differs: the binary
  // name, sha and date tokens are identical; the runtime tag is not. A
  // divergence that grew beyond that would be a real port bug hiding
  // behind an "expected to differ" comment.
  auto const split = [](std::string_view line) {
    std::vector<std::string> fields;
    for (auto const part : std::views::split(line, ' ')) {
      fields.emplace_back(std::string_view{part});
    }
    return fields;
  };
  auto const cpp_fields = split(std::string_view{cpp.out}.substr(0, cpp.out.size() - 1));
  auto const zig_fields = split(std::string_view{zig.out}.substr(0, zig.out.size() - 1));
  REQUIRE(cpp_fields.size() >= 4);
  REQUIRE(zig_fields.size() == 5);
  CHECK(cpp_fields[0] == zig_fields[0]);
  CHECK(cpp_fields[0] == "planar-agent");
  CHECK(cpp_fields[1] == zig_fields[1]);
  CHECK(cpp_fields[2] == zig_fields[2]);
  CHECK(cpp_fields[3] == "cxx");
  CHECK(zig_fields[3] == "zig");

  // AND the field COUNTS differ, which `planar.cli.version`'s header
  // claims they do not — see this binary's handlers.t.cpp for the full
  // finding. Asserted here against the LIVE oracle rather than a
  // transcription, which is the strongest form the observation takes:
  // `Clang 22.1.8` carries a space of its own, so the C++ line has six
  // whitespace-separated tokens against the oracle's five. A script that
  // splits on whitespace and indexes the last field gets a different
  // answer from each binary.
  CHECK(cpp_fields.size() == 6);
  CHECK(cpp_fields.size() != zig_fields.size());
}

TEST_CASE("planar-agent parity: no ported invocation creates a database", "[cmd][agent][parity][safety]") {
  if (!oracle_available()) {
    SKIP("Zig oracle not built (zig/zig-out/bin/planar-agent)");
  }

  // The consumer policy's most operator-visible consequence, checked
  // end-to-end rather than at the context level: none of the ported verbs
  // opens SQLite, so `$PLANAR_DB` must not exist afterwards. This is also
  // the assertion that would catch an eager open added to `main`.
  auto const arena = make_arena("nodb");
  for (auto const& argv : std::vector<std::vector<std::string>>{{"version"}, {"schema"}, {"--help"}, {"nosuchverb"}}) {
    auto const tag = argv.front();
    (void)run_pinned(cpp_bin(), argv, arena.cpp_root, tag);
    INFO("argv: " << tag);
    CHECK_FALSE(std::filesystem::exists(arena.cpp_root / "planar.db"));
  }
}

// ===========================================================================
// The claim ritual, diffed end-to-end against the oracle
// ===========================================================================
//
// Everything above compares invocations that touch no database. These
// compare the verbs that ARE the database, which needs two things the
// help-page cases do not:
//
//   1. IDENTICAL SEEDED STATE in both arenas. Both are seeded by the SAME
//      binary — the Zig `planar` — so any divergence the comparison finds
//      is `planar-agent`'s and not a difference in how the fixture was
//      built. (`planar` is the operator binary; its C++ port does not yet
//      carry `init` / `plan create` / `task add`.)
//
//   2. NORMALISATION of the three fields that cannot match by
//      construction: the 32-hex claim token (minted by
//      `randomblob`), the millisecond timestamps, and the scratch root
//      path, which differs between the two arenas by design. Everything
//      else — every status, every count, every field name, every ORDER —
//      is compared literally.
//
// Normalising the token is not a weakening: `claim`'s own `--help` calls
// it opaque, and its SHAPE is pinned in agentactivity.t.cpp. What these
// cases are for is the surrounding structure.

namespace {

/// @brief The Zig OPERATOR binary, beside the agent binary the tests diff.
/// Used only to seed fixtures identically in both arenas.
/// @return The path.
auto zig_planar_bin() -> std::filesystem::path {
  return zig_bin().parent_path() / "planar";
}

/// @brief Replace the fields that cannot match across two arenas.
///
/// 32-hex runs become `<TOKEN>`, `…T…Z` instants become `<TS>`, and any
/// occurrence of either arena root becomes `<ROOT>`. Nothing else is
/// touched — in particular no status, count, key name or ordering is.
/// @param text The captured output.
/// @param root The arena root to elide.
/// @return The normalised text.
auto normalise(std::string_view text, const std::filesystem::path& root) -> std::string {
  static std::regex const token_re{"[0-9a-f]{32}"};
  static std::regex const stamp_re{"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\\.[0-9]+Z"};
  std::string             out = std::regex_replace(std::string{text}, token_re, "<TOKEN>");
  out                         = std::regex_replace(out, stamp_re, "<TS>");
  // LONGEST FORM FIRST. On this platform `/tmp` is a symlink to
  // `/private/tmp`, and the locality probe records the CANONICAL path, so
  // both spellings occur. Replacing the short form first would leave
  // `/private<ROOT>` behind and turn a match into a spurious diff.
  for (auto const& form : {"/private" + root.string(), root.string()}) {
    for (std::size_t at = out.find(form); at != std::string::npos; at = out.find(form, at + 6)) {
      out.replace(at, form.size(), "<ROOT>");
    }
  }
  return out;
}

/// @brief Seed one arena with a project, a plan and `task_count` tasks,
/// using the Zig operator binary.
/// @param root The arena root.
/// @param task_count How many tasks to create.
auto seed_arena(const std::filesystem::path& root, int task_count) -> void {
  auto const seed_root = root.string();
  (void)run_pinned(zig_planar_bin(), {"init"}, root, "seed-init");
  (void)run_pinned(zig_planar_bin(), {"assoc", "create", "project:proj", "--kind", "project"}, root, "seed-assoc");
  (void)run_pinned(zig_planar_bin(), {"assoc", "add", "project:proj", (root / "proj").string()}, root, "seed-add");
  (void)run_pinned(zig_planar_bin(), {"plan", "create", "Test plan"}, root, "seed-plan");
  for (int i = 0; i < task_count; ++i) {
    (void)run_pinned(zig_planar_bin(), {"task", "add", std::format("task {}", i), "--plan", "1"}, root,
                     std::format("seed-task-{}", i));
  }
}

/// @brief One step of a scripted comparison. `@TOKEN<n>` in an argument is
/// replaced, per arena, with claim id `<n>`'s token from THAT arena's
/// database — the tokens differ by construction, so a literal script
/// could not address them.
struct step {
  std::string_view         tag;
  std::vector<std::string> args;
};

/// @brief Read claim `<id>`'s token out of an arena's database.
/// @param root The arena root.
/// @param claim_id The claim row id.
/// @return The token, or empty when absent.
auto token_for(const std::filesystem::path& root, int claim_id) -> std::string {
  auto conn = planar::db::connection::open((root / "planar.db").string());
  if (!conn) {
    return {};
  }
  auto stmt = conn->prepare(std::format("select claim_token from agent_work_claims where id = {}", claim_id));
  if (!stmt) {
    return {};
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != planar::db::step_result::row) {
    return {};
  }
  return stmt->column_text(0);
}

/// @brief Substitute `@TOKEN<n>` placeholders against one arena.
/// @param args The scripted arguments.
/// @param root The arena root.
/// @return The concrete arguments.
auto resolve(std::span<const std::string> args, const std::filesystem::path& root) -> std::vector<std::string> {
  std::vector<std::string> out;
  out.reserve(args.size());
  for (auto const& arg : args) {
    if (arg.starts_with("@TOKEN")) {
      out.push_back(token_for(root, std::stoi(arg.substr(6))));
    } else {
      out.push_back(arg);
    }
  }
  return out;
}

} // namespace

TEST_CASE("planar-agent parity: the claim ritual, scripted end to end", "[cmd][agent][parity][claims]") {
  if (!oracle_available() || !std::filesystem::exists(zig_planar_bin())) {
    SKIP("Zig oracle not built (zig/zig-out/bin/planar-agent, zig/zig-out/bin/planar)");
  }

  auto const arena = make_arena("ritual");
  seed_arena(arena.cpp_root, 4);
  seed_arena(arena.zig_root, 4);

  // The script walks a whole session the way an orchestrator would, and
  // then walks every refusal path off it. Order matters: each step's
  // output depends on the state the previous ones left.
  std::vector<step> const script{
      {"peek", {"peek", "1"}},
      {"peek-json", {"peek", "1", "--json"}},
      {"pull", {"pull", "1"}},
      {"pull-json", {"pull", "1", "--json", "--role", "coder", "--purpose", "p", "--ttl", "30m"}},
      {"pull-badplan", {"pull", "999", "--json"}},
      {"pull-badttl", {"pull", "1", "--ttl", "zzz"}},
      {"pull-badmeta", {"pull", "1", "--metadata", "{"}},
      {"pull-stage-no-run", {"pull", "1", "--stage", "code"}},
      {"pull-badparent", {"pull", "1", "--parent-action", "999"}},
      {"pull-parent-zero", {"pull", "1", "--parent-action", "0"}},
      {"heartbeat", {"heartbeat", "--claim", "@TOKEN1"}},
      {"heartbeat-json", {"heartbeat", "--claim", "@TOKEN2", "--json", "--ttl", "1h", "--status", "editing"}},
      {"heartbeat-unknown", {"heartbeat", "--claim", "deadbeef"}},
      {"complete", {"complete", "--claim", "@TOKEN1", "--summary", "done"}},
      {"complete-twice", {"complete", "--claim", "@TOKEN1"}},
      {"release-json", {"release", "--claim", "@TOKEN2", "--reason", "giving up", "--json"}},
      {"claim", {"claim", "--entity", "task:3"}},
      {"claim-contended", {"claim", "--entity", "task:3"}},
      {"claim-force-doing", {"claim", "--entity", "task:3", "--force"}},
      {"claim-no-transition", {"claim", "--entity", "task:4", "--no-transition", "--json"}},
      {"claim-bad-kind", {"claim", "--entity", "bogus:1"}},
      {"claim-bad-id", {"claim", "--entity", "task:abc"}},
      {"claim-no-colon", {"claim", "--entity", "nocolon"}},
      {"claim-missing-task", {"claim", "--entity", "task:99"}},
      {"claim-plan", {"claim", "--entity", "plan:1", "--json"}},
      {"complete-todo-task", {"complete", "--claim", "@TOKEN4"}},
      {"fail", {"fail", "--claim", "@TOKEN3", "--reason", "broke", "--category", "tool_failure"}},
      {"action-start", {"action", "start", "--claim", "@TOKEN4", "--kind", "tool_call", "--json"}},
      {"action-start-bad-kind", {"action", "start", "--claim", "@TOKEN4", "--kind", "bogus"}},
      {"action-start-entity", {"action", "start", "--claim", "@TOKEN4", "--kind", "coder", "--entity", "decision:1"}},
      {"action-start-bad-entity", {"action", "start", "--claim", "@TOKEN4", "--kind", "coder", "--entity", "bogus:1"}},
      {"action-end", {"action", "end", "--action", "5", "--outcome", "ok", "--summary", "s", "--json"}},
      {"action-end-again", {"action", "end", "--action", "5"}},
      {"action-end-missing", {"action", "end", "--action", "999"}},
      {"action-end-bad-outcome", {"action", "end", "--action", "5", "--outcome", "bogus"}},
      {"block", {"block", "--claim", "@TOKEN4", "--blocker", "2", "--reason", "waiting", "--json"}},
      {"abort", {"abort", "--claim", "@TOKEN5", "--reason", "stuck", "--json"}},
      {"abort-unknown", {"abort", "--claim", "deadbeef"}},
      {"reconcile-dry", {"reconcile", "--dry-run"}},
      {"reconcile-dry-json", {"reconcile", "--dry-run", "--json"}},
      {"reconcile-badgrace", {"reconcile", "--stale-after", "zzz"}},
      {"reconcile", {"reconcile", "--json", "--category", "usage_limit"}},
      {"reconcile-text", {"reconcile"}},
      {"reconcile-session", {"reconcile", "--session", "1", "--json"}},
      {"reconcile-plan", {"reconcile", "--plan", "1", "--json"}},
  };

  for (auto const& [tag, args] : script) {
    auto const cpp = run_pinned(cpp_bin(), resolve(args, arena.cpp_root), arena.cpp_root, tag);
    auto const zig = run_pinned(zig_bin(), resolve(args, arena.zig_root), arena.zig_root, tag);
    INFO("step: " << tag);
    CHECK(cpp.code == zig.code);
    CHECK(normalise(cpp.out, arena.cpp_root) == normalise(zig.out, arena.zig_root));
    CHECK(normalise(cpp.err, arena.cpp_root) == normalise(zig.err, arena.zig_root));
  }
}

TEST_CASE("planar-agent parity: the two databases end in identical states", "[cmd][agent][parity][claims]") {
  if (!oracle_available() || !std::filesystem::exists(zig_planar_bin())) {
    SKIP("Zig oracle not built (zig/zig-out/bin/planar-agent, zig/zig-out/bin/planar)");
  }

  // Output parity is not state parity: two binaries can print the same
  // line and write different rows. This walks a shorter script and then
  // diffs the ROWS — statuses, categories, reasons, action outcomes, the
  // `entity_links` edge `block` writes, and the plan roll-up the injected
  // policy performs.
  auto const arena = make_arena("state");
  seed_arena(arena.cpp_root, 3);
  seed_arena(arena.zig_root, 3);

  std::vector<step> const script{
      {"pull", {"pull", "1"}},
      {"complete", {"complete", "--claim", "@TOKEN1", "--summary", "done"}},
      {"pull2", {"pull", "1"}},
      {"fail", {"fail", "--claim", "@TOKEN2", "--reason", "broke", "--category", "validation"}},
      {"claim", {"claim", "--entity", "task:3"}},
      {"block", {"block", "--claim", "@TOKEN3", "--blocker", "1", "--reason", "waiting"}},
  };
  for (auto const& [tag, args] : script) {
    (void)run_pinned(cpp_bin(), resolve(args, arena.cpp_root), arena.cpp_root, tag);
    (void)run_pinned(zig_bin(), resolve(args, arena.zig_root), arena.zig_root, tag);
  }

  auto const dump = [](const std::filesystem::path& root, std::string_view sql) {
    auto conn = planar::db::connection::open((root / "planar.db").string());
    REQUIRE(conn.has_value());
    auto stmt = conn->prepare(sql);
    REQUIRE(stmt.has_value());
    std::string out;
    while (true) {
      auto stepped = stmt->step();
      REQUIRE(stepped.has_value());
      if (*stepped != planar::db::step_result::row) {
        break;
      }
      out += stmt->column_text(0);
      out += '\n';
    }
    return out;
  };

  for (auto const& sql : {
           "select id || '|' || entity_kind || '|' || entity_id || '|' || status || '|' || "
           "coalesce(failure_category,'') || '|' || coalesce(release_reason,'') from agent_work_claims order by id",
           "select id || '|' || status from tasks order by id",
           "select id || '|' || status from plans order by id",
           "select id || '|' || action_kind || '|' || coalesce(outcome,'') || '|' || coalesce(summary,'') "
           "from agent_actions order by id",
           "select from_kind || '|' || from_id || '|' || to_kind || '|' || to_id || '|' || relationship from entity_links "
           "order by id",
       }) {
    INFO("query: " << sql);
    CHECK(dump(arena.cpp_root, sql) == dump(arena.zig_root, sql));
  }
}
