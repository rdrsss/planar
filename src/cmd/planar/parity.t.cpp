// @file parity.t.cpp
// @brief Differential tests: run the built C++ `planar` binary and the Zig
// reference binary over identical argv in identical scratch environments,
// and require identical stdout, stderr and exit code (plan 996, tasks 6105
// and 6106).
//
// This pattern did not exist before this task — there was no C++ binary to
// run. It is the strongest evidence available at this layer, because it
// compares what an operator actually sees rather than what an intermediate
// function returns, and because a captured expectation transcribed into a
// test can be transcribed wrong while a live diff cannot. M9's parity gate
// rests on exactly this shape.
//
// SKIP, not fail, when the oracle is absent: `zig/zig-out/bin/planar` is a
// build artifact, not a checked-in file. Same posture as
// src/lib/db/migrate.t.cpp (D6 — the zig/ tree is the parity oracle until
// M10 cutover).
//
// ## WHAT TASK 6123 CHANGED HERE
//
// `src/lib/cli` — the hand-rolled parser, help renderer and parse-error
// formatter — is deleted; this binary drives CLI11 directly. So every argv
// shape whose output came from THAT layer (a `--help` page, a parse
// failure) is no longer oracle-comparable, and the operator sanctioned
// re-baselining it. Those shapes moved out of the live-diff cases below
// into `the CLI surface is CLI11's now, and pinned`, where the C++ bytes
// are captured and asserted EXACTLY — not loosened to a `contains` check,
// because a re-baselined expectation that no longer discriminates is a
// rubber stamp, not a test.
//
// Everything ENGINE-derived — every renderer payload, every JSON document,
// every exit code, and the whole `unlink` id-parsing table — is still a
// live byte-for-byte diff against the oracle and still passes UNCHANGED.
// That is the load-bearing fact of this swap: nothing under
// `src/lib/engine/` moved.
//
// And the question the deleted help-page diffs actually answered — "was
// this tree transcribed from the oracle correctly?" — is now answered
// directly by `every ported command declares what the oracle declares`
// below, off the `schema` catalog. See ../catalog_parity.hpp.
//
// DATABASE SAFETY IS THE WHOLE REASON THIS FILE IS CAREFUL.
// Both binaries resolve $PLANAR_DB and otherwise fall back to
// ~/.planar/planar.db, and BOTH apply pending migrations AUTOMATICALLY on
// first use. Shelling either with the inherited environment therefore opens
// the operator's live database — and the moment a migration lands on this
// branch, `ctest` would migrate it past the version every installed binary
// supports and lock every other agent on the machine out. That is not
// hypothetical: commit 3ec6c37 fixed exactly this defect in two cli parity
// tests, found by an unexplained live-DB mtime movement. Every invocation
// below runs under a per-case scratch root with PLANAR_DB, PLANAR_HOME,
// PLANAR_CONFIG_PATH, PLANAR_LOCAL_HOME and HOME all redirected into it —
// PLANAR_LOCAL_HOME included because the `local` sandbox resolves from that
// variable (falling back to HOME), never PLANAR_HOME, and a future ported
// leaf that reached it would otherwise write symlinks into the operator's
// real ~/.claude.
//
// A NOTE ON REDIRECTING THE ZIG BINARY'S STDOUT.
// Observed while capturing fixtures for this task: the Zig runtime's file
// writer uses POSITIONAL writes, so two Zig invocations appending to the
// SAME redirect target both start at offset 0 and the second overwrites the
// first. Every invocation here gets its own freshly-created capture file,
// which sidesteps it. Do not "simplify" this into a shared append target.

#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>

import std;
import cli11;
import planar.cliapp.schema;
import planar.cmd.planar.tree;
import planar.db;
import planar.db.migrate;

#include "catalog_parity.hpp"

namespace {

/// @brief One binary's observable output for one invocation.
struct capture {
  int         code = 0; ///< The process exit status.
  std::string out;      ///< Everything written to stdout.
  std::string err;      ///< Everything written to stderr.
};

/// @brief Read a whole file as bytes, or the empty string when absent.
/// @param path The file to read.
/// @return The file's contents.
auto read_all(const std::filesystem::path& path) -> std::string {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

/// @brief Shell-quote one argument for the `/bin/sh` line built below.
/// @param value The argument.
/// @return The single-quoted form.
auto shell_quote(std::string_view value) -> std::string {
  std::string quoted = "'";
  for (char const c : value) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted += c;
    }
  }
  quoted += "'";
  return quoted;
}

/// @brief Run `bin` with `args` inside `work`, under an environment pinned
/// entirely to `work`.
/// @param bin The binary to run.
/// @param args The arguments.
/// @param work The scratch root; also the working directory.
/// @param tag A discriminator so each invocation gets its own capture files.
/// @return The captured result.
auto run_pinned(const std::filesystem::path& bin, std::span<const std::string> args, const std::filesystem::path& work,
                std::string_view tag) -> capture {
  auto const out_path = work / std::format("{}.out", tag);
  auto const err_path = work / std::format("{}.err", tag);

  // `cd` FIRST, then `env` — and never the other way round. The obvious
  // spelling, `VAR=x cd dir && binary`, silently does NOT export the
  // assignments to `binary`: POSIX keeps assignments preceding a special
  // built-in only in shells that implement that rule, and this platform's
  // /bin/sh does not (verified: `sh -c "FOO=bar cd /tmp && env | grep FOO"`
  // prints nothing). This file was written that way first, and the result
  // was that BOTH binaries fell back to the inherited HOME and wrote ten
  // annotation rows into the operator's live ~/.planar/planar.db before the
  // mismatch in row ids gave it away. Routing through `env` puts the
  // variables in the child's environment unconditionally, with no
  // shell-specific rule in the path.
  //
  // The failure was also nearly missed, because the FIRST probe for it —
  // comparing the live database file's mtime before and after — reported
  // "untouched". Under WAL (which the runtime enables on every connection)
  // writes land in the `-wal` sibling and the main `.db` file's mtime does
  // not move. An mtime check on the main file alone is not a valid
  // liveness probe here; row counts are.
  std::string line = std::format("cd {} && env", shell_quote((work / "proj").string()));
  line += std::format(" PLANAR_DB={}", shell_quote((work / "planar.db").string()));
  line += std::format(" PLANAR_HOME={}", shell_quote((work / "home").string()));
  line += std::format(" PLANAR_CONFIG_PATH={}", shell_quote((work / "config.toml").string()));
  line += std::format(" PLANAR_LOCAL_HOME={}", shell_quote((work / "localhome").string()));
  line += std::format(" HOME={}", shell_quote((work / "fakehome").string()));
  line += std::format(" PWD={} ", shell_quote((work / "proj").string()));
  line += shell_quote(bin.string());
  for (auto const& arg : args) {
    line += " " + shell_quote(arg);
  }
  line += std::format(" > {} 2> {}", shell_quote(out_path.string()), shell_quote(err_path.string()));

  int const status = std::system(line.c_str());
  int const code   = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return capture{.code = code, .out = read_all(out_path), .err = read_all(err_path)};
}

/// @brief A pair of scratch roots — one per binary — seeded identically.
struct arena {
  std::filesystem::path cpp_root; ///< Scratch root for the C++ binary.
  std::filesystem::path zig_root; ///< Scratch root for the Zig binary.
};

/// @brief Create a fresh arena.
/// @param tag A short discriminator so a failure names its own case.
/// @return The created arena.
auto make_arena(std::string_view tag) -> arena {
  auto const      base = std::filesystem::temp_directory_path() /
                         std::format("planar_cmd_parity_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  arena           result{.cpp_root = base / "cpp", .zig_root = base / "zig"};
  for (auto const& root : {result.cpp_root, result.zig_root}) {
    std::filesystem::create_directories(root / "home", ec);
    std::filesystem::create_directories(root / "proj", ec);
    std::filesystem::create_directories(root / "fakehome", ec);
  }
  return result;
}

/// @brief Path to the built C++ binary (set by this target's CMakeLists).
/// @return The path.
auto cpp_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

/// @brief Path to the Zig reference binary (set by this target's CMakeLists).
/// @return The path.
auto zig_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_ZIG_BIN};
}

/// @brief True when the reference binary is present to diff against.
/// @return `true` if the oracle exists.
auto oracle_available() -> bool {
  return std::filesystem::exists(zig_bin());
}

} // namespace

TEST_CASE("the pinned environment actually reaches the child process", "[cmd][parity][safety]") {
  // A guard on the HARNESS, not on the binary — and it exists because the
  // harness got this wrong once already (see `run_pinned`). If the
  // environment silently stops reaching the child, every other case in this
  // file keeps passing while both binaries quietly operate on the
  // operator's real ~/.planar/planar.db. The probe is a positive one: after
  // a database-backed verb, the SCRATCH database must exist. It cannot
  // unless $PLANAR_DB arrived.
  auto const space = make_arena("envpin");
  auto const got   = run_pinned(cpp_bin(), std::array<std::string, 2>{"annotate", "list"}, space.cpp_root, "envpin");
  REQUIRE(got.code == 0);
  CHECK(std::filesystem::exists(space.cpp_root / "planar.db"));
  // And the HOME fallback path must be untouched, which is what a leaked
  // environment would have used.
  CHECK_FALSE(std::filesystem::exists(space.cpp_root / "fakehome" / ".planar" / "planar.db"));

  if (oracle_available()) {
    auto const ref = run_pinned(zig_bin(), std::array<std::string, 2>{"annotate", "list"}, space.zig_root, "envpin");
    REQUIRE(ref.code == 0);
    CHECK(std::filesystem::exists(space.zig_root / "planar.db"));
    CHECK_FALSE(std::filesystem::exists(space.zig_root / "fakehome" / ".planar" / "planar.db"));
  }
}

TEST_CASE("C++ and Zig agree byte-for-byte on the no-database leaves", "[cmd][parity][oracle]") {
  if (!oracle_available()) {
    SKIP("zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");
  }

  struct leaf {
    std::string_view         tag;  ///< Case discriminator.
    std::vector<std::string> args; ///< The argv tail.
  };
  std::vector<leaf> const leaves{
      {"wl", {"workflow", "list"}},
      {"wlj", {"workflow", "list", "--json"}},
      {"wll", {"workflow", "list", "--local"}},
      {"wsmiss", {"workflow", "show", "nope"}},
      {"wsmissj", {"workflow", "show", "nope", "--json"}},
      // A HANDLER-level refusal, not a parser one: still a true oracle diff
      // after task 6123, and deliberately kept here as the control showing
      // the re-baselining is confined to parser- and help-produced bytes.
      {"aareq", {"annotate", "add"}},
      // The nine `--help` / parse-failure shapes that used to live in this
      // list moved to `the CLI surface is CLI11\'s now, and pinned` below —
      // CLI11 renders help and writes parse errors as of task 6123, so
      // those bytes are no longer oracle-comparable. They are pinned
      // against the built binary rather than dropped or loosened.
  };

  for (auto const& [tag, args] : leaves) {
    auto const space = make_arena(tag);
    auto const mine  = run_pinned(cpp_bin(), args, space.cpp_root, tag);
    auto const ref   = run_pinned(zig_bin(), args, space.zig_root, tag);

    INFO("leaf: " << tag);
    CHECK(mine.code == ref.code);
    CHECK(mine.out == ref.out);
    CHECK(mine.err == ref.err);
  }
}

TEST_CASE("C++ and Zig agree byte-for-byte on the database-backed leaves", "[cmd][parity][oracle]") {
  if (!oracle_available()) {
    SKIP("zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");
  }

  // Everything except the two wall-clock timestamp fields, which cannot
  // agree across two processes. The text renderer prints them on their own
  // `created:` / `updated:` lines, so those two lines are dropped from both
  // sides before comparing; the JSON object interleaves them, so the JSON
  // case compares the prefix up to `"created_at"` and the shape after it.
  auto const strip_timestamps = [](std::string_view text) {
    std::string kept;
    for (auto const line : std::views::split(text, '\n')) {
      std::string_view view{line.begin(), line.end()};
      if (view.starts_with("created:") || view.starts_with("updated:")) {
        continue;
      }
      kept += view;
      kept += '\n';
    }
    return kept;
  };

  struct step {
    std::string_view         tag;  ///< Case discriminator.
    std::vector<std::string> args; ///< The argv tail.
  };
  // Run as an ordered sequence against ONE database per binary, so the
  // second `annotate list` sees the row the `annotate add` before it wrote
  // — a fresh arena per step would only ever compare empty states.
  std::vector<step> const steps{
      {"al0", {"annotate", "list"}},
      {"al0j", {"annotate", "list", "--json"}},
      {"add1",
       {"annotate", "add", "--anchor-path", "src/foo.zig", "--line-start", "3", "--line-end", "7", "--title", "T1", "--body",
        "B1", "--tags", "a, b,a"}},
      {"al1", {"annotate", "list"}},
      {"badstatus", {"annotate", "list", "--status", "bogus"}},
  };

  auto const space = make_arena("dbleaves");
  for (auto const& [tag, args] : steps) {
    auto const mine = run_pinned(cpp_bin(), args, space.cpp_root, tag);
    auto const ref  = run_pinned(zig_bin(), args, space.zig_root, tag);

    INFO("step: " << tag);
    CHECK(mine.code == ref.code);
    CHECK(strip_timestamps(mine.out) == strip_timestamps(ref.out));
    CHECK(mine.err == ref.err);
  }
}

TEST_CASE("neither binary touches a database on a no-database leaf", "[cmd][parity][oracle]") {
  // The lazy-acquisition rule, observed from outside: after `workflow list`
  // and `version` and a `--help`, the scratch $PLANAR_DB file must not
  // exist at all. This is the same property `context.t.cpp` asserts through
  // `db_opened()`, checked here against the shipped binary rather than the
  // in-process context — and it is the property whose absence made commit
  // 3ec6c37's defect dangerous rather than merely untidy.
  auto const space = make_arena("nodb");
  auto const db    = space.cpp_root / "planar.db";

  for (std::vector<std::string> const args : {std::vector<std::string>{"version"},
                                              {"workflow", "list"},
                                              {"workflow", "list", "--json"},
                                              {"workflow", "show", "nope"},
                                              {"workflow", "show", "--help"}}) {
    auto const got = run_pinned(cpp_bin(), args, space.cpp_root, "nodb");
    INFO("args: " << std::format("{}", args));
    CHECK_FALSE(std::filesystem::exists(db));
    CHECK(got.code != -1);
  }
}

// =========================================================================
// Task 6106 — `unlink`, `skills`, `workspace doctor`.
// =========================================================================

TEST_CASE("C++ and Zig agree byte-for-byte on the task-6106 no-fixture leaves", "[cmd][parity][oracle]") {
  if (!oracle_available()) {
    SKIP("zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");
  }

  struct leaf {
    std::string_view         tag;  ///< Case discriminator.
    std::vector<std::string> args; ///< The argv tail.
  };
  std::vector<leaf> const leaves{
      // `skills`, `skills --help`, `skills extra`, `unlink --help`,
      // `unlink` (no positional) and `workspace doctor --help` used to be
      // here. All six are help pages or parse failures, i.e. CLI11-produced
      // as of task 6123; they moved to the pinned-bytes case below.
      // Empty database, both render modes. The text mode is ZERO BYTES
      // where `--json` is `{"orgs":[]}` — a disagreement inside one leaf.
      {"wd", {"workspace", "doctor"}},
      {"wdj", {"workspace", "doctor", "--json"}},
      // The id parser, end to end. `1_0` and `007` reach the not-found path
      // naming the PARSED value; `_10` and the overflow are refusals.
      {"ulmiss", {"unlink", "999"}},
      {"ulmissj", {"unlink", "999", "--json"}},
      {"ulsep", {"unlink", "1_0"}},
      {"ulpad", {"unlink", "007"}},
      {"ulplus", {"unlink", "+12"}},
      {"ulbad", {"unlink", "abc"}},
      {"ullead", {"unlink", "_10"}},
      {"ultrail", {"unlink", "10_"}},
      {"ulhex", {"unlink", "0x10"}},
      {"ulovf", {"unlink", "9223372036854775808"}},
      {"ulmax", {"unlink", "9223372036854775807"}},
  };

  for (auto const& [tag, args] : leaves) {
    auto const space = make_arena(tag);
    auto const mine  = run_pinned(cpp_bin(), args, space.cpp_root, tag);
    auto const ref   = run_pinned(zig_bin(), args, space.zig_root, tag);

    INFO("leaf: " << tag);
    CHECK(mine.code == ref.code);
    CHECK(mine.out == ref.out);
    CHECK(mine.err == ref.err);
  }
}

TEST_CASE("C++ and Zig agree byte-for-byte on the three ported ext leaves", "[cmd][parity][oracle]") {
  if (!oracle_available()) {
    SKIP("zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");
  }

  // `created_at` is the only wall-clock field these renderers emit and it
  // cannot agree across two processes, so it is elided from BOTH sides. The
  // elision is deliberately narrow: it replaces the VALUE and keeps the key,
  // the comma and the surrounding punctuation, so a renderer that dropped the
  // field entirely, renamed it, or moved it in the key order still fails.
  auto const mask_created_at = [](std::string_view text) {
    std::string out;
    std::size_t at = 0;
    while (true) {
      auto const key = text.find(R"("created_at":")", at);
      if (key == std::string_view::npos) {
        out += text.substr(at);
        return out;
      }
      auto const value_start = key + std::string_view{R"("created_at":")"}.size();
      auto const value_end   = text.find('"', value_start);
      if (value_end == std::string_view::npos) {
        out += text.substr(at);
        return out;
      }
      out += text.substr(at, value_start - at);
      out += "<ts>";
      at = value_end;
    }
  };

  struct step {
    std::string_view         tag;  ///< Case discriminator.
    std::vector<std::string> args; ///< The argv tail.
  };
  // Ordered against ONE database per binary, because most of what is being
  // pinned is state-dependent: `list` on an EMPTY database is a completely
  // different renderer branch from `list` on a populated one (the text form
  // says "no external systems registered" while the JSON form emits nothing
  // at all), and the duplicate-slug refusal only exists because the
  // registration two steps earlier succeeded.
  std::vector<step> const steps{
      {"xl0", {"ext", "list"}},
      {"xl0j", {"ext", "list", "--json"}},
      {"xrj",
       {"ext", "register", "jira", "sync-jira", "--base-url", "http://127.0.0.1:18041", "--project", "SYNC", "--auth-env",
        "PLANAR_SYNC_TOKEN", "--json"}},
      // No --auth-env: the gh-cli arm, whose auth_ref the JSON list below is
      // what actually reveals.
      {"xrg", {"ext", "register", "github", "gh-demo", "--project", "acme/demo"}},
      {"xrgj", {"ext", "register", "github", "gh2", "--project", "o/r", "--auth-env", "TOK", "--json"}},
      // A base URL WITH a trailing slash, stored verbatim — neither side
      // normalizes it at registration time (the adapter trims it at use).
      {"xrj2",
       {"ext", "register", "jira", "j2", "--base-url", "https://acme.atlassian.net/", "--project", "P", "--auth-env", "E"}},
      {"xl1", {"ext", "list"}},
      {"xl1j", {"ext", "list", "--json"}},
      {"xdup", {"ext", "register", "github", "gh-demo", "--project", "x/y"}},
  };
  // A MISSING REQUIRED FLAG is deliberately NOT in that list. It is a PARSER
  // refusal, and task 6123 re-baselined parser wording onto CLI11's when
  // `src/lib/cli` was deleted: the oracle says
  // `error: required flag missing: --base-url` / `error: MissingRequired`
  // while CLI11 says `error: --base-url is required` / `error: RequiredError`
  // (verified by running both). The SHAPE — formatted message to stdout,
  // CamelCase tag to stderr, exit 2 — is the operator contract and is
  // preserved; see `planar.cmd.planar.dispatch`'s header. It is asserted
  // directly below, the same way `workflow show` and `unlink` already assert
  // their own missing-positional refusals in this file, rather than diffed
  // against an oracle it is known and sanctioned to differ from.

  auto const space = make_arena("extleaves");
  for (auto const& [tag, args] : steps) {
    auto const mine = run_pinned(cpp_bin(), args, space.cpp_root, tag);
    auto const ref  = run_pinned(zig_bin(), args, space.zig_root, tag);

    INFO("step: " << tag);
    CHECK(mine.code == ref.code);
    CHECK(mask_created_at(mine.out) == mask_created_at(ref.out));
    CHECK(mine.err == ref.err);
  }

  // The re-baselined parser refusals, and the EXIT CODE the oracle still
  // agrees on. `--auth-env` is required on `register jira` and optional on
  // `register github`, so both arms are checked: a tree that marked the
  // github one required would refuse a legitimate gh-cli registration.
  auto const missing_flag = run_pinned(cpp_bin(), std::array<std::string, 6>{"ext", "register", "jira", "j3", "--project", "P"},
                                       space.cpp_root, "xmiss");
  CHECK(missing_flag.code == 2);
  CHECK(missing_flag.err == "error: RequiredError\n");

  auto const oracle_missing_flag = run_pinned(
      zig_bin(), std::array<std::string, 6>{"ext", "register", "jira", "j3", "--project", "P"}, space.zig_root, "xmissz");
  // Same code, different wording — the sanctioned half of the divergence.
  CHECK(missing_flag.code == oracle_missing_flag.code);

  auto const github_without_auth_env = run_pinned(
      cpp_bin(), std::array<std::string, 6>{"ext", "register", "github", "gh3", "--project", "o/r"}, space.cpp_root, "xnoauth");
  CHECK(github_without_auth_env.code == 0);
}

TEST_CASE("C++ and Zig agree on unlink over a seeded external link", "[cmd][parity][oracle]") {
  if (!oracle_available()) {
    SKIP("zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");
  }

  // `init`, `ext register` and `link` are NOT ported, so the fixture is
  // built by running the ORACLE in BOTH arenas. That is legitimate here
  // and worth being explicit about: the subject under test is `unlink`,
  // and seeding both sides with the same binary means the two databases
  // start identical by construction rather than by assertion.
  auto const                                  space = make_arena("ulseed");
  std::vector<std::vector<std::string>> const seed{
      {"init"},
      {"ext", "register", "github", "gh", "--project", "owner/repo"},
      {"link", "plan:1", "--to", "gh:42", "--json"},
      {"link", "plan:2", "--to", "gh:43", "--json"},
  };
  for (std::size_t i = 0; i < seed.size(); ++i) {
    auto const tag = std::format("seed{}", i);
    auto const a   = run_pinned(zig_bin(), seed[i], space.cpp_root, tag);
    auto const b   = run_pinned(zig_bin(), seed[i], space.zig_root, tag);
    INFO("seed step: " << tag);
    REQUIRE(a.code == 0);
    REQUIRE(b.code == 0);
  }

  struct step {
    std::string_view         tag;  ///< Case discriminator.
    std::vector<std::string> args; ///< The argv tail.
  };
  // Ordered against ONE database per binary: the second `unlink 1` only
  // reaches the not-found path because the first one succeeded.
  std::vector<step> const steps{
      {"ul1", {"unlink", "1"}},
      {"ul2j", {"unlink", "2", "--json"}},
      {"ul1again", {"unlink", "1"}},
  };
  for (auto const& [tag, args] : steps) {
    auto const mine = run_pinned(cpp_bin(), args, space.cpp_root, tag);
    auto const ref  = run_pinned(zig_bin(), args, space.zig_root, tag);
    INFO("step: " << tag);
    CHECK(mine.code == ref.code);
    CHECK(mine.out == ref.out);
    CHECK(mine.err == ref.err);
  }

  // The audit trail is part of the contract and is invisible in stdout, so
  // it is read back out of each arena's database and diffed too. No verb
  // renders `session_entries` (there is no `capture show`), so the rows are
  // read directly. Without this, a port that dropped the `session_entries`
  // append entirely would pass every assertion above.
  auto const entries = [](const std::filesystem::path& root) {
    auto conn = planar::db::connection::open((root / "planar.db").string());
    REQUIRE(conn.has_value());
    auto stmt = conn->prepare("select s.vendor, e.prefix, e.body from session_entries e "
                              "join sessions s on s.id = e.session_id order by e.session_id, e.ordinal");
    REQUIRE(stmt.has_value());
    std::string rendered;
    for (;;) {
      auto stepped = stmt->step();
      REQUIRE(stepped.has_value());
      if (*stepped == planar::db::step_result::done) {
        break;
      }
      rendered += std::format("{}|{}|{}\n", stmt->column_text(0), stmt->column_text(1), stmt->column_text(2));
    }
    return rendered;
  };
  auto const mine_entries = entries(space.cpp_root);
  CHECK(mine_entries == entries(space.zig_root));
  CHECK(mine_entries == "cli|action|unlink: removed external link 1\n"
                        "cli|action|unlink: removed external link 2\n");
}

TEST_CASE("C++ and Zig agree on workspace doctor's diagnose-and-repair pass", "[cmd][parity][oracle]") {
  if (!oracle_available()) {
    SKIP("zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");
  }

  // `workspace init` refuses without child directories containing `.git`,
  // and building two of those per arena just to reach `doctor` would make
  // the fixture about `init`. The org row is inserted directly instead —
  // through `planar.db`, into each arena's own scratch database, with that
  // arena's own `proj/` as the recorded `root_path`.
  //
  // Because the two arenas are at DIFFERENT paths and doctor's output
  // carries absolute paths, the comparison substitutes each side's root
  // prefix with a placeholder before diffing. That is the only normalized
  // comparison in this file; everything else is raw bytes. The substitution
  // is total — if either binary emitted a path outside its own arena the
  // placeholder would not cover it and the diff would fail.
  auto const space = make_arena("wdorg");
  for (auto const& root : {space.cpp_root, space.zig_root}) {
    auto conn = planar::db::connection::open((root / "planar.db").string());
    REQUIRE(conn.has_value());
    REQUIRE(planar::db::apply_all(*conn).has_value());
    auto stmt = conn->prepare("insert into associations (slug, name, kind, config_json) values ('acme','acme','org',?)");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->bind_text(1, std::format(R"({{"root_path":"{}"}})", (root / "proj").string())).has_value());
    REQUIRE(stmt->step().has_value());
  }

  auto const normalize = [](std::string text, const std::filesystem::path& root) {
    auto const       needle = root.string();
    std::string      out;
    std::string_view rest{text};
    for (;;) {
      auto const at = rest.find(needle);
      if (at == std::string_view::npos) {
        out.append(rest);
        break;
      }
      out.append(rest.substr(0, at));
      out.append("<ARENA>");
      rest.remove_prefix(at + needle.size());
    }
    return out;
  };

  for (auto const& [tag, args] : std::vector<std::pair<std::string, std::vector<std::string>>>{
           {"wdrepair", {"workspace", "doctor"}},
           // Second pass: the state directory now exists, so the `fix` line
           // is gone and the issue count drops. Running it twice is what
           // proves the first pass REPAIRED rather than merely reported.
           {"wdsecond", {"workspace", "doctor", "--json"}},
       }) {
    auto const mine = run_pinned(cpp_bin(), args, space.cpp_root, tag);
    auto const ref  = run_pinned(zig_bin(), args, space.zig_root, tag);
    INFO("step: " << tag);
    CHECK(mine.code == ref.code);
    CHECK(normalize(mine.out, space.cpp_root) == normalize(ref.out, space.zig_root));
    CHECK(mine.err == ref.err);
  }

  // The repair reached the filesystem on BOTH sides, identically.
  for (auto const& root : {space.cpp_root, space.zig_root}) {
    INFO("arena: " << root.string());
    CHECK(std::filesystem::exists(root / "home" / "workspaces" / "1"));
    CHECK(std::filesystem::is_symlink(root / "proj" / "AGENTS.md"));
    CHECK(std::filesystem::is_symlink(root / "proj" / "CLAUDE.md"));
  }
}

TEST_CASE("every ported command declares what the oracle declares", "[cmd][parity][oracle][catalog]") {
  if (!oracle_available()) {
    SKIP("zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");
  }

  // TASK 6123. This case replaces "C++ and Zig agree byte-for-byte on the
  // workbench leaves' help pages", which diffed ten rendered pages against
  // the oracle. That diff existed to catch a half-ported leaf — dropping
  // `--filter-mode` from `archive` because the engine ignores it, say —
  // and a rendered page was the observable proxy for the declaration.
  // CLI11 renders help now, so the proxy is gone; the declaration is not,
  // and this compares it directly and across the WHOLE tree rather than
  // ten leaves. See ../catalog_parity.hpp for the full argument.
  //
  // TASK 6065 changed two things here. The C++ side is now the BUILT
  // BINARY's own `planar schema` output rather than an in-process
  // `schema_json(root_app())` — `planar schema` did not exist as a verb
  // before this task, which is why the binary carrying 223 of the 260
  // leaves was the one the parity harness could not read. And the
  // comparison is now two-directional, because the tree is no longer a
  // deliberate subset.
  auto const space  = make_arena("catalog");
  auto const ref    = run_pinned(zig_bin(), std::array<std::string, 1>{"schema"}, space.zig_root, "zig");
  auto const actual = run_pinned(cpp_bin(), std::array<std::string, 1>{"schema"}, space.cpp_root, "cpp");
  REQUIRE(ref.code == 0);
  REQUIRE(actual.code == 0);

  auto const mine = planar::cmd::parity::parse_catalog(actual.out);
  REQUIRE(mine.has_value());
  auto const theirs = planar::cmd::parity::parse_catalog(ref.out);
  REQUIRE(theirs.has_value());

  // Non-vacuous: an empty left-hand side would pass trivially, and the
  // named entries below are the ones whose declarations this task most
  // easily could have got wrong.
  CHECK(mine->size() == 261);
  CHECK(theirs->size() == 261);
  CHECK(mine->contains("planar workbench gc"));
  CHECK(mine->contains("planar workbench archive"));
  CHECK(mine->contains("planar annotate add"));
  CHECK(mine->contains("planar unlink"));
  // Four the tree did not carry at all before task 6065: a deep leaf, a
  // dual node, and the two tree-describing verbs this task implemented.
  CHECK(mine->contains("planar plan next"));
  CHECK(mine->contains("planar health"));
  CHECK(mine->contains("planar schema"));
  CHECK(mine->contains("planar completion"));

  auto const problems = planar::cmd::parity::diff_against_oracle(*mine, *theirs);
  INFO("declaration mismatches:\n" << std::format("{}", problems));
  CHECK(problems.empty());

  auto const missing = planar::cmd::parity::oracle_only_commands(*mine, *theirs);
  INFO("declared by the oracle and NOT by this binary:\n" << std::format("{}", missing));
  CHECK(missing.empty());
}

TEST_CASE("all three catalogs are byte-identical to the oracle's", "[cmd][parity][oracle][catalog]") {
  if (!oracle_available()) {
    SKIP("zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");
  }
  // This case used to be titled "the catalog differs from the oracle's in
  // FOUR values, all `default`", and it pinned that residue precisely: the
  // four flags declaring an EMPTY-STRING default, which `CLI::Option`
  // cannot distinguish from "no default" because `get_default_str()`
  // answers `""` to both. Task 6130 closed it by supplying the four as
  // data — see `planar.cliapp.schema`'s three-argument `schema_json` and
  // `planar.cmd.planar.surface::surface_empty_string_defaults`.
  //
  // So the assertion is now the strongest form available: byte equality,
  // no patching step. `planar-agent` and `planar-watch` already had it.
  //
  // This is also the standing guard for task 6138. Every bool flag in all
  // three trees now declares a `--no-X` negation, and CLI11 files that
  // name into `Option::lnames_` as well as `fnames_` — so the moment
  // `planar.cliapp.schema::aliases_of` stops subtracting the negations,
  // all 341 of them appear as `"aliases"` here and this case fails on the
  // first one.
  auto const space  = make_arena("catalogbytes");
  auto const ref    = run_pinned(zig_bin(), std::array<std::string, 1>{"schema"}, space.zig_root, "zig");
  auto const actual = run_pinned(cpp_bin(), std::array<std::string, 1>{"schema"}, space.cpp_root, "cpp");
  REQUIRE(ref.code == 0);
  REQUIRE(actual.code == 0);
  REQUIRE(ref.out.size() > 300000); // Not two empty strings.

  CHECK(ref.out == actual.out);
  // Non-vacuity for the 6130 half specifically: the four empty-string
  // defaults are PRESENT rather than absent from both sides.
  CHECK(actual.out.contains(R"("default":"",)"));
  for (auto const& flag :
       {R"("long":"--editor")", R"("long":"--args")", R"("long":"--worktree")", R"("long":"--sandbox-root")"}) {
    INFO(flag);
    CHECK(actual.out.contains(flag));
  }
  // Non-vacuity for the 6138 half: no negation leaked into the catalog.
  CHECK_FALSE(actual.out.contains("--no-json"));
}

TEST_CASE("the CLI surface is CLI11's now, and pinned", "[cmd][parity][cli-surface]") {
  // TASK 6123 RE-BASELINE, in one place so the whole cost of the swap is
  // readable at a glance. Every expectation here used to be a live diff
  // against the Zig oracle in one of the two "no-database leaves" cases
  // above. `src/lib/cli` produced those bytes; CLI11 produces these.
  //
  // Captured from the BUILT binary exactly the way the oracle captures
  // were taken, and pinned EXACTLY — trailing spaces included. Loosening
  // any of these to a `contains` check would replace a pinned contract
  // with a rubber stamp: a dropped flag would stop failing.
  //
  // The EXIT CODES are not re-baselined and are the part that still had to
  // survive: 2 for every parse failure on the OPERATOR binary, where
  // planar-agent and planar-watch both say 1.
  //
  // Runs without the oracle — it is an assertion about this binary, not a
  // comparison — so it stays live on a checkout with no zig/ build.
  auto const space = make_arena("clisurface");
  auto const run   = [&](std::vector<std::string> args, std::string_view tag) {
    return run_pinned(cpp_bin(), args, space.cpp_root, tag);
  };

  SECTION("parse failures write BOTH streams and exit 2") {
    // The dual-stream shape is the surprising part and the reason these
    // are pinned at all: a formatted message on STDOUT and a CamelCase tag
    // on STDERR, from one invocation. A reasonable person would have put
    // the parse error on stderr alone and been wrong. That shape is the
    // oracle's and did not move.
    auto const bad_verb = run({"nosuchverb"}, "badverb");
    CHECK(bad_verb.code == 2);
    CHECK(bad_verb.out == "error: planar: The following argument was not expected: nosuchverb\n");
    CHECK(bad_verb.err == "error: ExtrasError\n");

    auto const bad_sub = run({"workflow", "nosuchsub"}, "badsub");
    CHECK(bad_sub.code == 2);
    CHECK(bad_sub.out == "error: workflow: The following argument was not expected: nosuchsub\n");
    CHECK(bad_sub.err == "error: ExtrasError\n");

    auto const bad_flag = run({"workflow", "list", "--nosuchflag"}, "badflag");
    CHECK(bad_flag.code == 2);
    CHECK(bad_flag.out == "error: list: The following argument was not expected: --nosuchflag\n");
    CHECK(bad_flag.err == "error: ExtrasError\n");

    auto const missing_pos = run({"workflow", "show"}, "missingpos");
    CHECK(missing_pos.code == 2);
    CHECK(missing_pos.out == "error: name is required\n");
    CHECK(missing_pos.err == "error: RequiredError\n");

    auto const unlink_no_pos = run({"unlink"}, "ulnopos");
    CHECK(unlink_no_pos.code == 2);
    CHECK(unlink_no_pos.out == "error: link-id is required\n");
    CHECK(unlink_no_pos.err == "error: RequiredError\n");

    auto const skills_extra = run({"skills", "extra"}, "skextra");
    CHECK(skills_extra.code == 2);
    CHECK(skills_extra.out == "error: skills: The following argument was not expected: extra\n");
    CHECK(skills_extra.err == "error: ExtrasError\n");
  }

  SECTION("leaf help pages render every declared flag and positional") {
    auto const version_help = run({"version", "--help"}, "vhelp");
    CHECK(version_help.code == 0);
    CHECK(version_help.err.empty());
    CHECK(version_help.out == "Print the planar version, commit, and zig runtime.\n"
                              "\n"
                              "\n"
                              "version [OPTIONS]\n"
                              "\n"
                              "\n"
                              "OPTIONS:\n"
                              "  -h,     --help              Print this help message and exit\n");

    auto const list_help = run({"workflow", "list", "--help"}, "wlhelp");
    CHECK(list_help.code == 0);
    CHECK(list_help.out == "List shipped and sandbox workflows.\n"
                           "\n"
                           "\n"
                           "list [OPTIONS]\n"
                           "\n"
                           "\n"
                           "OPTIONS:\n"
                           "  -h,     --help              Print this help message and exit\n"
                           "          --local             \n"
                           "          --json              \n");

    auto const show_help = run({"workflow", "show", "--help"}, "wshelp");
    CHECK(show_help.code == 0);
    CHECK(show_help.out == "Show @meta and source path for a named workflow.\n"
                           "\n"
                           "\n"
                           "show [OPTIONS] name\n"
                           "\n"
                           "\n"
                           "POSITIONALS:\n"
                           "  name REQUIRED               \n"
                           "\n"
                           "OPTIONS:\n"
                           "  -h,     --help              Print this help message and exit\n"
                           "          --json              \n");

    // The widest leaf in the binary: fifteen flags, two of them integer-
    // kinded. The `:INT` type tag comes from `cliapp::zig_int_validator`,
    // which is also what enforces Zig's underscore-separator semantics at
    // parse time — so this page is where a lost validator shows up.
    auto const annotate_add_help = run({"annotate", "add", "--help"}, "aahelp");
    CHECK(annotate_add_help.code == 0);
    CHECK(annotate_add_help.out == "Create a new annotation.\n"
                                   "\n"
                                   "\n"
                                   "add [OPTIONS]\n"
                                   "\n"
                                   "\n"
                                   "OPTIONS:\n"
                                   "  -h,     --help              Print this help message and exit\n"
                                   "          --anchor-path       \n"
                                   "          --line-start :INT   \n"
                                   "          --line-end :INT     \n"
                                   "          --commit-sha        \n"
                                   "          --text-hash         \n"
                                   "          --text              \n"
                                   "          --title             \n"
                                   "          --slug              \n"
                                   "          --body              \n"
                                   "          --vendor            \n"
                                   "          --plan :INT         \n"
                                   "          --task :INT         \n"
                                   "          --tags              \n"
                                   "          --scope             \n"
                                   "          --json              \n");

    auto const annotate_list_help = run({"annotate", "list", "--help"}, "alhelp");
    CHECK(annotate_list_help.code == 0);
    CHECK(annotate_list_help.out == "List annotations.\n"
                                    "\n"
                                    "\n"
                                    "list [OPTIONS]\n"
                                    "\n"
                                    "\n"
                                    "OPTIONS:\n"
                                    "  -h,     --help              Print this help message and exit\n"
                                    "          --anchor-path       \n"
                                    "          --status            \n"
                                    "          --plan :INT         \n"
                                    "          --task :INT         \n"
                                    "          --vendor            \n"
                                    "          --tag               \n"
                                    "          --scope             \n"
                                    "          --json              \n");

    // The only top-level LEAF with a positional AND a described flag.
    auto const unlink_help = run({"unlink", "--help"}, "ulhelp");
    CHECK(unlink_help.code == 0);
    CHECK(unlink_help.out == "Remove an external_links row by its link id.\n"
                             "\n"
                             "Associated sync_events rows are detached by setting link_id to null\n"
                             "rather than cascade-deleted; they are no longer reachable through\n"
                             "the deleted link's audit trail.\n"
                             "\n"
                             "\n"
                             "unlink [OPTIONS] link-id\n"
                             "\n"
                             "\n"
                             "POSITIONALS:\n"
                             "  link-id REQUIRED            External-link id (integer)\n"
                             "\n"
                             "OPTIONS:\n"
                             "  -h,     --help              Print this help message and exit\n"
                             "          --scope             Scope for the cross-scope guard (currently informational)\n"
                             "          --json              \n");

    auto const doctor_help = run({"workspace", "doctor", "--help"}, "wdhelp");
    CHECK(doctor_help.code == 0);
    CHECK(doctor_help.out == "Scan and fix workspace registration and state consistency.\n"
                             "\n"
                             "\n"
                             "doctor [OPTIONS]\n"
                             "\n"
                             "\n"
                             "OPTIONS:\n"
                             "  -h,     --help              Print this help message and exit\n"
                             "          --json              \n");
  }

  SECTION("workbench leaf pages keep every flag the oracle accepts") {
    // Two of the ten `workbench` leaves, chosen as the two whose flag sets
    // are hardest to get right: `push` carries the only wrapped
    // description in the tree, and `gc` carries the most flags plus an
    // OPTIONAL positional (rendered `[plan]`, not `plan`). Declaration
    // fidelity for all ten — including `archive`/`restore` still declaring
    // `--filter-mode` even though the engine ignores it — is covered
    // against the oracle by the catalog case above; these two pin that the
    // RENDERING of a declared surface is intact.
    auto const push_help = run({"workbench", "push", "--help"}, "wbpush");
    CHECK(push_help.code == 0);
    CHECK(push_help.out == "Apply DB\xe2\x86\x92"
                           "FS changes atomically; report FS\xe2\x86\x92"
                           "DB drift.\n"
                           "\n"
                           "\n"
                           "push [OPTIONS] plan\n"
                           "\n"
                           "\n"
                           "POSITIONALS:\n"
                           "  plan REQUIRED               \n"
                           "\n"
                           "OPTIONS:\n"
                           "  -h,     --help              Print this help message and exit\n"
                           "          --verbose           \n"
                           "          --json              \n"
                           "          --filter-mode       Terminal-status filter: 'failures' (default) or 'all'\n"
                           "          --apply-cleanup     Remove pre-existing FS files for entities this push would have\n"
                           "                              filtered\n");

    auto const gc_help = run({"workbench", "gc", "--help"}, "wbgc");
    CHECK(gc_help.code == 0);
    CHECK(gc_help.out == "Remove FS files whose backing entity is terminal in the DB.\n"
                         "\n"
                         "\n"
                         "gc [OPTIONS] [plan]\n"
                         "\n"
                         "\n"
                         "POSITIONALS:\n"
                         "  plan                        \n"
                         "\n"
                         "OPTIONS:\n"
                         "  -h,     --help              Print this help message and exit\n"
                         "          --dry-run           Preview only; do not touch disk\n"
                         "          --yes               Discard FS-content drift; remove drifted files anyway\n"
                         "          --filter-mode       Terminal-status filter: 'failures' (default) or 'all'\n"
                         "          --all-scopes        Walk every plan's workbench tree\n"
                         "          --json              \n");
  }

  SECTION("the retired `skills` verb still points operators at scriptorium") {
    // Both routes to that page — the bare verb (which needs a HANDLER
    // here, because a childless node is a leaf) and `--help` (which goes
    // through dispatch) — must agree, or the handler is rendering
    // something the tree does not say.
    auto const bare   = run({"skills"}, "sk");
    auto const helped = run({"skills", "--help"}, "skhelp");
    CHECK(bare.code == 0);
    CHECK(helped.code == 0);
    CHECK(bare.out == helped.out);
    CHECK(bare.out.starts_with("The unified skill source tree under skills/src/ is rendered by the\n"
                               "external scriptorium binary (plan 918)."));
    CHECK(bare.out.contains("`scriptorium\ncheck`/`scriptorium status` instead."));
  }
}

TEST_CASE("C++ and Zig agree over a seeded workbench feature tree", "[cmd][parity][oracle][workbench]") {
  if (!oracle_available()) {
    SKIP("zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");
  }

  // SEED EACH ARENA INDEPENDENTLY, never by copying one. `projects.root_path`
  // is absolute, so a copied arena silently loses cwd-derived scope, and
  // `task update` then touches a different set of rows on the two sides. The
  // first draft of this fixture copied, and the resulting "divergence" was
  // the harness, not the port.
  auto const                                  space = make_arena("wbtree");
  std::vector<std::vector<std::string>> const seed{
      {"init", "--name", "demo", "--slug", "demo"},
      {"assoc", "create", "project:demo", "--kind", "project"},
      {"plan", "create", "Demo Feature", "--slug", "demo-feature", "--summary", "A demo."},
      {"task", "add", "First Task", "--plan", "1", "--body", "Task body here.", "--editor=false"},
      {"task", "add", "Second Task", "--plan", "1", "--editor=false"},
      {"artifact", "add", "Tech Spec: Auth", "--kind", "tech_spec", "--plan", "1", "--body", "Spec body.", "--editor=false"},
      {"decision", "add", "Use SQLite", "--plan", "1", "--body", "We use SQLite.", "--rationale", "Simple."},
      {"question", "add", "Which format?", "--plan", "1"},
      {"scenario", "add", "Round trip", "--plan", "1"},
      {"plan", "create", "Child Milestone", "--slug", "child-ms", "--parent", "1"},
  };
  for (auto const& root : {space.cpp_root, space.zig_root}) {
    // `assoc add` needs the arena's own project directory, so it is issued
    // per-arena rather than from the shared list above.
    for (std::size_t i = 0; i < seed.size(); ++i) {
      auto const tag = std::format("seed{}_{}", root == space.cpp_root ? "c" : "z", i);
      auto const ran = run_pinned(zig_bin(), seed[i], root, tag);
      INFO("seed step: " << tag << " -> " << ran.err);
      REQUIRE(ran.code == 0);
      if (i == 1) {
        std::vector<std::string> const attach{"assoc", "add", "project:demo", (root / "proj").string()};
        REQUIRE(run_pinned(zig_bin(), attach, root, std::format("{}attach", tag)).code == 0);
      }
    }
  }

  struct step {
    std::string_view         tag;  ///< Case discriminator.
    std::vector<std::string> args; ///< The argv tail.
  };
  // An ORDERED sequence against ONE tree per binary. Each step depends on
  // the one before: the filter counts only appear once a task is cancelled,
  // and `restore` only means anything after `archive`.
  std::vector<step> const steps{
      {"wblist0", {"workbench", "list"}},
      {"wbstat0", {"workbench", "status"}},
      {"wbpush", {"workbench", "push", "1", "--verbose"}},
      {"wbpushj", {"workbench", "push", "1", "--json"}},
      {"wbstat1", {"workbench", "status", "1", "--verbose"}},
      {"wblist1", {"workbench", "list", "--json"}},
      {"wblint", {"workbench", "lint", "1"}},
      {"wblintall", {"workbench", "lint", "--all"}},
      {"wbgcdry", {"workbench", "gc", "1", "--dry-run"}},
      {"wbpull", {"workbench", "pull", "1", "--verbose"}},
      {"wbbadplan", {"workbench", "push", "999"}},
      {"wbbadplan0", {"workbench", "push", "0"}},
      {"wbchild", {"workbench", "push", "2"}},
      {"wbbadmode", {"workbench", "push", "1", "--filter-mode", "nope"}},
      {"wbexcl", {"workbench", "push", "1", "--filter-mode", "all", "--apply-cleanup"}},
      {"wblintnone", {"workbench", "lint"}},
      {"wbgcnoplan", {"workbench", "gc"}},
      {"wbresbad", {"workbench", "resolve", "abc", "--prefer", "fs"}},
      {"wbresnf", {"workbench", "resolve", "99", "--prefer", "fs"}},
      {"wbarch", {"workbench", "archive", "1", "--json"}},
      {"wbarch2", {"workbench", "archive", "1"}},
      {"wbrest", {"workbench", "restore", "1", "--json"}},
      {"wbgcall", {"workbench", "gc", "--all-scopes", "--json"}},
  };

  // `archive` and `restore` print the ABSOLUTE feature directory, which
  // necessarily differs between the two arenas. Normalizing the arena root
  // is what keeps those two steps comparable without weakening them.
  auto const normalize = [](std::string_view text, const std::filesystem::path& root) {
    std::string const needle = root.string();
    std::string       out;
    std::string_view  rest = text;
    for (;;) {
      auto const at = rest.find(needle);
      if (at == std::string_view::npos) {
        out.append(rest);
        return out;
      }
      out.append(rest.substr(0, at));
      out.append("<ARENA>");
      rest.remove_prefix(at + needle.size());
    }
  };

  for (auto const& [tag, args] : steps) {
    auto const mine = run_pinned(cpp_bin(), args, space.cpp_root, tag);
    auto const ref  = run_pinned(zig_bin(), args, space.zig_root, tag);
    INFO("step: " << tag);
    CHECK(mine.code == ref.code);
    CHECK(normalize(mine.out, space.cpp_root) == normalize(ref.out, space.zig_root));
    CHECK(normalize(mine.err, space.cpp_root) == normalize(ref.err, space.zig_root));
  }

  // The RENDERED FILES are the real product, and none of the summary lines
  // above would notice a byte-level divergence in them. Every `.md` under
  // each arena's workbench root is compared directly. `.sync` is compared
  // on its first three columns only: the fourth is `last_synced_at`, which
  // SQLite stamps at write time and cannot agree across two processes.
  auto const tree_of = [](const std::filesystem::path& root) {
    std::map<std::string, std::string> files;
    auto const                         wb = root / "fakehome" / ".planar" / "workbench";
    std::error_code                    ec;
    if (!std::filesystem::is_directory(wb, ec)) {
      return files;
    }
    for (auto const& entry : std::filesystem::recursive_directory_iterator(wb, ec)) {
      std::error_code entry_ec;
      if (!entry.is_regular_file(entry_ec)) {
        continue;
      }
      auto const rel     = std::filesystem::relative(entry.path(), wb, entry_ec).generic_string();
      auto       content = read_all(entry.path());
      // The two arenas are seeded independently, so every entity's
      // `created_at` / `updated_at` differs by however long the first seed
      // took. Those reach the rendered file as `**Created:**` /
      // `**Updated:**` lines; drop them from both sides, exactly as the
      // annotate parity case drops its `created:` / `updated:` lines.
      // Everything else in the file -- front matter, headings, body,
      // hard-break double spaces -- is still compared byte for byte.
      {
        std::string kept;
        for (auto const line : std::views::split(content, '\n')) {
          std::string_view view{line.begin(), line.end()};
          if (view.starts_with("**Created:**") || view.starts_with("**Updated:**")) {
            continue;
          }
          kept.append(view);
          kept += '\n';
        }
        content = std::move(kept);
      }
      if (rel.ends_with(".sync")) {
        // `.sync` keeps only its FIRST TWO columns: the path and the
        // `<kind>:<id>` pair. The third is the content hash -- which
        // digests the timestamped file and therefore cannot agree across
        // two arenas -- and the fourth is `last_synced_at`, stamped by
        // SQLite at write time. Path and entity mapping are the part of
        // this mirror that IS a contract, and they are compared in full.
        std::string trimmed;
        for (auto const line : std::views::split(content, '\n')) {
          std::string_view view{line.begin(), line.end()};
          if (view.empty()) {
            continue;
          }
          auto const first_tab = view.find('\t');
          if (first_tab == std::string_view::npos) {
            trimmed.append(view);
            trimmed += '\n';
            continue;
          }
          auto const second_tab = view.find('\t', first_tab + 1);
          trimmed.append(second_tab == std::string_view::npos ? view : view.substr(0, second_tab));
          trimmed += '\n';
        }
        content = trimmed;
      }
      files.emplace(rel, std::move(content));
    }
    return files;
  };
  auto const mine_tree = tree_of(space.cpp_root);
  auto const ref_tree  = tree_of(space.zig_root);
  // A non-empty tree on both sides, so an arrangement where NEITHER wrote
  // anything cannot pass this vacuously.
  CHECK_FALSE(mine_tree.empty());
  CHECK(mine_tree.size() == ref_tree.size());
  for (auto const& [rel, content] : mine_tree) {
    INFO("file: " << rel);
    auto const it = ref_tree.find(rel);
    REQUIRE(it != ref_tree.end());
    CHECK(content == it->second);
  }
}

TEST_CASE("C++ and Zig agree on init, including the git remote it captures", "[cmd][parity][oracle][init]") {
  if (!oracle_available()) {
    SKIP("zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");
  }

  // `init` ECHOES ITS ARENA. The database path and the project root path
  // appear verbatim in both render modes, and the two binaries necessarily
  // run under different scratch roots, so a raw byte diff would fail on the
  // one difference that is not a divergence. Each side's own root is
  // replaced with a placeholder before comparing — which leaves the schema
  // version, the row id, the derived slug, the derived name, the captured
  // remote, the key order, the omission rules and the terminator all still
  // under a byte-for-byte diff. Nothing is loosened to a `contains` check.
  auto const scrub = [](std::string text, const std::filesystem::path& root) {
    auto const needle = root.string();
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 5)) {
      text.replace(at, needle.size(), "$ROOT");
    }
    return text;
  };

  // Make `<root>/proj` a git repository with an `origin`. This is the ONE
  // fixture shape in which a handler that never probes git still differs
  // from one that does — see handlers.t.cpp's `[6128]` cases. `upstream` is
  // added FIRST and alphabetically before `origin`, so a probe that took
  // "the first remote" would capture the wrong URL on both sides and the
  // diff would still pass; it is the ORACLE that decides which is right,
  // and it answers `origin`.
  auto const seed_repo = [](const std::filesystem::path& root) -> bool {
    auto const proj = (root / "proj").string();
    auto const line = std::format("git -C '{}' init -q . >/dev/null 2>&1 && "
                                  "git -C '{}' remote add upstream https://example.com/up.git >/dev/null 2>&1 && "
                                  "git -C '{}' remote add origin git@github.com:example/repo.git >/dev/null 2>&1",
                                  proj, proj, proj);
    return std::system(line.c_str()) == 0;
  };

  struct shape {
    std::string_view         tag;      ///< Case discriminator.
    std::vector<std::string> args;     ///< The argv tail.
    bool                     git_repo; ///< Whether to seed a git repository with an `origin` first.
  };
  std::vector<shape> const shapes{
      {"initplain", {"init"}, false},
      {"initjson", {"init", "--json"}, false},
      {"initskip", {"init", "--skip-project"}, false},
      {"initskipj", {"init", "--skip-project", "--json"}, false},
      // Declared, and never read by either binary. Probed rather than
      // assumed: `init` in a non-git directory succeeds WITHOUT it.
      {"initanr", {"init", "--allow-no-repo"}, false},
      {"initnameslug", {"init", "--name", "My Proj", "--slug", "custom-slug", "--json"}, false},
      // THE `git_remote` CASES. Without these the whole column is invisible
      // to this file.
      {"initremote", {"init", "--json"}, true},
      {"initremotetext", {"init"}, true},
  };

  for (auto const& [tag, args, git_repo] : shapes) {
    auto const space = make_arena(tag);
    if (git_repo) {
      if (!seed_repo(space.cpp_root) || !seed_repo(space.zig_root)) {
        WARN("git unavailable — skipping the remote-capture shape " << tag);
        continue;
      }
    }
    auto const mine = run_pinned(cpp_bin(), args, space.cpp_root, tag);
    auto const ref  = run_pinned(zig_bin(), args, space.zig_root, tag);

    INFO("shape: " << tag);
    CHECK(mine.code == ref.code);
    CHECK(scrub(mine.out, space.cpp_root) == scrub(ref.out, space.zig_root));
    CHECK(mine.err == ref.err);

    auto const json_mode = std::ranges::find(args, "--json") != args.end();
    if (git_repo && json_mode) {
      // The scrub cannot hide this one: the remote URL is not a path, so a
      // side that failed to capture it differs from a side that did.
      CHECK(mine.out.contains("git@github.com:example/repo.git"));
      CHECK(ref.out.contains("git@github.com:example/repo.git"));
      CHECK_FALSE(mine.out.contains("up.git"));
    }
    if (git_repo && !json_mode) {
      // FINDING, pinned rather than assumed away. The TEXT renderer never
      // mentions the remote at all — `planar init` in a repository with an
      // `origin` prints exactly what it prints without one, even though the
      // column IS written. `--json` is the only operator-visible signal for
      // `projects.git_remote`, which is precisely why this cycle asserts on
      // the ROW in handlers.t.cpp rather than trusting stdout: a text-only
      // test suite cannot distinguish a captured remote from a dropped one.
      CHECK_FALSE(mine.out.contains("git@github.com:example/repo.git"));
      CHECK_FALSE(ref.out.contains("git@github.com:example/repo.git"));
    }
  }
}

TEST_CASE("C++ and Zig agree on repeated init and on --force", "[cmd][parity][oracle][init]") {
  if (!oracle_available()) {
    SKIP("zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");
  }

  auto const scrub = [](std::string text, const std::filesystem::path& root) {
    auto const needle = root.string();
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 5)) {
      text.replace(at, needle.size(), "$ROOT");
    }
    return text;
  };

  // Ordered against ONE database per binary: the second `init` must see the
  // row the first wrote, which is the whole point. A fresh arena per step
  // would only ever compare first-insert behaviour and the INSERT OR IGNORE
  // rule would go untested.
  struct step {
    std::string_view         tag;  ///< Case discriminator.
    std::vector<std::string> args; ///< The argv tail.
  };
  std::vector<step> const steps{
      {"i1", {"init", "--json"}},
      // Idempotent: `--name` is NOT applied on the second run, because the
      // insert is OR IGNORE. Both binaries must decline it identically.
      {"i2", {"init", "--name", "Ignored", "--json"}},
      // `--force` repoints the SAME row id rather than inserting a second.
      {"i3", {"init", "--force", "--name", "Renamed", "--json"}},
      {"i4", {"init"}},
  };

  auto const space = make_arena("initidem");
  for (auto const& [tag, args] : steps) {
    auto const mine = run_pinned(cpp_bin(), args, space.cpp_root, tag);
    auto const ref  = run_pinned(zig_bin(), args, space.zig_root, tag);

    INFO("step: " << tag);
    CHECK(mine.code == ref.code);
    CHECK(scrub(mine.out, space.cpp_root) == scrub(ref.out, space.zig_root));
    CHECK(mine.err == ref.err);
  }
  // The row id never moved: three registrations, one project.
  CHECK(scrub(read_all(space.cpp_root / "i3.out"), space.cpp_root)
            .contains(R"("project_id":1,"project_slug":"proj","project_name":"Renamed")"));
}
