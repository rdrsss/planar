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
// `src/engine/` moved.
//
// And the question the deleted help-page diffs actually answered — "was
// this tree transcribed from the oracle correctly?" — is now answered
// directly by `every ported command declares what the oracle declares`
// below, off the `schema` catalog. See ../catalog_parity.hpp.
//
// DATABASE SAFETY IS THE WHOLE REASON THE HARNESS IS CAREFUL, and the
// harness now lives in ONE place: ../parity_harness.hpp. This file used to
// carry its own verbatim copy of `run_pinned` / `make_arena` (task 6105,
// before the shared header existed), and that copy is how a second defect
// in the pinned-capture path survived here after being reasoned about
// three times — see task 6198 and the header's own account. Read that file
// before touching any invocation below; the `cd`-then-`env` ordering, the
// per-invocation capture files and the pipe-rather-than-file redirect were
// each a real bug, and the first of them wrote rows into the operator's
// live ~/.planar/planar.db.

#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>

import std;
import cli11;
import planar.cliapp.schema;
import planar.cmd.planar.main;
import planar.db;
import planar.db.migrate;

#include "catalog_parity.hpp"
#include "parity_harness.hpp"

namespace {

// The harness — `capture`, `run_pinned`, `arena`, `make_arena` — lives in
// ../parity_harness.hpp, which the other three binaries' parity files
// already share. This file carried its own verbatim copy from task 6105,
// which is how a SECOND defect in the pinned-capture path survived in it:
// the file-redirect form corrupts any Zig invocation that writes a stream
// twice (found by task 6198; see that header). One copy, one fix.
using ::planar::cmd::parity::arena;
using ::planar::cmd::parity::capture;
using ::planar::cmd::parity::make_arena;
using ::planar::cmd::parity::read_all;
using ::planar::cmd::parity::run_pinned;
using ::planar::cmd::parity::shell_quote;

/// @brief Path to the built C++ binary (set by this target's CMakeLists).
/// @return The path.
auto cpp_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

/// @brief Remove one command object from a compact `planar schema` catalog
/// document, by its exact `path` array.
///
/// Plan 996, task 6419: `ext`/`sync` moved to `planar-ext`, so the oracle's
/// `schema` document still declares 10 leaves this binary's own no longer
/// does (decision 999 — this move owes no oracle parity). Byte-identity
/// between the two is only recoverable by excising exactly those 10
/// objects from the ORACLE side first. `schema.cpp`'s `render_command`
/// composes each command as `{"path":[...` and joins the array with plain
/// commas, so the object is found by that exact prefix and removed with
/// its ONE separating comma — whichever side of it exists (the first
/// command in the array has none before it).
/// @param catalog The full `planar schema` JSON document.
/// @param path The command's path segments, e.g. `{"ext","register","jira"}`.
/// @return The document with that one command object removed.
auto strip_command(std::string catalog, std::initializer_list<std::string_view> path) -> std::string {
  // The oracle's own command objects open `{"name":...` — `"path"` is a
  // LATER key, not the first — so the anchor is the `"path":[...]` array
  // itself, and the object's opening `{` is found by scanning BACKWARD to
  // the nearest one, string-aware in both directions the same way the
  // forward close-scan below is: JSON does not require escaping `{`/`}`
  // inside a string, and command descriptions do contain literal braces
  // (JSON examples in prose), so a scan that does not track string
  // boundaries can close on the wrong byte.
  std::string needle = R"("path":[)";
  for (auto const& segment : path) {
    if (&segment != path.begin()) {
      needle += ",";
    }
    needle += "\"" + std::string{segment} + "\"";
  }
  needle += "]";

  auto const anchor = catalog.find(needle);
  REQUIRE(anchor != std::string::npos);

  auto const start = catalog.rfind("{\"name\"", anchor);
  REQUIRE(start != std::string::npos);

  int         depth     = 0;
  bool        in_string = false;
  bool        escaped   = false;
  std::size_t end       = std::string::npos;
  for (std::size_t i = start; i < catalog.size(); ++i) {
    char const c = catalog[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == '{') {
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth == 0) {
        end = i;
        break;
      }
    }
  }
  REQUIRE(end != std::string::npos);

  if (start > 0 && catalog[start - 1] == ',') {
    catalog.erase(start - 1, end - (start - 1) + 1);
  } else {
    // The first command in the array: no leading comma, but a trailing one
    // if it is not also the LAST command.
    auto erase_end = end + 1;
    if (erase_end < catalog.size() && catalog[erase_end] == ',') {
      ++erase_end;
    }
    catalog.erase(start, erase_end - start);
  }
  return catalog;
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

  // THE WORKBENCH ROOT IS A SECOND, INDEPENDENT ESCAPE (task 6305). It
  // resolves `$PLANAR_WORKBENCH_ROOT` → config → `~/.planar/workbench`, so
  // before that variable was pinned a workbench case was contained only by
  // `HOME` also being redirected — incidental containment that a reorder of
  // the env map silently removes. Both halves are asserted, because the
  // absence half alone passes for the wrong reason: a `workbench push` that
  // failed outright writes nothing to EITHER location.
  auto const seeded = std::to_array<std::vector<std::string>>({
      {"init", "--name", "envpin", "--slug", "envpin"},
      {"assoc", "create", "project:envpin", "--kind", "project"},
      {"assoc", "add", "project:envpin", (space.cpp_root / "proj").string()},
      {"plan", "create", "Env Pin", "--slug", "env-pin", "--summary", "Probe."},
  });
  for (std::size_t i = 0; i < seeded.size(); ++i) {
    auto const ran = run_pinned(cpp_bin(), seeded[i], space.cpp_root, std::format("envpinseed{}", i));
    INFO("seed step " << i << " -> " << ran.err);
    REQUIRE(ran.code == 0);
  }
  auto const pushed = run_pinned(cpp_bin(), std::array<std::string, 3>{"workbench", "push", "1"}, space.cpp_root, "envpinwb");
  INFO("workbench push -> " << pushed.err);
  REQUIRE(pushed.code == 0);
  // PRESENT: the pinned root received the tree.
  CHECK(std::filesystem::is_directory(space.cpp_root / "workbench"));
  CHECK_FALSE(std::filesystem::is_empty(space.cpp_root / "workbench"));
  // ABSENT: neither fallback location was reached.
  CHECK_FALSE(std::filesystem::exists(space.cpp_root / "fakehome" / ".planar" / "workbench"));

  // TASK 6542 RETIREMENT: this probe was never a comparison — the oracle
  // half above only re-ran the exact same assertions against `zig_bin()`,
  // which is a second instance of the same env-pinning check, not a
  // byte-for-byte diff. The C++ assertions above already stand alone (the
  // comment that used to sit here said so), so there is nothing left to
  // transcribe: the case runs entirely against the C++ binary now and needs
  // no oracle to be complete.
}

TEST_CASE("the no-database leaves are pinned", "[cmd][parity][cli-surface]") {
  // TASK 6542 RETIREMENT: the six leaves below are HANDLER-level output,
  // not parser output, so task 6123 left them as a live oracle diff. But
  // that diff has now been captured and confirmed byte-identical (built
  // C++ binary vs. the zig/zig-out/bin/planar oracle at this commit), and
  // the established pattern from `the CLI surface is CLI11's now, and
  // pinned` applies here too: an agreement, once confirmed, is preserved
  // by pinning the C++ side rather than re-diffing it forever. Runs
  // without the oracle.
  struct leaf {
    std::string_view         tag;  ///< Case discriminator.
    std::vector<std::string> args; ///< The argv tail.
    int                      code; ///< Expected exit code.
    std::string_view         out;  ///< Expected stdout, transcribed from the oracle.
    std::string_view         err;  ///< Expected stderr, transcribed from the oracle.
  };
  std::vector<leaf> const leaves{
      {"wl", {"workflow", "list"}, 0, "no shipped + sandbox workflows found\n", ""},
      {"wlj", {"workflow", "list", "--json"}, 0, "", ""},
      {"wll", {"workflow", "list", "--local"}, 0, "no sandbox workflows found\n", ""},
      {"wsmiss", {"workflow", "show", "nope"}, 1, "", "error: workflow 'nope' not found\n"},
      // The additive --json error envelope (decision 1145, task 6844) is
      // the only thing on stdout here; stderr is unchanged.
      {"wsmissj",
       {"workflow", "show", "nope", "--json"},
       1,
       R"({"error":{"verb":"workflow show","tag":"generic_failure"}})"
       "\n",
       "error: workflow 'nope' not found\n"},
      // A HANDLER-level refusal, not a parser one: still a true oracle
      // match after task 6123, and deliberately kept here as the control
      // showing the re-baselining is confined to parser- and help-produced
      // bytes.
      {"aareq", {"annotate", "add"}, 2, "", "error: --anchor-path is required\n"},
      // The nine `--help` / parse-failure shapes that used to live in this
      // list moved to `the CLI surface is CLI11\'s now, and pinned` below —
      // CLI11 renders help and writes parse errors as of task 6123, so
      // those bytes are no longer oracle-comparable. They are pinned
      // against the built binary rather than dropped or loosened.
  };

  for (auto const& leaf : leaves) {
    auto const space = make_arena(leaf.tag);
    auto const mine  = run_pinned(cpp_bin(), leaf.args, space.cpp_root, leaf.tag);

    INFO("leaf: " << leaf.tag);
    CHECK(mine.code == leaf.code);
    CHECK(mine.out == leaf.out);
    CHECK(mine.err == leaf.err);
  }
}

TEST_CASE("the database-backed leaves are pinned", "[cmd][parity][cli-surface]") {
  // TASK 6542 RETIREMENT: confirmed byte-identical against the oracle at
  // this commit (both wall-clock `created:`/`updated:` lines aside, which
  // could never agree across two processes and are dropped from the
  // captured expectation the same way the live diff used to drop them
  // before comparing). Pinned against the C++ binary alone from here;
  // see `the CLI surface is CLI11's now, and pinned` for the pattern.
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
    int                      code; ///< Expected exit code.
    std::string_view         out;  ///< Expected stdout (timestamp lines already stripped).
    std::string_view         err;  ///< Expected stderr.
  };
  // Run as an ordered sequence against ONE database, so the second
  // `annotate list` sees the row the `annotate add` before it wrote — a
  // fresh arena per step would only ever compare empty states.
  std::vector<step> const steps{
      {"al0", {"annotate", "list"}, 0, "(no annotations)\n\n", ""},
      {"al0j", {"annotate", "list", "--json"}, 0, "[]\n\n", ""},
      {"add1",
       {"annotate", "add", "--anchor-path", "src/foo.zig", "--line-start", "3", "--line-end", "7", "--title", "T1", "--body",
        "B1", "--tags", "a, b,a"},
       0,
       "id:          1\n"
       "title:       T1\n"
       "status:      active\n"
       "scope:       global\n"
       "anchor path: src/foo.zig\n"
       "anchor line: 3-7\n"
       "tags:        a, b\n"
       "body:        B1\n\n",
       ""},
      {"al1", {"annotate", "list"}, 0, "    1  active      T1\n\n", ""},
      {"badstatus", {"annotate", "list", "--status", "bogus"}, 1, "", "error: unknown status 'bogus'\n"},
  };

  auto const space = make_arena("dbleaves");
  for (auto const& step : steps) {
    auto const mine = run_pinned(cpp_bin(), step.args, space.cpp_root, step.tag);

    INFO("step: " << step.tag);
    CHECK(mine.code == step.code);
    CHECK(strip_timestamps(mine.out) == step.out);
    CHECK(mine.err == step.err);
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

TEST_CASE("the task-6106 no-fixture leaves are pinned", "[cmd][parity][cli-surface]") {
  // TASK 6542 RETIREMENT: confirmed byte-identical against the oracle at
  // this commit. Pinned against the C++ binary alone from here; see
  // `the CLI surface is CLI11's now, and pinned` for the pattern.
  struct leaf {
    std::string_view         tag;  ///< Case discriminator.
    std::vector<std::string> args; ///< The argv tail.
    int                      code; ///< Expected exit code.
    std::string_view         out;  ///< Expected stdout.
    std::string_view         err;  ///< Expected stderr.
  };
  std::vector<leaf> const leaves{
      // `skills`, `skills --help`, `skills extra`, `unlink --help`,
      // `unlink` (no positional) and `workspace doctor --help` used to be
      // here. All six are help pages or parse failures, i.e. CLI11-produced
      // as of task 6123; they moved to the pinned-bytes case below.
      // Empty database, both render modes. The text mode is ZERO BYTES
      // where `--json` is `{"orgs":[]}` — a disagreement inside one leaf.
      {"wd", {"workspace", "doctor"}, 0, "", ""},
      {"wdj", {"workspace", "doctor", "--json"}, 0, "{\"orgs\":[]}\n", ""},
      // The id parser, end to end. `1_0` and `007` reach the not-found path
      // naming the PARSED value; `_10` and the overflow are refusals.
      {"ulmiss", {"unlink", "999"}, 1, "", "error: link 999 not found\n"},
      // The additive --json error envelope (decision 1145, task 6844) is
      // the only thing on stdout here; stderr is unchanged.
      {"ulmissj",
       {"unlink", "999", "--json"},
       1,
       R"({"error":{"verb":"unlink","tag":"generic_failure"}})"
       "\n",
       "error: link 999 not found\n"},
      {"ulsep", {"unlink", "1_0"}, 1, "", "error: link 10 not found\n"},
      {"ulpad", {"unlink", "007"}, 1, "", "error: link 7 not found\n"},
      {"ulplus", {"unlink", "+12"}, 1, "", "error: link 12 not found\n"},
      {"ulbad", {"unlink", "abc"}, 2, "", "error: invalid link id 'abc'\n"},
      {"ullead", {"unlink", "_10"}, 2, "", "error: invalid link id '_10'\n"},
      {"ultrail", {"unlink", "10_"}, 2, "", "error: invalid link id '10_'\n"},
      {"ulhex", {"unlink", "0x10"}, 2, "", "error: invalid link id '0x10'\n"},
      {"ulovf", {"unlink", "9223372036854775808"}, 2, "", "error: invalid link id '9223372036854775808'\n"},
      {"ulmax", {"unlink", "9223372036854775807"}, 1, "", "error: link 9223372036854775807 not found\n"},
  };

  for (auto const& leaf : leaves) {
    auto const space = make_arena(leaf.tag);
    auto const mine  = run_pinned(cpp_bin(), leaf.args, space.cpp_root, leaf.tag);

    INFO("leaf: " << leaf.tag);
    CHECK(mine.code == leaf.code);
    CHECK(mine.out == leaf.out);
    CHECK(mine.err == leaf.err);
  }
}

// "C++ and Zig agree byte-for-byte on the three ported ext leaves" removed
// at plan 996, task 6419 (decision 999): `ext register`/`ext list` moved to
// `planar-ext`, which has no Zig oracle counterpart to diff against. See
// `src/cmd/planar-ext/ext_leaves.t.cpp` and `ext_create_leaf.t.cpp` for their
// C++-only coverage now.

TEST_CASE("unlink over a seeded external link is pinned", "[cmd][parity][cli-surface]") {
  // TASK 6542 RETIREMENT: `init` and `link` are ported to this binary; only
  // `ext register` moved to `planar-ext` (decision 999). So the fixture no
  // longer needs the oracle at all — it is seeded by `cpp_bin()` and its
  // sibling `planar-ext` binary, which both write the same on-disk
  // database and were confirmed byte-identical to the oracle's own seed
  // sequence at this commit. `planar-ext` has no `PLANAR_*_BIN` define of
  // its own in this target (this file may only touch itself, not
  // CMakeLists.txt), so it is found next to `cpp_bin()` — the two binaries
  // are always built into the same output directory.
  auto const ext_bin = cpp_bin().parent_path() / "planar-ext";
  REQUIRE(std::filesystem::exists(ext_bin));

  auto const space = make_arena("ulseed");
  auto const init  = run_pinned(cpp_bin(), std::array<std::string, 1>{"init"}, space.cpp_root, "seed0");
  REQUIRE(init.code == 0);
  auto const reg = run_pinned(ext_bin, std::vector<std::string>{"ext", "register", "github", "gh", "--project", "owner/repo"},
                              space.cpp_root, "seed1");
  REQUIRE(reg.code == 0);
  // The two plans must EXIST. Before task 6314 `link` validated only the
  // entity KIND, so this fixture linked `plan:1` / `plan:2` into an arena
  // that had never created a plan and still got exit 0. The verb now
  // resolves the row, so the seed creates them.
  auto const plan1 = run_pinned(cpp_bin(), std::vector<std::string>{"plan", "create", "Seed one", "--scope", "global"},
                                space.cpp_root, "seedp1");
  REQUIRE(plan1.code == 0);
  auto const plan2 = run_pinned(cpp_bin(), std::vector<std::string>{"plan", "create", "Seed two", "--scope", "global"},
                                space.cpp_root, "seedp2");
  REQUIRE(plan2.code == 0);

  auto const link1 =
      run_pinned(cpp_bin(), std::vector<std::string>{"link", "plan:1", "--to", "gh:42", "--json"}, space.cpp_root, "seed2");
  INFO("link1 stderr: " << link1.err);
  REQUIRE(link1.code == 0);
  auto const link2 =
      run_pinned(cpp_bin(), std::vector<std::string>{"link", "plan:2", "--to", "gh:43", "--json"}, space.cpp_root, "seed3");
  REQUIRE(link2.code == 0);

  struct step {
    std::string_view         tag;  ///< Case discriminator.
    std::vector<std::string> args; ///< The argv tail.
    int                      code; ///< Expected exit code.
    std::string_view         out;  ///< Expected stdout.
    std::string_view         err;  ///< Expected stderr.
  };
  // Ordered against ONE database: the second `unlink 1` only reaches the
  // not-found path because the first one succeeded.
  std::vector<step> const steps{
      {"ul1", {"unlink", "1"}, 0, "unlinked: external link 1 removed\n", ""},
      {"ul2j", {"unlink", "2", "--json"}, 0, "{\"ok\":true,\"id\":2}\n", ""},
      {"ul1again", {"unlink", "1"}, 1, "", "error: link 1 not found\n"},
  };
  for (auto const& step : steps) {
    auto const mine = run_pinned(cpp_bin(), step.args, space.cpp_root, step.tag);
    INFO("step: " << step.tag);
    CHECK(mine.code == step.code);
    CHECK(mine.out == step.out);
    CHECK(mine.err == step.err);
  }

  // The audit trail is part of the contract and is invisible in stdout, so
  // it is read back out of the database and pinned too. No verb renders
  // `session_entries` (there is no `capture show`), so the rows are read
  // directly. Without this, a port that dropped the `session_entries`
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
  CHECK(entries(space.cpp_root) == "cli|action|unlink: removed external link 1\n"
                                   "cli|action|unlink: removed external link 2\n");
}

// "C++ and Zig agree on the three sync write leaves over seeded links"
// removed at plan 996, task 6419 (decisions 996, 999): `sync pull`/`push`/
// `resolve` moved to `planar-ext`, and decision 996 makes `sync pull` a
// deliberate, recorded divergence from the oracle (it no longer applies the
// remote to local planning tables) -- there is no longer a byte-identical
// oracle shape to pin here. See `src/cmd/planar-ext/sync_leaves.t.cpp`.

TEST_CASE("promote, demote and test-spec status are pinned", "[cmd][parity][cli-surface]") {
  // TASK 6542 RETIREMENT: every seed verb here is ported, so the fixture is
  // seeded by `cpp_bin()` alone now — confirmed to produce the same ids and
  // scope graph as the oracle's own seed sequence at this commit. Every
  // step below was confirmed byte-identical to the oracle first; see
  // `the CLI surface is CLI11's now, and pinned` for the pattern.
  //
  // TWO associations, and that is load-bearing: with one, `promote` can
  // only ever move a row in from global or report `scope_unchanged`, and
  // the association-to-association arm — the only one whose `--json`
  // envelope carries a non-null `previous_scope_id` — is unreachable.
  auto const space = make_arena("promotion");
  auto const seed  = std::to_array<std::vector<std::string>>({
      {"init", "--name", "Proj", "--slug", "proj", "--allow-no-repo", "--json"},
      {"assoc", "create", "project:proj", "--kind", "project", "--json"},
      {"assoc", "create", "org:acme", "--kind", "org", "--json"},
      {"plan", "create", "Anchor", "--slug", "anchor", "--scope", "project:proj", "--json"},
      {"plan", "create", "M1 Foundation", "--slug", "m1", "--parent", "1", "--scope", "project:proj", "--json"},
      {"task", "add", "T one", "--plan", "2", "--slug", "t-one", "--no-editor", "--json"},
  });
  for (std::size_t i = 0; i < seed.size(); ++i) {
    auto const tag = std::format("pseed{}", i);
    auto const ran = run_pinned(cpp_bin(), seed[i], space.cpp_root, tag);
    INFO("seed step: " << tag);
    REQUIRE(ran.code == 0);
  }

  struct step {
    std::string_view         tag;  ///< Case discriminator.
    std::vector<std::string> args; ///< The argv tail.
    int                      code; ///< Expected exit code.
    std::string_view         out;  ///< Expected stdout.
    std::string_view         err;  ///< Expected stderr.
  };
  // ORDER IS LOAD-BEARING for the mutating steps: each `promote`/`demote`
  // observes the scope the previous one left behind, which is what makes
  // the `scope_unchanged` and already-global arms reachable at all.
  std::vector<step> const steps{
      // --- refusals, before any success ---
      {"pref1", {"promote", "plan", "--to", "org:acme"}, 2, "", "error: invalid ref 'plan': expected kind:id\n"},
      {"pref2", {"promote", "plan:", "--to", "org:acme"}, 2, "", "error: invalid ref 'plan:': expected kind:id\n"},
      {"pref3", {"promote", ":1", "--to", "org:acme"}, 2, "", "error: invalid ref ':1': expected kind:id\n"},
      {"pslug",
       {"promote", "plan:some-slug", "--to", "org:acme"},
       2,
       "",
       "error: promote requires a numeric id (got slug 'plan:some-slug')\n"},
      // The two bad-kind arms, which land in different buckets: `bogus` is
      // not an `entity_kind` (exit 2, parse_ref), `session` is one but is
      // not promotable (exit 1, the pre-read).
      {"pbogus", {"promote", "bogus:1", "--to", "org:acme"}, 2, "", "error: invalid ref 'bogus:1': expected kind:id\n"},
      {"psess", {"promote", "session:1", "--to", "org:acme"}, 1, "", "error: reading entity scope: InvalidScope\n"},
      // Non-positive ids are MALFORMED, not absent — a different code and a
      // different message from `plan:999`.
      {"pzero", {"promote", "plan:0", "--to", "org:acme"}, 2, "", "error: invalid ref 'plan:0': expected kind:id\n"},
      {"pneg", {"promote", "plan:-1", "--to", "org:acme"}, 2, "", "error: invalid ref 'plan:-1': expected kind:id\n"},
      {"pabsent", {"promote", "plan:999", "--to", "org:acme"}, 1, "", "error: reading entity scope: NotFound\n"},
      {"prepo", {"promote", "plan:1", "--to", "repo:proj"}, 1, "", "error: repo: scopes are not supported\n"},
      {"punk", {"promote", "plan:1", "--to", "nonexistent"}, 1, "", "error: no association with slug 'nonexistent'\n"},
      {"psame", {"promote", "plan:1", "--to", "project:proj"}, 1, "", "error: plan:1 is already at scope 'project:proj'\n"},
      {"dref", {"demote", "plan"}, 2, "", "error: invalid ref 'plan': expected kind:id\n"},
      {"dslug", {"demote", "plan:some-slug"}, 2, "", "error: demote requires a numeric id (got slug 'plan:some-slug')\n"},
      {"dabsent", {"demote", "plan:999"}, 1, "", "error: reading entity scope: NotFound\n"},
      // --- successes, walking one row around the scope graph ---
      {"pmove",
       {"promote", "plan:1", "--to", "org:acme"},
       0,
       "plan:1 promoted to association org:acme  (was: association:1)\n",
       ""},
      {"pmovej",
       {"promote", "plan:1", "--to", "project:proj", "--json"},
       0,
       "{\"ok\":true,\"kind\":\"plan\",\"id\":1,\"scope_kind\":\"association\",\"scope_id\":1,\"previous_scope_kind\":"
       "\"association\","
       "\"previous_scope_id\":2}\n",
       ""},
      {"ddown",
       {"demote", "plan:1", "--json"},
       0,
       "{\"ok\":true,\"kind\":\"plan\",\"id\":1,\"scope_kind\":\"global\",\"scope_id\":null,\"previous_scope_kind\":"
       "\"association\","
       "\"previous_scope_id\":1}\n",
       ""},
      {"dagain", {"demote", "plan:1"}, 1, "", "error: plan:1 is already at global scope\n"},
      // `--from` is read and DISCARDED: a slug that does not exist is not a
      // refusal, because the engine call takes no source scope.
      {"dfrom", {"demote", "plan:1", "--from", "nonexistent-slug"}, 1, "", "error: plan:1 is already at global scope\n"},
      {"pback", {"promote", "plan:1", "--to", "org:acme"}, 0, "plan:1 promoted to association org:acme  (was: global)\n", ""},
      {"ptask",
       {"promote", "task:1", "--to", "org:acme", "--json"},
       0,
       "{\"ok\":true,\"kind\":\"task\",\"id\":1,\"scope_kind\":\"association\",\"scope_id\":2,\"previous_scope_kind\":\"global\","
       "\"previous_scope_id\":null}\n",
       ""},
      {"dtask", {"demote", "task:1", "--from", "org:acme"}, 0, "task:1 demoted to global  (was: association:2)\n", ""},
      // --- test-spec status: a real MILESTONE reports "not found" ---
      {"tsmid", {"test-spec", "status", "2"}, 1, "", "error: plan '2' not found\n"},
      {"tsmslug", {"test-spec", "status", "m1"}, 1, "", "error: plan 'm1' not found\n"},
      {"tsabsent", {"test-spec", "status", "999"}, 1, "", "error: plan '999' not found\n"},
      {"tsnope", {"test-spec", "status", "nope"}, 1, "", "error: plan 'nope' not found\n"},
      {"tsid",
       {"test-spec", "status", "1"},
       0,
       "test-spec status for plan 1 (anchor)\n"
       "  milestone                        tasks  slug   cov   happy empty error  edge other\n"
       "  Anchor                              +0    +0    +0      +0    +0    +0    +0    +0\n"
       "\n"
       "  0 scenarios total; 0 of 0 slug-bearing tasks covered (0 total tasks).\n",
       ""},
      {"tsidj",
       {"test-spec", "status", "1", "--json"},
       0,
       "{\"plan_id\":1,\"title\":\"Anchor\",\"total_tasks\":0,\"tasks_with_slug\":0,\"tasks_covered\":0,\"happy\":0,\"empty\":0,"
       "\"error\":0,\"edge\":0,\"other\":0}\n"
       "{\"anchor_plan_id\":1,\"total_tasks\":0,\"tasks_with_slug\":0,\"tasks_covered\":0,\"total_scenarios\":0}\n",
       ""},
      {"tsslug",
       {"test-spec", "status", "anchor"},
       0,
       "test-spec status for plan 1 (anchor)\n"
       "  milestone                        tasks  slug   cov   happy empty error  edge other\n"
       "  Anchor                              +0    +0    +0      +0    +0    +0    +0    +0\n"
       "\n"
       "  0 scenarios total; 0 of 0 slug-bearing tasks covered (0 total tasks).\n",
       ""},
      {"tsslugj",
       {"test-spec", "status", "anchor", "--json"},
       0,
       "{\"plan_id\":1,\"title\":\"Anchor\",\"total_tasks\":0,\"tasks_with_slug\":0,\"tasks_covered\":0,\"happy\":0,\"empty\":0,"
       "\"error\":0,\"edge\":0,\"other\":0}\n"
       "{\"anchor_plan_id\":1,\"total_tasks\":0,\"tasks_with_slug\":0,\"tasks_covered\":0,\"total_scenarios\":0}\n",
       ""},
  };
  for (auto const& step : steps) {
    auto const mine = run_pinned(cpp_bin(), step.args, space.cpp_root, step.tag);
    INFO("step: " << step.tag);
    CHECK(mine.code == step.code);
    CHECK(mine.out == step.out);
    CHECK(mine.err == step.err);
  }

  // `promote` and `demote` write `scope_kind`/`scope_id`, and the stream
  // above renders those columns only through the handler's own envelope.
  // Without this a port that printed the right envelope while writing the
  // wrong row — or writing nothing — would pass every assertion above.
  auto const scopes = [](const std::filesystem::path& root) {
    auto conn = planar::db::connection::open((root / "planar.db").string());
    REQUIRE(conn.has_value());
    auto stmt = conn->prepare("select 'plan', id, scope_kind, coalesce(scope_id,-1) from plans"
                              " union all select 'task', id, scope_kind, coalesce(scope_id,-1) from tasks"
                              " order by 1, 2");
    REQUIRE(stmt.has_value());
    std::string rendered;
    for (;;) {
      auto stepped = stmt->step();
      REQUIRE(stepped.has_value());
      if (*stepped == planar::db::step_result::done) {
        break;
      }
      rendered +=
          std::format("{}:{}|{}|{}\n", stmt->column_text(0), stmt->column_int64(1), stmt->column_text(2), stmt->column_int64(3));
    }
    return rendered;
  };
  CHECK(scopes(space.cpp_root) == "plan:1|association|2\n"
                                  "plan:2|association|1\n"
                                  "task:1|global|-1\n");
}

TEST_CASE("workspace doctor's diagnose-and-repair pass is pinned", "[cmd][parity][cli-surface]") {
  // TASK 6542 RETIREMENT: confirmed byte-identical against the oracle at
  // this commit, after normalizing each side's own arena root to
  // `<ARENA>` — the arena path is random per run (`make_arena` keys it off
  // `steady_clock`), so it is the one thing this case could never pin
  // literally, oracle or not. Pinned against the C++ binary alone from
  // here; see `the CLI surface is CLI11's now, and pinned` for the pattern.
  //
  // `workspace init` refuses without child directories containing `.git`,
  // and building one of those just to reach `doctor` would make the
  // fixture about `init`. The org row is inserted directly instead —
  // through `planar.db`, into the arena's own scratch database, with the
  // arena's own `proj/` as the recorded `root_path`.
  auto const space = make_arena("wdorg");
  {
    auto conn = planar::db::connection::open((space.cpp_root / "planar.db").string());
    REQUIRE(conn.has_value());
    REQUIRE(planar::db::apply_all(*conn).has_value());
    auto stmt = conn->prepare("insert into associations (slug, name, kind, config_json) values ('acme','acme','org',?)");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->bind_text(1, std::format(R"({{"root_path":"{}"}})", (space.cpp_root / "proj").string())).has_value());
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

  auto const first = run_pinned(cpp_bin(), std::array<std::string, 2>{"workspace", "doctor"}, space.cpp_root, "wdrepair");
  CHECK(first.code == 0);
  CHECK(normalize(first.out, space.cpp_root) ==
        "fix: created state dir <ARENA>/home/workspaces/1\n"
        "missing: <ARENA>/home/workspaces/1/AGENTS.md (run `planar workspace regenerate` after M3)\n"
        "missing: <ARENA>/home/workspaces/1/routing-table.json (run `planar workspace regenerate` after M3)\n"
        "fix: reinstalled symlink <ARENA>/proj/AGENTS.md → <ARENA>/home/workspaces/1/AGENTS.md\n"
        "fix: reinstalled symlink <ARENA>/proj/CLAUDE.md → <ARENA>/home/workspaces/1/AGENTS.md\n"
        "org:acme repaired 5 issues\n");
  CHECK(first.err.empty());

  // Second pass: the state directory now exists, so the `fix` line is
  // gone and the issue count drops. Running it twice is what proves the
  // first pass REPAIRED rather than merely reported.
  auto const second =
      run_pinned(cpp_bin(), std::array<std::string, 3>{"workspace", "doctor", "--json"}, space.cpp_root, "wdsecond");
  CHECK(second.code == 0);
  CHECK(normalize(second.out, space.cpp_root) ==
        "{\"orgs\":[{\"slug\":\"acme\",\"org_id\":1,\"issues_found\":2,\"issues_repaired\":[{\"kind\":\"missing\",\"detail\":"
        "\"<ARENA>/home/workspaces/1/AGENTS.md (run `planar workspace regenerate` after M3)\"},{\"kind\":\"missing\","
        "\"detail\":\"<ARENA>/home/workspaces/1/routing-table.json (run `planar workspace regenerate` after M3)\"}]}]}\n");
  CHECK(second.err.empty());

  // The repair reached the filesystem.
  CHECK(std::filesystem::exists(space.cpp_root / "home" / "workspaces" / "1"));
  CHECK(std::filesystem::is_symlink(space.cpp_root / "proj" / "AGENTS.md"));
  CHECK(std::filesystem::is_symlink(space.cpp_root / "proj" / "CLAUDE.md"));
}

TEST_CASE("every ported command declares what it should, self-consistently", "[cmd][parity][cli-surface][catalog]") {
  // TASK 6542 RETIREMENT. This case used to structurally diff this
  // binary's catalog against the oracle's (task 6123 / task 6065's
  // history is unchanged, see below); that comparison has now been
  // confirmed and is preserved as a full BYTE pin in the sibling case
  // immediately below (`the schema catalog is pinned, whole`), which is
  // the stronger of the two assertions — byte identity implies every
  // structural fact this case used to check via `diff_against_oracle`
  // and `oracle_only_commands`. What is left here is what does NOT need
  // the oracle at all: parsing this binary's OWN declared catalog and
  // asserting the self-consistent facts about it — non-vacuity checks
  // that a summary (a bare byte-length check, say) would not catch.
  // Runs without the oracle.
  //
  // TASK 6123. This case originally replaced "C++ and Zig agree
  // byte-for-byte on the workbench leaves' help pages", which diffed ten
  // rendered pages against the oracle. That diff existed to catch a
  // half-ported leaf — dropping `--filter-mode` from `archive` because
  // the engine ignores it, say — and a rendered page was the observable
  // proxy for the declaration. CLI11 renders help now, so the proxy is
  // gone; the declaration is not. See ../catalog_parity.hpp for the full
  // argument.
  auto const space  = make_arena("catalog");
  auto const actual = run_pinned(cpp_bin(), std::array<std::string, 1>{"schema"}, space.cpp_root, "cpp");
  REQUIRE(actual.code == 0);

  auto const mine = planar::cmd::parity::parse_catalog(actual.out);
  REQUIRE(mine.has_value());

  // Non-vacuous: an empty catalog would pass trivially, and the named
  // entries below are the ones whose declarations task 6123/6065 most
  // easily could have got wrong.
  //
  // 247, confirmed against the oracle (after its 14 moved `ext`/`sync`
  // entries are excised — decision 999, task 6419) at this commit; see
  // the byte pin below for the whole document this count is drawn from.
  // Task 6939 adds the `document` parent plus its two leaves.
  // Task 7205 adds the `help` leaf.
  CHECK(mine->size() == 256);
  CHECK(mine->contains("planar help"));
  CHECK(mine->contains("planar document"));
  CHECK(mine->contains("planar document project"));
  CHECK(mine->contains("planar document validate-range"));
  CHECK(mine->contains("planar workbench gc"));
  CHECK(mine->contains("planar workbench archive"));
  CHECK(mine->contains("planar annotate add"));
  CHECK(mine->contains("planar annotate capabilities"));
  CHECK(mine->contains("planar task facts stage"));
  CHECK(mine->contains("planar unlink"));
  // Four the tree did not carry at all before task 6065: a deep leaf, a
  // dual node, and the two tree-describing verbs that task implemented.
  CHECK(mine->contains("planar plan next"));
  CHECK(mine->contains("planar health"));
  CHECK(mine->contains("planar schema"));
  CHECK(mine->contains("planar completion"));
  // The 10 moved LEAVES (not counting the 4 group/deferred entries above)
  // are GENUINELY absent from this binary's own catalog — decision 999,
  // task 6419 moved them to `planar-ext`.
  for (auto const& moved :
       {"planar ext register jira", "planar ext register github", "planar ext list", "planar ext test", "planar ext create",
        "planar ext propagate-one", "planar sync pull", "planar sync push", "planar sync status", "planar sync resolve"}) {
    INFO("moved to planar-ext at task 6419: " << moved);
    CHECK_FALSE(mine->contains(moved));
  }
}

TEST_CASE("the schema catalog is pinned, whole", "[cmd][parity][cli-surface][catalog]") {
  // TASK 6542 RETIREMENT. This case used to be titled "all three
  // catalogs are byte-identical to the oracle's", diffing this binary's
  // `planar schema` output against the oracle's with the 14 `ext`/`sync`
  // entries excised from the ORACLE side (decision 999, task 6419) and
  // two subcommand-list renames applied. That comparison is CONFIRMED
  // byte-identical at this commit, so the golden value below IS that
  // confirmed oracle-derived catalog, transcribed whole rather than
  // summarized — a size check or a spot-check of a few flags would miss
  // exactly the kind of single-command regression this case exists to
  // catch. Runs without the oracle.
  //
  // History carried forward from the pre-retirement case: task 6130
  // closed the four-empty-string-default residue (see
  // `planar.cliapp.schema`'s three-argument `schema_json` and
  // `planar.cmd.planar.surface::surface_empty_string_defaults`), and
  // task 6138 is why every bool flag declares a `--no-X` negation — both
  // are baked into the pinned bytes below, not re-derived.
  auto const space  = make_arena("catalogbytes");
  auto const actual = run_pinned(cpp_bin(), std::array<std::string, 1>{"schema"}, space.cpp_root, "cpp");
  REQUIRE(actual.code == 0);
  REQUIRE(actual.out.size() > 300000); // Not an empty string.

  // clang-format off
  CHECK(actual.out == R"CATALOG6542({"schemaVersion":1,"layout":"flat","root":"planar","commands":[{"name":"planar","aliases":[],"hidden":false,"deprecated":null,"path":[],"command":"planar","summary":"Planning + agent operations CLI.","description":"Planning + agent operations CLI.\n\n  This binary writes planning entities (plans, tasks, decisions,\n  questions, artifacts, annotations) and makes manual tasks.status\n  transitions. The agent-coordination tables (agent_actions,\n  agent_work_claims, routing_dispatch_*, and the host-queue tables) are\n  written by planar-agent; the external-link tables are written by\n  planar-ext.","subcommands":["init","scope","assoc","plan","task","question","scenario","decision","artifact","document","annotate","promote","demote","workbench","workspace","link","unlink","links","resume","handoff","capture","audit","health","models","dashboard","spec","test-spec","config","templates","tree","search","local","skills","import","synthesize","version","completion","schema","report","bench","closure","run","groups","explore","workflow","feedback","help"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"init","aliases":[],"hidden":false,"deprecated":null,"path":["init"],"command":"planar init","summary":"Initialize the Planar database and register the current directory as a project.","description":"Initialize the Planar database and register the current directory as a project.","subcommands":[],"flags":[{"long":"--name","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Project name (defaults to repo dir)","completion":{"kind":"none","values":[]},"env":null},{"long":"--slug","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Explicit project slug (defaults to a slug derived from the directory name); with --force, targets that registration for repoint","completion":{"kind":"none","values":[]},"env":null},{"long":"--skip-project","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Only init the DB; skip project registration","completion":{"kind":"none","values":[]},"env":null},{"long":"--allow-no-repo","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Allow initialization outside a git repo","completion":{"kind":"none","values":[]},"env":null},{"long":"--force","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Overwrite an existing project registration","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable output","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"scope","aliases":[],"hidden":false,"deprecated":null,"path":["scope"],"command":"planar scope","summary":"Inspect the cwd-derived scope and suggest memberships.","description":"Inspect the scope Planar will resolve for the current working\n  directory.\n\n  Plan 153 removed the active scope stack; scope is now derived from\n  cwd and overridden by passing --scope <slug> to individual verbs.","subcommands":["show","suggest","use","pop","clear"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"show","aliases":[],"hidden":false,"deprecated":null,"path":["scope","show"],"command":"planar scope show","summary":"Show the cwd-derived scope (and any --scope override).","description":"Show the cwd-derived scope (and any --scope override).","subcommands":[],"flags":[{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug to inspect instead of the cwd-derived scope","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"suggest","aliases":[],"hidden":false,"deprecated":null,"path":["scope","suggest"],"command":"planar scope suggest","summary":"Suggest scope associations based on cwd.","description":"Suggest scope associations based on cwd.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"use","aliases":[],"hidden":false,"deprecated":null,"path":["scope","use"],"command":"planar scope use","summary":"Removed in plan 153 M5 — see `planar scope show`.","description":"Removed in plan 153 M5 — see `planar scope show`.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[{"name":"slug","kind":"string","required":false,"default":null,"description":"Ignored; the verb was removed and always refuses","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"pop","aliases":[],"hidden":false,"deprecated":null,"path":["scope","pop"],"command":"planar scope pop","summary":"Removed in plan 153 M5 — see `planar scope show`.","description":"Removed in plan 153 M5 — see `planar scope show`.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"clear","aliases":[],"hidden":false,"deprecated":null,"path":["scope","clear"],"command":"planar scope clear","summary":"Removed in plan 153 M5 — see `planar scope show`.","description":"Removed in plan 153 M5 — see `planar scope show`.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"assoc","aliases":[],"hidden":false,"deprecated":null,"path":["assoc"],"command":"planar assoc","summary":"Manage associations (many-to-many scope tags for repos).","description":"Manage associations — the many-to-many tags that group repos into\n  named scopes.\n\n  Both 'assoc' and 'association' are valid subcommand names.\n  User-creatable kinds: org, project, client, personal, ad-hoc.\n  Auto-detected kinds (via 'assoc detect'): host, path, lang.","subcommands":["list","create","add","remove","members","detect"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"list","aliases":[],"hidden":false,"deprecated":null,"path":["assoc","list"],"command":"planar assoc list","summary":"List all known associations.","description":"List all known associations.","subcommands":[],"flags":[{"long":"--kind","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to one association kind, e.g. org, project, personal, ad-hoc","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"create","aliases":[],"hidden":false,"deprecated":null,"path":["assoc","create"],"command":"planar assoc create","summary":"Create a new association.","description":"Create a new association.","subcommands":[],"flags":[{"long":"--name","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Human-readable name (default: the slug)","completion":{"kind":"none","values":[]},"env":null},{"long":"--kind","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Association kind: org, project, client, personal, ad-hoc (default: ad-hoc)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"slug","kind":"string","required":true,"default":null,"description":"Unique association slug, e.g. org:acme","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"add","aliases":[],"hidden":false,"deprecated":null,"path":["assoc","add"],"command":"planar assoc add","summary":"Add a repo to an association.","description":"Add a repo to an association.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"slug","kind":"string","required":true,"default":null,"description":"Association slug","completion":{"kind":"none","values":[]}},{"name":"repo-path","kind":"string","required":true,"default":null,"description":"Registered project root path to add to the association","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"remove","aliases":[],"hidden":false,"deprecated":null,"path":["assoc","remove"],"command":"planar assoc remove","summary":"Remove a repo from an association.","description":"Remove a repo from an association.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"slug","kind":"string","required":true,"default":null,"description":"Association slug","completion":{"kind":"none","values":[]}},{"name":"repo-path","kind":"string","required":true,"default":null,"description":"Project root path to remove from the association","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"members","aliases":[],"hidden":false,"deprecated":null,"path":["assoc","members"],"command":"planar assoc members","summary":"List all project members of an association.","description":"List all project members of an association.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"slug","kind":"string","required":true,"default":null,"description":"Association slug","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"detect","aliases":[],"hidden":false,"deprecated":null,"path":["assoc","detect"],"command":"planar assoc detect","summary":"Propose (or apply) auto-detected associations for the current directory.","description":"Propose (or apply) auto-detected associations for the current directory.","subcommands":[],"flags":[{"long":"--apply","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Create the proposed associations and add the current project (default: preview only)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"plan","aliases":[],"hidden":false,"deprecated":null,"path":["plan"],"command":"planar plan","summary":"Manage plans and plan steps.","description":"Manage plans — the top-level structured intent for a body of work.\n\n  Plans may be hierarchical (--parent) and contain ordered steps\n  (plan step add).\n  Status lifecycle: draft → active → paused / done / abandoned.","subcommands":["create","show","list","update","edit","view","diff","review","link","next","recommend-strategy","divergence","recompute-status","closeout","step","descendants"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"create","aliases":[],"hidden":false,"deprecated":null,"path":["plan","create"],"command":"planar plan create","summary":"Create a new plan.","description":"Create a new plan.","subcommands":[],"flags":[{"long":"--summary","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Plan summary text; a leading @ reads it from a file","completion":{"kind":"none","values":[]},"env":null},{"long":"--slug","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Slug for the plan (derived from the title when omitted)","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug to resolve against instead of the cwd-derived scope","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":"draft","description":"Initial status: draft, active, paused, done, abandoned","completion":{"kind":"none","values":[]},"env":null},{"long":"--parent","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Parent plan id","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"title","kind":"string","required":true,"default":null,"description":"Title of the new plan","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"show","aliases":[],"hidden":false,"deprecated":null,"path":["plan","show"],"command":"planar plan show","summary":"Show a plan's details, steps, and child plans.","description":"Show a plan's details, steps, and child plans.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan-id","kind":"string","required":true,"default":null,"description":"Plan id or slug","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"list","aliases":[],"hidden":false,"deprecated":null,"path":["plan","list"],"command":"planar plan list","summary":"List plans.","description":"List plans.","subcommands":[],"flags":[{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to this scope slug instead of the cwd-derived scope","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to a status or comma-separated list: draft, active, paused, done, abandoned","completion":{"kind":"none","values":[]},"env":null},{"long":"--parent","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Restrict to children of this parent plan id","completion":{"kind":"none","values":[]},"env":null},{"long":"--touches","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to plans scoped to or touching this repo slug","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"update","aliases":[],"hidden":false,"deprecated":null,"path":["plan","update"],"command":"planar plan update","summary":"Update mutable fields on a plan.","description":"Update mutable fields on a plan.","subcommands":[],"flags":[{"long":"--title","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"New title for the plan","completion":{"kind":"none","values":[]},"env":null},{"long":"--slug","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"New slug for the plan","completion":{"kind":"none","values":[]},"env":null},{"long":"--summary","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"New summary text; a leading @ reads it from a file","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"New status: draft, active, paused, done, abandoned","completion":{"kind":"none","values":[]},"env":null},{"long":"--parent","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"New parent plan id","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug to move the plan to","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan-id","kind":"string","required":true,"default":null,"description":"Plan id or slug","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"edit","aliases":[],"hidden":false,"deprecated":null,"path":["plan","edit"],"command":"planar plan edit","summary":"Edit a plan in $EDITOR (editor-first flow).","description":"Edit a plan in $EDITOR (editor-first flow).","subcommands":[],"flags":[{"long":"--no-pull","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Accepted for parity; the handler does not read it","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Accepted for parity; the handler does not read it","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan-id","kind":"string","required":true,"default":null,"description":"Plan id or slug","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"view","aliases":[],"hidden":false,"deprecated":null,"path":["plan","view"],"command":"planar plan view","summary":"View a plan's workbench file.","description":"View a plan's workbench file.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[{"name":"plan-id","kind":"string","required":true,"default":null,"description":"Plan id or slug","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"diff","aliases":[],"hidden":false,"deprecated":null,"path":["plan","diff"],"command":"planar plan diff","summary":"Diff plan against database version.","description":"Diff plan against database version.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[{"name":"plan-id","kind":"string","required":true,"default":null,"description":"Plan id or slug","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"review","aliases":[],"hidden":false,"deprecated":null,"path":["plan","review"],"command":"planar plan review","summary":"Reviewer entry point for plan diff.","description":"Reviewer entry point for plan diff.","subcommands":[],"flags":[{"long":"--approve","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Report an approve verdict on the pending workbench diff (not persisted)","completion":{"kind":"none","values":[]},"env":null},{"long":"--request-changes","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Report a request-changes verdict on the pending workbench diff (not persisted); excludes --approve","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan-id","kind":"string","required":true,"default":null,"description":"Plan id or slug","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"link","aliases":[],"hidden":false,"deprecated":null,"path":["plan","link"],"command":"planar plan link","summary":"Create an entity link from a plan to another entity.","description":"Create an entity link from a plan to another entity.","subcommands":[],"flags":[{"long":"--relationship","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Link relationship: derives-from, depends-on, addresses, verifies, cites, supersedes, touches","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Accepted but not read by this verb; no scope check is made","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan-id","kind":"string","required":true,"default":null,"description":"Plan id or slug","completion":{"kind":"none","values":[]}},{"name":"ref","kind":"string","required":true,"default":null,"description":"Target entity ref (kind:id)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"next","aliases":[],"hidden":false,"deprecated":null,"path":["plan","next"],"command":"planar plan next","summary":"Bucketed claim-aware view of next work on a plan (available / claimed / stale / blocked).","description":"Bucketed claim-aware view of next work on a plan.\n\n  Buckets:\n    available  task is todo (or doing without an active claim)\n               and ready to be pulled\n    claimed    task has an active unexpired claim\n    stale      task has a stale claim (reconcile or lease-expired)\n    blocked    task status is blocked\n\n  Without --include-claimed / --include-stale the text rendering\n  shows only the available + blocked buckets — the JSON shape always\n  carries every bucket.","subcommands":[],"flags":[{"long":"--include-claimed","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Show the claimed bucket in text mode (JSON always includes it)","completion":{"kind":"none","values":[]},"env":null},{"long":"--include-stale","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Show the stale bucket in text mode (JSON always includes it)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan-id","kind":"string","required":true,"default":null,"description":"Plan id or slug","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"recommend-strategy","aliases":[],"hidden":false,"deprecated":null,"path":["plan","recommend-strategy"],"command":"planar plan recommend-strategy","summary":"Recommend an execution strategy: compute the parallel-eligible subset of a plan's open tasks via the six parallelizability rules.","description":"Recommend an execution strategy for a plan's open (todo) tasks.\n\n  Applies the six parallel-eligibility rules (decision 370) and\n  reports the parallel-eligible subset plus the serialized remainder\n  with per-task exclusion reasons:\n    1. no blocked_by chain to a not-done task\n    2. disjoint task_touches (empty touches = touches-everything)\n    3. no schema migration touched\n    4. no singleton authoritative file touched\n    5. no open question linked\n    6. no proposed decision linked\n\n  --closure-source selects the signal rule 2's overlap test reads\n  (decision D4): 'declared' (default) uses the declared task_touches and\n  is byte-for-byte the pre-D4 behavior; 'derived' uses the computed\n  symbol-level closure (closures table) so two tasks overlap when their\n  derived closures share a symbol even when their declared files differ.\n\n  READ-ONLY: computes and reports; writes nothing. fan_out_available\n  is true when >= 2 tasks are eligible.","subcommands":[],"flags":[{"long":"--closure-source","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":"declared","description":"Rule-2 overlap signal: 'declared' (default, task_touches) or 'derived' (computed closure).","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan-id","kind":"string","required":true,"default":null,"description":"Plan id or slug","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"divergence","aliases":[],"hidden":false,"deprecated":null,"path":["plan","divergence"],"command":"planar plan divergence","summary":"Report the declared-vs-derived closure divergence for a plan's open tasks (decision D4).","description":"Report the declared-vs-derived closure divergence for a plan's open tasks.\n\n  For every unordered pair of open (todo) tasks, compares whether the two\n  tasks overlap under the DECLARED touch set vs. the DERIVED closure set.\n  A pair whose verdict differs between sources is a FLIP — the two sources\n  disagree about whether those tasks can run in parallel.\n\n  Jaccard distance = flips / |declared_overlaps ∪ derived_overlaps|.\n  0.0 = sources agree on every pair; 1.0 = no overlapping pair in common.\n\n  READ-ONLY: computes and reports; writes nothing.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan-id","kind":"string","required":true,"default":null,"description":"Plan id or slug","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"recompute-status","aliases":[],"hidden":false,"deprecated":null,"path":["plan","recompute-status"],"command":"planar plan recompute-status","summary":"Recompute a plan's roll-up status (--plan <id> or --all).","description":"Recompute a plan's roll-up status (--plan <id> or --all).","subcommands":[],"flags":[{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Recompute one plan by id","completion":{"kind":"none","values":[]},"env":null},{"long":"--all","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Recompute every plan in the DB","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"closeout","aliases":[],"hidden":false,"deprecated":null,"path":["plan","closeout"],"command":"planar plan closeout","summary":"Delivery-evidence gate: report whether a plan is ready to close and (without --dry-run) mark it done.","description":"Evaluate the DB-hard gate (all tasks terminal, all descendants terminal, no live claims)\n  and advisory git-evidence for a plan. In apply mode (no --dry-run), marks the plan\n  done when the hard gate passes. Cancelled tasks are terminal — they do not block.\n\n  Hard gate failures exit non-zero in APPLY mode. --dry-run always exits 0:\n  it is a preview, and the caller reads ready/blocked_by from the report.\n\n  --check-merge adds an advisory epic-branch merge roll-up: for each contributing\n  branch from agent_work_claims, reports how many are merged to the target branch.\n  Never blocks; absent branches are inconclusive.","subcommands":[],"flags":[{"long":"--dry-run","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Evaluate and report only; never writes","completion":{"kind":"none","values":[]},"env":null},{"long":"--check-merge","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Include advisory epic-branch merge roll-up in the output","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan-id","kind":"string","required":true,"default":null,"description":"Plan id or slug","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"step","aliases":[],"hidden":false,"deprecated":null,"path":["plan","step"],"command":"planar plan step","summary":"Manage plan steps.","description":"Manage plan steps.","subcommands":["add","list","done","skip","link"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"add","aliases":[],"hidden":false,"deprecated":null,"path":["plan","step","add"],"command":"planar plan step add","summary":"Append a new step to a plan.","description":"Append a new step to a plan.","subcommands":[],"flags":[{"long":"--after","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Insert after this step ordinal, renumbering later steps (default: append)","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Accepted but not read by this verb; no scope check is made","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan-id","kind":"string","required":true,"default":null,"description":"Plan id or slug","completion":{"kind":"none","values":[]}},{"name":"body","kind":"string","required":true,"default":null,"description":"Step body text","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"list","aliases":[],"hidden":false,"deprecated":null,"path":["plan","step","list"],"command":"planar plan step list","summary":"List steps of a plan.","description":"List steps of a plan.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan-id","kind":"string","required":true,"default":null,"description":"Plan id or slug","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"done","aliases":[],"hidden":false,"deprecated":null,"path":["plan","step","done"],"command":"planar plan step done","summary":"Mark a plan step as done.","description":"Mark a plan step as done.","subcommands":[],"flags":[{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Accepted but not read by this verb; no scope check is made","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"step-id","kind":"string","required":true,"default":null,"description":"Plan step id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"skip","aliases":[],"hidden":false,"deprecated":null,"path":["plan","step","skip"],"command":"planar plan step skip","summary":"Mark a plan step as skipped.","description":"Mark a plan step as skipped.","subcommands":[],"flags":[{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Accepted but not read by this verb; no scope check is made","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"step-id","kind":"string","required":true,"default":null,"description":"Plan step id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"link","aliases":[],"hidden":false,"deprecated":null,"path":["plan","step","link"],"command":"planar plan step link","summary":"Associate a plan step with the task that materializes it.","description":"Associate a plan step with the task that materializes it.","subcommands":[],"flags":[{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Accepted but not read by this verb; no scope check is made","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"step-id","kind":"string","required":true,"default":null,"description":"Plan step id","completion":{"kind":"none","values":[]}},{"name":"task-id","kind":"string","required":true,"default":null,"description":"Task id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"descendants","aliases":[],"hidden":false,"deprecated":null,"path":["plan","descendants"],"command":"planar plan descendants","summary":"Emit the anchor plan's full subtree in dependency-topological order. READ-ONLY.","description":"Emit the anchor plan's full subtree (child plans + tasks) in\n  dependency-topological order (anchor → child plans → tasks).\n\n  READ-ONLY: queries and reports; writes nothing.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan-id","kind":"string","required":true,"default":null,"description":"Plan id or slug","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"task","aliases":[],"hidden":false,"deprecated":null,"path":["task"],"command":"planar task","summary":"Manage tasks.","description":"Manage tasks — the discrete units of work.\n\n  Tasks may belong to a plan (--plan) or another task (--parent), and\n  carry the next_action field required by resume validate.\n  Status lifecycle: todo → doing → done / cancelled; blocked is set\n  via task block.","subcommands":["add","show","packet","list","update","edit","view","diff","review","done","cancel","block","link","reopen","touches","facts"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"add","aliases":[],"hidden":false,"deprecated":null,"path":["task","add"],"command":"planar task add","summary":"Create a new task.","description":"Create a new task.","subcommands":[],"flags":[{"long":"--body","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Task body; use @<file> to read it from a file","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug for the new task instead of the cwd-derived write scope","completion":{"kind":"none","values":[]},"env":null},{"long":"--next-action","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Immediate next concrete action (needed for `resume validate` to pass)","completion":{"kind":"none","values":[]},"env":null},{"long":"--due","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Due date (ISO 8601, e.g. 2026-05-15)","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Plan id to associate the task with","completion":{"kind":"none","values":[]},"env":null},{"long":"--parent","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Parent task id, making this a sub-task","completion":{"kind":"none","values":[]},"env":null},{"long":"--slug","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Explicit slug (globally unique across tasks)","completion":{"kind":"none","values":[]},"env":null},{"long":"--priority","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":100,"description":"Integer priority; lower is higher","completion":{"kind":"none","values":[]},"env":null},{"long":"--editor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":true,"description":"Editor body flow; refuses on a TTY with no --body unless --editor=false","completion":{"kind":"none","values":[]},"env":null},{"long":"--no-auto-promote","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Skip the plan-status auto-promotion for this operation","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"title","kind":"string","required":true,"default":null,"description":"Task title","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"show","aliases":[],"hidden":false,"deprecated":null,"path":["task","show"],"command":"planar task show","summary":"Show full task details.","description":"Show full task details.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"task-id","kind":"string","required":true,"default":null,"description":"Task id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"packet","aliases":[],"hidden":false,"deprecated":null,"path":["task","packet"],"command":"planar task packet","summary":"Compile the authoritative current routing packet for a task.","description":"Compile the authoritative current routing packet for a task.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"task-id","kind":"string","required":true,"default":null,"description":"Task id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"list","aliases":[],"hidden":false,"deprecated":null,"path":["task","list"],"command":"planar task list","summary":"List tasks.","description":"List tasks.","subcommands":[],"flags":[{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to this scope slug instead of the cwd-derived read set","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to a status: todo, doing, blocked, done, cancelled (default: open statuses)","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Restrict to tasks of this plan id","completion":{"kind":"none","values":[]},"env":null},{"long":"--priority-max","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Only tasks with priority at or below this value","completion":{"kind":"none","values":[]},"env":null},{"long":"--touches","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to tasks scoped to or touching this repo slug","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"update","aliases":[],"hidden":false,"deprecated":null,"path":["task","update"],"command":"planar task update","summary":"Update mutable fields on a task.","description":"Update mutable fields on a task.","subcommands":[],"flags":[{"long":"--title","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"New title","completion":{"kind":"none","values":[]},"env":null},{"long":"--body","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"New body; use @<file> to read it from a file","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"New status: todo, doing, blocked, done, cancelled (must be a legal transition)","completion":{"kind":"none","values":[]},"env":null},{"long":"--next-action","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"New next concrete action","completion":{"kind":"none","values":[]},"env":null},{"long":"--due","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"New due date (ISO 8601, e.g. 2026-05-15)","completion":{"kind":"none","values":[]},"env":null},{"long":"--priority","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"New priority integer (lower is higher)","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Move the task to this plan id","completion":{"kind":"none","values":[]},"env":null},{"long":"--slug","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"New slug (globally unique across tasks)","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug to resolve against instead of the cwd-derived scope","completion":{"kind":"none","values":[]},"env":null},{"long":"--force","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Bypass the active-claim guard and the status-transition matrix","completion":{"kind":"none","values":[]},"env":null},{"long":"--reason","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Rationale recorded on the audit row when --force reopens a terminal task","completion":{"kind":"none","values":[]},"env":null},{"long":"--no-auto-promote","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Skip the plan-status auto-promotion for this update","completion":{"kind":"none","values":[]},"env":null},{"long":"--editor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Accepted and ignored; use `task edit` for the editor flow","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"task-id","kind":"string","required":true,"default":null,"description":"Task id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"edit","aliases":[],"hidden":false,"deprecated":null,"path":["task","edit"],"command":"planar task edit","summary":"Edit a task in $EDITOR (editor-first flow).","description":"Edit a task in $EDITOR (editor-first flow).","subcommands":[],"flags":[{"long":"--no-pull","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Accepted for parity; the handler does not read it","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Accepted for parity; the handler does not read it","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"task-id","kind":"string","required":true,"default":null,"description":"Task id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"view","aliases":[],"hidden":false,"deprecated":null,"path":["task","view"],"command":"planar task view","summary":"View task's workbench file.","description":"View task's workbench file.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[{"name":"task-id","kind":"string","required":true,"default":null,"description":"Task id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"diff","aliases":[],"hidden":false,"deprecated":null,"path":["task","diff"],"command":"planar task diff","summary":"Diff task against its database-stored version.","description":"Diff task against its database-stored version.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[{"name":"task-id","kind":"string","required":true,"default":null,"description":"Task id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"review","aliases":[],"hidden":false,"deprecated":null,"path":["task","review"],"command":"planar task review","summary":"Reviewer entry point for task diff.","description":"Reviewer entry point for task diff.","subcommands":[],"flags":[{"long":"--approve","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Report an approve verdict on the pending workbench diff (not persisted)","completion":{"kind":"none","values":[]},"env":null},{"long":"--request-changes","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Report a request-changes verdict on the pending workbench diff (not persisted); excludes --approve","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"task-id","kind":"string","required":true,"default":null,"description":"Task id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"done","aliases":[],"hidden":false,"deprecated":null,"path":["task","done"],"command":"planar task done","summary":"Mark a task as done.","description":"Mark a task as done.","subcommands":[],"flags":[{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Accepted but not read by this verb; no scope check is made","completion":{"kind":"none","values":[]},"env":null},{"long":"--force","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Override active-claim guard and flip status anyway","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"task-id","kind":"string","required":true,"default":null,"description":"Task id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"cancel","aliases":[],"hidden":false,"deprecated":null,"path":["task","cancel"],"command":"planar task cancel","summary":"Cancel a task.","description":"Cancel a task.","subcommands":[],"flags":[{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Accepted but not read by this verb; no scope check is made","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"task-id","kind":"string","required":true,"default":null,"description":"Task id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"block","aliases":[],"hidden":false,"deprecated":null,"path":["task","block"],"command":"planar task block","summary":"Mark a task as blocked and record the blocking relationship.","description":"Mark a task as blocked and record the blocking relationship.","subcommands":[],"flags":[{"long":"--on","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"N","default":null,"description":"Blocking task id","completion":{"kind":"none","values":[]},"env":null},{"long":"--reason","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Optional reason for the block","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Accepted but not read by this verb; no scope check is made","completion":{"kind":"none","values":[]},"env":null},{"long":"--force","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Override active-claim guard and flip status anyway","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"task-id","kind":"string","required":true,"default":null,"description":"Task id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"link","aliases":[],"hidden":false,"deprecated":null,"path":["task","link"],"command":"planar task link","summary":"Create an entity link from a task to another entity.","description":"Create an entity link from a task to another entity.","subcommands":[],"flags":[{"long":"--relationship","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Link relationship: derives-from, depends-on, addresses, verifies, cites, supersedes, touches","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Accepted but not read by this verb; no scope check is made","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"task-id","kind":"string","required":true,"default":null,"description":"Task id","completion":{"kind":"none","values":[]}},{"name":"ref","kind":"string","required":true,"default":null,"description":"Target entity ref (kind:id)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"reopen","aliases":[],"hidden":false,"deprecated":null,"path":["task","reopen"],"command":"planar task reopen","summary":"Reopen a done or cancelled task with an audit-trail entry.","description":"Reopen a done or cancelled task with an audit-trail entry.","subcommands":[],"flags":[{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Non-terminal status to reopen into: todo, doing, blocked (default: todo)","completion":{"kind":"none","values":[]},"env":null},{"long":"--reason","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Rationale recorded on the task_reopens audit row","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Accepted but not read by this verb; no scope check is made","completion":{"kind":"none","values":[]},"env":null},{"long":"--force","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Override active-claim guard and flip status anyway","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"task-id","kind":"string","required":true,"default":null,"description":"Task id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"touches","aliases":[],"hidden":false,"deprecated":null,"path":["task","touches"],"command":"planar task touches","summary":"Manage repo-touches links on a task.","description":"Manage repo-touches links on a task.","subcommands":["add","infer","list","remove"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"add","aliases":[],"hidden":false,"deprecated":null,"path":["task","touches","add"],"command":"planar task touches add","summary":"Link a task to a repo via a 'touches' relationship.","description":"Declare that a task touches a repo (and, with --path, a specific file).\n\n  Without --path: writes the repo-level entity_links 'touches' edge\n  (task -> repo). This is the coarse signal used by `task list --touches`.\n\n  With --path <p>: writes a path-level task_touch_paths row (task, repo,\n  path) AND the repo-level edge — a path-touch implies the repo-touch, so\n  the repo-level signal stays consistent. <p> is a repo-relative file path.\n  The parallelizability rules (`plan recommend-strategy`) read these\n  path-level declarations for rules 2/3/4 (disjoint touches, migration\n  touched, singleton file touched). Declare path touches per file (repeat\n  the verb), not as a list.","subcommands":[],"flags":[{"long":"--path","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Repo-relative file path to record as a path-level touch","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Accepted but not read by this verb; no scope check is made","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"task-id","kind":"string","required":true,"default":null,"description":"Task id","completion":{"kind":"none","values":[]}},{"name":"repo-slug","kind":"string","required":true,"default":null,"description":"Slug of a registered repo the task touches","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"infer","aliases":[],"hidden":false,"deprecated":null,"path":["task","touches","infer"],"command":"planar task touches infer","summary":"Propose path-level touches from the task's own text (preview by default).","description":"Extract path-shaped tokens from a task's title, body, and next_action\n  and resolve them against a repo checkout, proposing task_touch_paths\n  rows. PREVIEW BY DEFAULT — without --apply nothing is written.\n\n  Each candidate is classified: 'resolved' (exact file), 'directory'\n  (expanded to its files), 'basename' (every matching path), 'unresolved'\n  (path-shaped but unplaceable) or 'too_broad' (expansion too large).\n\n  Only 'resolved' is written by default. The wide classifications —\n  directory and basename — are shown with their expansion size and\n  withheld unless --wide is passed. Measured over 46 tasks in six real\n  plans, including them yielded FEWER parallel-eligible tasks (13) than\n  resolved-only (14): a wide set intersects peers, and rule 2 drops both\n  sides of an overlap, so one loose directory mention can remove tasks\n  that were otherwise eligible.\n\n  Proposal still resolves ambiguity wide (decision 906) — a directory\n  expands, a basename yields every match, nothing unplaceable is\n  invented. What --wide controls is which proposals are WRITTEN.\n\n  --repo <slug> names the checkout to resolve against; without it the repo\n  is derived from the current directory (longest matching root_path).","subcommands":[],"flags":[{"long":"--repo","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Repo slug to resolve paths against (default: derived from the cwd)","completion":{"kind":"none","values":[]},"env":null},{"long":"--apply","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Write the proposed touch paths; without it the run is a preview","completion":{"kind":"none","values":[]},"env":null},{"long":"--wide","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Also write directory and basename expansions, not just exact files","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"task-id","kind":"string","required":true,"default":null,"description":"Task id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"list","aliases":[],"hidden":false,"deprecated":null,"path":["task","touches","list"],"command":"planar task touches list","summary":"List the repo- and path-level touches declared on a task.","description":"List the repo- and path-level touches declared on a task.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"task-id","kind":"string","required":true,"default":null,"description":"Task id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"remove","aliases":[],"hidden":false,"deprecated":null,"path":["task","touches","remove"],"command":"planar task touches remove","summary":"Remove a 'touches' link between a task and a repo.","description":"Withdraw a touch declaration.\n\n  Without --path: removes the repo-level entity_links 'touches' edge.\n\n  With --path <p>: removes ONE path-level task_touch_paths row and leaves\n  the repo edge in place. Deliberately not symmetric with `touches add`,\n  where a path-touch implies the repo-touch — withdrawing one file should\n  not silently drop a repo claim that may carry other paths.\n\n  Removing the repo edge is not a substitute for --path: the parallel\n  eligibility rules read task_touch_paths directly, so orphaned path rows\n  keep driving eligibility after their edge is gone.","subcommands":[],"flags":[{"long":"--path","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Repo-relative file path whose touch to remove (leaves the repo-level link)","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Accepted but not read by this verb; no scope check is made","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"task-id","kind":"string","required":true,"default":null,"description":"Task id","completion":{"kind":"none","values":[]}},{"name":"repo-slug","kind":"string","required":true,"default":null,"description":"Slug of the repo whose touch declaration to withdraw","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"facts","aliases":[],"hidden":false,"deprecated":null,"path":["task","facts"],"command":"planar task facts","summary":"Manage routing facts on a task.","description":"Manage routing facts on a task.","subcommands":["stage"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"stage","aliases":[],"hidden":false,"deprecated":null,"path":["task","facts","stage"],"command":"planar task facts stage","summary":"Stage this task's routing facts under operator provenance.","description":"Stage this task's routing facts under operator provenance.\n\n  `spec ingest --apply` is the only other writer of routing facts, and it\n  rebuilds an entire anchor plan, so a hand-filed task could never obtain\n  them and an edited task could never restage them. This stages exactly\n  one task, stamped `operator-v1`, and never touches a sibling's facts.\n\n  Citation facts are staged only for artifacts this task already cites\n  AND references explicitly in its body; it never invents a citation.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"task-id","kind":"string","required":true,"default":null,"description":"Task id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"question","aliases":[],"hidden":false,"deprecated":null,"path":["question"],"command":"planar question","summary":"Manage questions.","description":"Manage questions — open uncertainties surfaced during work.\n\n  Status lifecycle: open → answered (via 'question answer') / wontfix.","subcommands":["add","edit","view","diff","review","answer","wontfix","list","show","link"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"add","aliases":[],"hidden":false,"deprecated":null,"path":["question","add"],"command":"planar question add","summary":"Create a new question.","description":"Create a new question.","subcommands":[],"flags":[{"long":"--body","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Question body text; a leading @ reads it from a file","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug to resolve against instead of the cwd-derived scope","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Plan id to attach the question to","completion":{"kind":"none","values":[]},"env":null},{"long":"--editor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Accepted but not implemented; falls back to inline create","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"title","kind":"string","required":true,"default":null,"description":"Title of the new question","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"edit","aliases":[],"hidden":false,"deprecated":null,"path":["question","edit"],"command":"planar question edit","summary":"Edit a question in $EDITOR (editor-first flow).","description":"Edit a question in $EDITOR (editor-first flow).","subcommands":[],"flags":[{"long":"--no-pull","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Accepted for parity; the handler does not read it","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Accepted for parity; the handler does not read it","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"question-id","kind":"string","required":true,"default":null,"description":"Question id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"view","aliases":[],"hidden":false,"deprecated":null,"path":["question","view"],"command":"planar question view","summary":"View question's workbench file.","description":"View question's workbench file.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[{"name":"question-id","kind":"string","required":true,"default":null,"description":"Question id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"diff","aliases":[],"hidden":false,"deprecated":null,"path":["question","diff"],"command":"planar question diff","summary":"Diff question against database version.","description":"Diff question against database version.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[{"name":"question-id","kind":"string","required":true,"default":null,"description":"Question id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"review","aliases":[],"hidden":false,"deprecated":null,"path":["question","review"],"command":"planar question review","summary":"Reviewer entry point for question diff.","description":"Reviewer entry point for question diff.","subcommands":[],"flags":[{"long":"--approve","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Report an approve verdict on the pending workbench diff (not persisted)","completion":{"kind":"none","values":[]},"env":null},{"long":"--request-changes","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Report a request-changes verdict on the pending workbench diff (not persisted); excludes --approve","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"question-id","kind":"string","required":true,"default":null,"description":"Question id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"answer","aliases":[],"hidden":false,"deprecated":null,"path":["question","answer"],"command":"planar question answer","summary":"Record an answer to a question.","description":"Record an answer to a question.","subcommands":[],"flags":[{"long":"--answer","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Answer text, taken literally","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"question-id","kind":"string","required":true,"default":null,"description":"Question id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"wontfix","aliases":[],"hidden":false,"deprecated":null,"path":["question","wontfix"],"command":"planar question wontfix","summary":"Mark a question as wontfix.","description":"Mark a question as wontfix.","subcommands":[],"flags":[{"long":"--reason","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Reason the question will not be answered","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"question-id","kind":"string","required":true,"default":null,"description":"Question id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"list","aliases":[],"hidden":false,"deprecated":null,"path":["question","list"],"command":"planar question list","summary":"List questions.","description":"List questions.","subcommands":[],"flags":[{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to this scope slug instead of the cwd-derived scope","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to a status or comma-separated list: open, answered, wontfix","completion":{"kind":"none","values":[]},"env":null},{"long":"--touches","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to entities scoped to or touching this repo slug","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Restrict to questions attached to this plan id","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"show","aliases":[],"hidden":false,"deprecated":null,"path":["question","show"],"command":"planar question show","summary":"Show a question's details.","description":"Show a question's details.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"question-id","kind":"string","required":true,"default":null,"description":"Question id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"link","aliases":[],"hidden":false,"deprecated":null,"path":["question","link"],"command":"planar question link","summary":"Create an entity link from a question to another entity.","description":"Create an entity link from a question to another entity.","subcommands":[],"flags":[{"long":"--relationship","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Link relationship: derives-from, depends-on, addresses, verifies, cites, supersedes, touches","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Accepted but not read by this verb; no scope check is made","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"question-id","kind":"string","required":true,"default":null,"description":"Question id","completion":{"kind":"none","values":[]}},{"name":"ref","kind":"string","required":true,"default":null,"description":"Target entity ref (kind:id)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"scenario","aliases":[],"hidden":false,"deprecated":null,"path":["scenario"],"command":"planar scenario","summary":"Manage test scenarios.","description":"Manage test scenarios — verification artifacts tied to specs,\n  plans, or tasks.\n\n  Planar records scenarios and their outcomes; it does not execute\n  them.\n  Status lifecycle: draft → ready → verified / failing → retired.\n  Transitions: `scenario verify` (draft→verified via auto-ready, or ready→verified),\n  `scenario retire` (any→retired).","subcommands":["add","edit","view","diff","review","verify","retire","list","show","link"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"add","aliases":[],"hidden":false,"deprecated":null,"path":["scenario","add"],"command":"planar scenario add","summary":"Create a new test scenario.","description":"Create a new test scenario.","subcommands":[],"flags":[{"long":"--body","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scenario description; @<file> reads it from a file","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug to create in (default: cwd-derived write scope)","completion":{"kind":"none","values":[]},"env":null},{"long":"--related","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Artifact id (spec or ADR) the scenario relates to","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Attach the scenario to this plan id","completion":{"kind":"none","values":[]},"env":null},{"long":"--editor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Accepted for parity; not implemented, create proceeds inline","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"title","kind":"string","required":true,"default":null,"description":"Scenario title","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"edit","aliases":[],"hidden":false,"deprecated":null,"path":["scenario","edit"],"command":"planar scenario edit","summary":"Edit a scenario in $EDITOR (editor-first flow).","description":"Edit a scenario in $EDITOR (editor-first flow).","subcommands":[],"flags":[{"long":"--no-pull","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Accepted for parity; the handler does not read it","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Accepted for parity; the handler does not read it","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"scenario-id","kind":"string","required":true,"default":null,"description":"Scenario id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"view","aliases":[],"hidden":false,"deprecated":null,"path":["scenario","view"],"command":"planar scenario view","summary":"View scenario's workbench file.","description":"View scenario's workbench file.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[{"name":"scenario-id","kind":"string","required":true,"default":null,"description":"Scenario id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"diff","aliases":[],"hidden":false,"deprecated":null,"path":["scenario","diff"],"command":"planar scenario diff","summary":"Diff scenario against database version.","description":"Diff scenario against database version.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[{"name":"scenario-id","kind":"string","required":true,"default":null,"description":"Scenario id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"review","aliases":[],"hidden":false,"deprecated":null,"path":["scenario","review"],"command":"planar scenario review","summary":"Reviewer entry point for scenario diff.","description":"Reviewer entry point for scenario diff.","subcommands":[],"flags":[{"long":"--approve","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Report an approve verdict on the pending workbench diff (not persisted)","completion":{"kind":"none","values":[]},"env":null},{"long":"--request-changes","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Report a request-changes verdict on the pending workbench diff (not persisted); excludes --approve","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"scenario-id","kind":"string","required":true,"default":null,"description":"Scenario id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"verify","aliases":[],"hidden":false,"deprecated":null,"path":["scenario","verify"],"command":"planar scenario verify","summary":"Record a test run for a scenario (--outcome pass|fail|error|skipped; defaults to pass).","description":"Record a test run for a scenario (--outcome pass|fail|error|skipped; defaults to pass).","subcommands":[],"flags":[{"long":"--outcome","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Run outcome: pass, fail, error, skipped (default: pass)","completion":{"kind":"none","values":[]},"env":null},{"long":"--summary","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Free-text summary of the run","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"scenario-id","kind":"string","required":true,"default":null,"description":"Scenario id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"retire","aliases":[],"hidden":false,"deprecated":null,"path":["scenario","retire"],"command":"planar scenario retire","summary":"Mark a scenario as retired.","description":"Mark a scenario as retired.","subcommands":[],"flags":[{"long":"--reason","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Why the scenario is being retired","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"scenario-id","kind":"string","required":true,"default":null,"description":"Scenario id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"list","aliases":[],"hidden":false,"deprecated":null,"path":["scenario","list"],"command":"planar scenario list","summary":"List scenarios.","description":"List scenarios.","subcommands":[],"flags":[{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug to filter by (default: cwd-derived read set)","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Filter by scenario status: draft, ready, verified, failing, retired","completion":{"kind":"none","values":[]},"env":null},{"long":"--related","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Restrict to scenarios related to this artifact id","completion":{"kind":"none","values":[]},"env":null},{"long":"--touches","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to scenarios touching this repo slug","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"show","aliases":[],"hidden":false,"deprecated":null,"path":["scenario","show"],"command":"planar scenario show","summary":"Show a scenario's details.","description":"Show a scenario's details.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"scenario-id","kind":"string","required":true,"default":null,"description":"Scenario id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"link","aliases":[],"hidden":false,"deprecated":null,"path":["scenario","link"],"command":"planar scenario link","summary":"Create an entity link from a scenario to another entity.","description":"Create an entity link from a scenario to another entity.","subcommands":[],"flags":[{"long":"--relationship","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Link relationship: derives-from, depends-on, addresses, verifies, cites, supersedes, touches","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Accepted but not read by this verb; no scope check is made","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"scenario-id","kind":"string","required":true,"default":null,"description":"Scenario id","completion":{"kind":"none","values":[]}},{"name":"ref","kind":"string","required":true,"default":null,"description":"Target entity ref (kind:id)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"decision","aliases":[],"hidden":false,"deprecated":null,"path":["decision"],"command":"planar decision","summary":"Manage decision records.","description":"Manage decision records — rationale for choices made during work.\n\n  Status lifecycle: proposed → accepted / superseded / withdrawn.\n  Terminal statuses: superseded, withdrawn.","subcommands":["add","show","list","accept","supersede","withdraw","edit","view","diff","review","link"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"add","aliases":[],"hidden":false,"deprecated":null,"path":["decision","add"],"command":"planar decision add","summary":"Create a new decision record.","description":"Create a new decision record.","subcommands":[],"flags":[{"long":"--body","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Decision body text; a leading @ reads it from a file","completion":{"kind":"none","values":[]},"env":null},{"long":"--rationale","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Why the decision was made; a leading @ reads it from a file","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Plan id to attach the decision to","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug to resolve against instead of the cwd-derived scope","completion":{"kind":"none","values":[]},"env":null},{"long":"--editor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Accepted but not implemented; without --body it warns and the verb still refuses","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"title","kind":"string","required":true,"default":null,"description":"Title of the new decision","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"show","aliases":[],"hidden":false,"deprecated":null,"path":["decision","show"],"command":"planar decision show","summary":"Show a decision's details.","description":"Show a decision's details.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"decision-id","kind":"string","required":true,"default":null,"description":"Decision id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"list","aliases":[],"hidden":false,"deprecated":null,"path":["decision","list"],"command":"planar decision list","summary":"List decisions.","description":"List decisions.","subcommands":[],"flags":[{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to this scope slug instead of the cwd-derived scope","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to a status or comma-separated list: proposed, accepted, withdrawn, superseded","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Restrict to decisions attached to this plan id","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"accept","aliases":[],"hidden":false,"deprecated":null,"path":["decision","accept"],"command":"planar decision accept","summary":"Accept a proposed decision.","description":"Accept a proposed decision.","subcommands":[],"flags":[{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug to resolve against instead of the cwd-derived scope","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"decision-id","kind":"string","required":true,"default":null,"description":"Decision id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"supersede","aliases":[],"hidden":false,"deprecated":null,"path":["decision","supersede"],"command":"planar decision supersede","summary":"Mark a decision as superseded by a newer decision.","description":"Mark a decision as superseded by a newer decision.","subcommands":[],"flags":[{"long":"--by","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"N","default":null,"description":"Id of the decision that replaces this one","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Accepted but not read by this verb; no scope check is made","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"decision-id","kind":"string","required":true,"default":null,"description":"Decision id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"withdraw","aliases":[],"hidden":false,"deprecated":null,"path":["decision","withdraw"],"command":"planar decision withdraw","summary":"Withdraw a decision.","description":"Withdraw a decision.","subcommands":[],"flags":[{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug to resolve against instead of the cwd-derived scope","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"decision-id","kind":"string","required":true,"default":null,"description":"Decision id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"edit","aliases":[],"hidden":false,"deprecated":null,"path":["decision","edit"],"command":"planar decision edit","summary":"Edit a decision in $EDITOR (editor-first flow).","description":"Edit a decision in $EDITOR (editor-first flow).","subcommands":[],"flags":[{"long":"--no-pull","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Accepted for parity; the handler does not read it","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Accepted for parity; the handler does not read it","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"decision-id","kind":"string","required":true,"default":null,"description":"Decision id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"view","aliases":[],"hidden":false,"deprecated":null,"path":["decision","view"],"command":"planar decision view","summary":"View decision's workbench file.","description":"View decision's workbench file.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[{"name":"decision-id","kind":"string","required":true,"default":null,"description":"Decision id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"diff","aliases":[],"hidden":false,"deprecated":null,"path":["decision","diff"],"command":"planar decision diff","summary":"Diff decision against database version.","description":"Diff decision against database version.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[{"name":"decision-id","kind":"string","required":true,"default":null,"description":"Decision id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"review","aliases":[],"hidden":false,"deprecated":null,"path":["decision","review"],"command":"planar decision review","summary":"Reviewer entry point for decision diff.","description":"Reviewer entry point for decision diff.","subcommands":[],"flags":[{"long":"--approve","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Report an approve verdict on the pending workbench diff (not persisted)","completion":{"kind":"none","values":[]},"env":null},{"long":"--request-changes","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Report a request-changes verdict on the pending workbench diff (not persisted); excludes --approve","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"decision-id","kind":"string","required":true,"default":null,"description":"Decision id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"link","aliases":[],"hidden":false,"deprecated":null,"path":["decision","link"],"command":"planar decision link","summary":"Create an entity link from a decision to another entity.","description":"Create an entity link from a decision to another entity.","subcommands":[],"flags":[{"long":"--relationship","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Link relationship: derives-from, depends-on, addresses, verifies, cites, supersedes, touches","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Accepted but not read by this verb; no scope check is made","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"decision-id","kind":"string","required":true,"default":null,"description":"Decision id","completion":{"kind":"none","values":[]}},{"name":"ref","kind":"string","required":true,"default":null,"description":"Target entity ref (kind:id)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"artifact","aliases":[],"hidden":false,"deprecated":null,"path":["artifact"],"command":"planar artifact","summary":"Manage artifacts (tech specs, ADRs, design notes, etc.).","description":"Manage artifacts — durable documents that crystallize from work.\n\n  Kinds: tech_spec, adr, design_note, summary, readme, generated,\n  other, product_spec, roadmap, research, getting_started,\n  changelog_entry, glossary_term.\n  Status lifecycle: draft → active → superseded/retired.","subcommands":["add","show","list","update","edit","view","diff","review","link"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"add","aliases":[],"hidden":false,"deprecated":null,"path":["artifact","add"],"command":"planar artifact add","summary":"Register a new artifact.","description":"Register a new artifact.","subcommands":[],"flags":[{"long":"--body","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Artifact body text; @<file> reads it from a file","completion":{"kind":"none","values":[]},"env":null},{"long":"--kind","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Artifact kind, e.g. tech_spec, adr, design_note, research","completion":{"kind":"none","values":[]},"env":null},{"long":"--from-file","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Read the body from this file; also sets the source path","completion":{"kind":"none","values":[]},"env":null},{"long":"--source-path","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Path of the source file the artifact mirrors","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug to create in (default: cwd-derived write scope)","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":"draft","description":"Initial status: draft, active, superseded, retired (default draft)","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Attach to this plan id via a derives-from link","completion":{"kind":"none","values":[]},"env":null},{"long":"--editor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":true,"description":"Accepted for parity; not read by the handler","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"title","kind":"string","required":true,"default":null,"description":"Artifact title","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"show","aliases":[],"hidden":false,"deprecated":null,"path":["artifact","show"],"command":"planar artifact show","summary":"Show an artifact's metadata and body.","description":"Show an artifact's metadata and body.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"artifact-id","kind":"string","required":true,"default":null,"description":"Artifact id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"list","aliases":[],"hidden":false,"deprecated":null,"path":["artifact","list"],"command":"planar artifact list","summary":"List artifacts.","description":"List artifacts.","subcommands":[],"flags":[{"long":"--kind","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to a single artifact kind","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug to filter by (default: cwd-derived read set)","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Filter by status: draft, active, superseded, retired","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Restrict to artifacts attached to this plan id","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"update","aliases":[],"hidden":false,"deprecated":null,"path":["artifact","update"],"command":"planar artifact update","summary":"Update mutable fields on an artifact.","description":"Update mutable fields on an artifact.","subcommands":[],"flags":[{"long":"--title","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"New artifact title","completion":{"kind":"none","values":[]},"env":null},{"long":"--body","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"New body text; @<file> reads it from a file","completion":{"kind":"none","values":[]},"env":null},{"long":"--source-path","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"New source file path","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"New status: draft, active, superseded, retired","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Move the artifact to this scope slug","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"artifact-id","kind":"string","required":true,"default":null,"description":"Artifact id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"edit","aliases":[],"hidden":false,"deprecated":null,"path":["artifact","edit"],"command":"planar artifact edit","summary":"Edit an artifact in $EDITOR (editor-first flow).","description":"Edit an artifact in $EDITOR (editor-first flow).","subcommands":[],"flags":[{"long":"--no-pull","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Accepted for parity; the handler does not read it","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Accepted for parity; the handler does not read it","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"artifact-id","kind":"string","required":true,"default":null,"description":"Artifact id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"view","aliases":[],"hidden":false,"deprecated":null,"path":["artifact","view"],"command":"planar artifact view","summary":"View the artifact's workbench file in $PAGER.","description":"View the artifact's workbench file in $PAGER.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[{"name":"artifact-id","kind":"string","required":true,"default":null,"description":"Artifact id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"diff","aliases":[],"hidden":false,"deprecated":null,"path":["artifact","diff"],"command":"planar artifact diff","summary":"Show a unified diff between the DB's artifact content and the workbench file.","description":"Show a unified diff between the DB's artifact content and the workbench file.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[{"name":"artifact-id","kind":"string","required":true,"default":null,"description":"Artifact id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"review","aliases":[],"hidden":false,"deprecated":null,"path":["artifact","review"],"command":"planar artifact review","summary":"Reviewer entry point for artifact diff.","description":"Reviewer entry point for artifact diff.","subcommands":[],"flags":[{"long":"--approve","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Report an approve verdict on the pending workbench diff (not persisted)","completion":{"kind":"none","values":[]},"env":null},{"long":"--request-changes","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Report a request-changes verdict on the pending workbench diff (not persisted); excludes --approve","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"artifact-id","kind":"string","required":true,"default":null,"description":"Artifact id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"link","aliases":[],"hidden":false,"deprecated":null,"path":["artifact","link"],"command":"planar artifact link","summary":"Create an entity link from an artifact to another entity.","description":"Create an entity link from an artifact to another entity.","subcommands":[],"flags":[{"long":"--relationship","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Link relationship: derives-from, depends-on, addresses, verifies, cites, supersedes, touches","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Accepted but not read by this verb; no scope check is made","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"artifact-id","kind":"string","required":true,"default":null,"description":"Artifact id","completion":{"kind":"none","values":[]}},{"name":"ref","kind":"string","required":true,"default":null,"description":"Target entity ref (kind:id)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"document","aliases":[],"hidden":false,"deprecated":null,"path":["document"],"command":"planar document","summary":"Project and validate authoritative block documents.","description":"Project and validate authoritative block documents.","subcommands":["project","validate-range"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"project","aliases":[],"hidden":false,"deprecated":null,"path":["document","project"],"command":"planar document project","summary":"Emit an authoritative block-document-v1 projection.","description":"Emit an authoritative block-document-v1 projection.","subcommands":[],"flags":[{"long":"--kind","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Source entity kind: plan or artifact","completion":{"kind":"none","values":[]},"env":null},{"long":"--id","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"N","default":null,"description":"Source entity id","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"validate-range","aliases":[],"hidden":false,"deprecated":null,"path":["document","validate-range"],"command":"planar document validate-range","summary":"Validate an adjacent revision-bound passage range.","description":"Validate an adjacent revision-bound passage range.","subcommands":[],"flags":[{"long":"--kind","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Source entity kind: plan or artifact","completion":{"kind":"none","values":[]},"env":null},{"long":"--id","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"N","default":null,"description":"Source entity id","completion":{"kind":"none","values":[]},"env":null},{"long":"--content-revision","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Revision from the projection being validated against","completion":{"kind":"none","values":[]},"env":null},{"long":"--start-key","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Key of the passage where the range starts","completion":{"kind":"none","values":[]},"env":null},{"long":"--start-offset","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"N","default":null,"description":"Byte offset within the start passage","completion":{"kind":"none","values":[]},"env":null},{"long":"--end-key","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Key of the passage where the range ends","completion":{"kind":"none","values":[]},"env":null},{"long":"--end-offset","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"N","default":null,"description":"Byte offset within the end passage","completion":{"kind":"none","values":[]},"env":null},{"long":"--covered-key","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":true,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Covered passage key; repeat for each passage from start to end, in order","completion":{"kind":"none","values":[]},"env":null},{"long":"--segment-quote","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":true,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Projected text of one covered segment; repeat once per covered key","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"annotate","aliases":[],"hidden":false,"deprecated":null,"path":["annotate"],"command":"planar annotate","summary":"Manage source annotations.","description":"Manage line-anchored annotations on source code.\n\n  Status lifecycle: active → resolved / dismissed / archived.","subcommands":["add","show","list","capabilities","update","remove","tag","resolve","dismiss","archive","bulk-resolve","bulk-dismiss","bulk-archive","verify","sweep","command","receipt"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"add","aliases":[],"hidden":false,"deprecated":null,"path":["annotate","add"],"command":"planar annotate add","summary":"Create a new annotation.","description":"Create a new annotation.","subcommands":[],"flags":[{"long":"--anchor-path","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"File path the annotation is anchored to (required)","completion":{"kind":"none","values":[]},"env":null},{"long":"--line-start","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"First line of the anchored range","completion":{"kind":"none","values":[]},"env":null},{"long":"--line-end","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Last line of the anchored range","completion":{"kind":"none","values":[]},"env":null},{"long":"--commit-sha","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Commit the anchored content was captured at, for later verification","completion":{"kind":"none","values":[]},"env":null},{"long":"--text-hash","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Hash of the anchored text, for later drift detection","completion":{"kind":"none","values":[]},"env":null},{"long":"--text","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Annotation text","completion":{"kind":"none","values":[]},"env":null},{"long":"--title","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Annotation title","completion":{"kind":"none","values":[]},"env":null},{"long":"--slug","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Annotation slug","completion":{"kind":"none","values":[]},"env":null},{"long":"--body","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Annotation body (literal text)","completion":{"kind":"none","values":[]},"env":null},{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Vendor that authored the annotation","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Plan id the annotation is associated with","completion":{"kind":"none","values":[]},"env":null},{"long":"--task","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Task id the annotation is associated with","completion":{"kind":"none","values":[]},"env":null},{"long":"--tags","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Comma-separated tags","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug the annotation is created under","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"show","aliases":[],"hidden":false,"deprecated":null,"path":["annotate","show"],"command":"planar annotate show","summary":"Show an annotation.","description":"Show an annotation.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"annotation-id","kind":"string","required":true,"default":null,"description":"Annotation id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"list","aliases":[],"hidden":false,"deprecated":null,"path":["annotate","list"],"command":"planar annotate list","summary":"List annotations.","description":"List annotations.","subcommands":[],"flags":[{"long":"--anchor-path","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to this anchor path","completion":{"kind":"none","values":[]},"env":null},{"long":"--anchor-kind","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to an anchor kind: file, entity","completion":{"kind":"none","values":[]},"env":null},{"long":"--target-kind","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to a target kind: plan, task (needs --target-id)","completion":{"kind":"none","values":[]},"env":null},{"long":"--target-id","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Restrict to this target id (needs --target-kind)","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to a status: active, resolved, dismissed, archived","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Restrict to annotations on this plan id","completion":{"kind":"none","values":[]},"env":null},{"long":"--task","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Restrict to annotations on this task id","completion":{"kind":"none","values":[]},"env":null},{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to annotations by this vendor","completion":{"kind":"none","values":[]},"env":null},{"long":"--tag","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to annotations carrying this tag","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to this scope slug","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"capabilities","aliases":[],"hidden":false,"deprecated":null,"path":["annotate","capabilities"],"command":"planar annotate capabilities","summary":"Describe annotation read and command support.","description":"Describe annotation read and command support.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"update","aliases":[],"hidden":false,"deprecated":null,"path":["annotate","update"],"command":"planar annotate update","summary":"Update an annotation.","description":"Update an annotation.","subcommands":[],"flags":[{"long":"--title","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Annotation title","completion":{"kind":"none","values":[]},"env":null},{"long":"--slug","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Annotation slug","completion":{"kind":"none","values":[]},"env":null},{"long":"--body","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Annotation body (literal text)","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"New status: active, resolved, dismissed, archived","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Plan id the annotation is associated with","completion":{"kind":"none","values":[]},"env":null},{"long":"--task","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Task id the annotation is associated with","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Move the annotation to this scope slug (patch field)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"annotation-id","kind":"string","required":true,"default":null,"description":"Annotation id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"remove","aliases":[],"hidden":false,"deprecated":null,"path":["annotate","remove"],"command":"planar annotate remove","summary":"Remove an annotation.","description":"Remove an annotation.","subcommands":[],"flags":[{"long":"--expected-revision","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Annotation revision the caller last saw (required); mismatch refuses","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"annotation-id","kind":"string","required":true,"default":null,"description":"Annotation id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"tag","aliases":[],"hidden":false,"deprecated":null,"path":["annotate","tag"],"command":"planar annotate tag","summary":"Add or remove a tag on an annotation.","description":"Add or remove a tag on an annotation.","subcommands":[],"flags":[{"long":"--remove","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Remove the tag instead of adding it","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"annotation-id","kind":"string","required":true,"default":null,"description":"Annotation id","completion":{"kind":"none","values":[]}},{"name":"tag","kind":"string","required":true,"default":null,"description":"Tag to add or remove","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"resolve","aliases":[],"hidden":false,"deprecated":null,"path":["annotate","resolve"],"command":"planar annotate resolve","summary":"Mark an annotation as resolved.","description":"Mark an annotation as resolved.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"annotation-id","kind":"string","required":true,"default":null,"description":"Annotation id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"dismiss","aliases":[],"hidden":false,"deprecated":null,"path":["annotate","dismiss"],"command":"planar annotate dismiss","summary":"Dismiss an annotation.","description":"Dismiss an annotation.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"annotation-id","kind":"string","required":true,"default":null,"description":"Annotation id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"archive","aliases":[],"hidden":false,"deprecated":null,"path":["annotate","archive"],"command":"planar annotate archive","summary":"Archive an annotation.","description":"Archive an annotation.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"annotation-id","kind":"string","required":true,"default":null,"description":"Annotation id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"bulk-resolve","aliases":[],"hidden":false,"deprecated":null,"path":["annotate","bulk-resolve"],"command":"planar annotate bulk-resolve","summary":"Resolve every active annotation matching the filter.","description":"Resolve every active annotation matching the filter.","subcommands":[],"flags":[{"long":"--operation-id","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Operation UUID; runs as a receipt-backed command and records an aggregate receipt","completion":{"kind":"none","values":[]},"env":null},{"long":"--anchor-path","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"File path the annotation is anchored to","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Plan id the annotation is associated with","completion":{"kind":"none","values":[]},"env":null},{"long":"--task","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Task id the annotation is associated with","completion":{"kind":"none","values":[]},"env":null},{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Vendor that authored the annotation","completion":{"kind":"none","values":[]},"env":null},{"long":"--tag","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Tag name","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug to resolve against instead of the cwd-derived scope","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"bulk-dismiss","aliases":[],"hidden":false,"deprecated":null,"path":["annotate","bulk-dismiss"],"command":"planar annotate bulk-dismiss","summary":"Dismiss every active annotation matching the filter.","description":"Dismiss every active annotation matching the filter.","subcommands":[],"flags":[{"long":"--operation-id","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Operation UUID; runs as a receipt-backed command and records an aggregate receipt","completion":{"kind":"none","values":[]},"env":null},{"long":"--anchor-path","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"File path the annotation is anchored to","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Plan id the annotation is associated with","completion":{"kind":"none","values":[]},"env":null},{"long":"--task","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Task id the annotation is associated with","completion":{"kind":"none","values":[]},"env":null},{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Vendor that authored the annotation","completion":{"kind":"none","values":[]},"env":null},{"long":"--tag","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Tag name","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug to resolve against instead of the cwd-derived scope","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"bulk-archive","aliases":[],"hidden":false,"deprecated":null,"path":["annotate","bulk-archive"],"command":"planar annotate bulk-archive","summary":"Archive every annotation matching the filter (including non-active rows).","description":"Archive every annotation matching the filter (including non-active rows).","subcommands":[],"flags":[{"long":"--operation-id","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Operation UUID; runs as a receipt-backed command and records an aggregate receipt","completion":{"kind":"none","values":[]},"env":null},{"long":"--anchor-path","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"File path the annotation is anchored to","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Plan id the annotation is associated with","completion":{"kind":"none","values":[]},"env":null},{"long":"--task","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Task id the annotation is associated with","completion":{"kind":"none","values":[]},"env":null},{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Vendor that authored the annotation","completion":{"kind":"none","values":[]},"env":null},{"long":"--tag","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Tag name","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug to resolve against instead of the cwd-derived scope","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"verify","aliases":[],"hidden":false,"deprecated":null,"path":["annotate","verify"],"command":"planar annotate verify","summary":"Verify annotation anchors against workspace state.","description":"Verify annotation anchors against workspace state.","subcommands":[],"flags":[{"long":"--anchor-path","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Verify only annotations anchored to this path","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to this scope slug","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"sweep","aliases":[],"hidden":false,"deprecated":null,"path":["annotate","sweep"],"command":"planar annotate sweep","summary":"Sweep stale annotations (resolved/dismissed older than --since-days).","description":"Sweep stale annotations (resolved/dismissed older than --since-days).","subcommands":[],"flags":[{"long":"--since-days","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":30,"description":"Archive resolved or dismissed annotations older than this many days","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict the sweep to this scope slug","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"command","aliases":[],"hidden":false,"deprecated":null,"path":["annotate","command"],"command":"planar annotate command","summary":"Apply a receipt-backed annotation JSON request from stdin (--request @-).","description":"Apply a receipt-backed annotation JSON request from stdin (--request @-).","subcommands":[],"flags":[{"long":"--request","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Request source; only @- (JSON object on stdin) is accepted","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"receipt","aliases":[],"hidden":false,"deprecated":null,"path":["annotate","receipt"],"command":"planar annotate receipt","summary":"Look up a durable annotation command receipt.","description":"Look up a durable annotation command receipt.","subcommands":[],"flags":[{"long":"--source-uuid","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Source UUID of the command (required)","completion":{"kind":"none","values":[]},"env":null},{"long":"--operation-id","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Operation UUID of the command (required)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"promote","aliases":[],"hidden":false,"deprecated":null,"path":["promote"],"command":"planar promote","summary":"Promote an entity to an association scope.","description":"Promote an entity from its current scope to a named association.\n\n  Valid entity kinds: plan, task, question, test_scenario (alias:\n  scenario), artifact, decision.\n\n  Examples:\n    planar promote task:42 --to org:acme\n    planar promote plan:7 --to project:planar","subcommands":[],"flags":[{"long":"--to","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Target association slug","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"ref","kind":"string","required":true,"default":null,"description":"Entity ref (kind:id)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"demote","aliases":[],"hidden":false,"deprecated":null,"path":["demote"],"command":"planar demote","summary":"Demote an entity back to global scope.","description":"Reverse a promotion — move an entity back to global personal scope.\n\n  The destination is always global; the optional --from flag names the\n  source association slug for clarity. Association-to-association\n  transitions go through promote.\n\n  Example:\n    planar demote task:42 --from project:planar","subcommands":[],"flags":[{"long":"--from","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Accepted but ignored; demotion always targets global","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"ref","kind":"string","required":true,"default":null,"description":"Entity ref (kind:id)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"workbench","aliases":[],"hidden":false,"deprecated":null,"path":["workbench"],"command":"planar workbench","summary":"Manage workbench sync for plan feature directories.","description":"Manage the bidirectional sync surface between the workbench\n  filesystem and the Planar database.\n\n  The workbench root resolution order (highest to lowest priority):\n    1. $PLANAR_WORKBENCH_ROOT env var\n    2. workbench.root in $PLANAR_CONFIG_PATH or ~/.planar/config.toml\n    3. Default: ~/.planar/workbench/","subcommands":["lint","pull","push","status","resolve","sync","archive","restore","gc","list","publish","extract-questions","edit"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"lint","aliases":[],"hidden":false,"deprecated":null,"path":["workbench","lint"],"command":"planar workbench lint","summary":"Validate workbench Markdown frontmatter without syncing.","description":"Validate workbench Markdown frontmatter without syncing.","subcommands":[],"flags":[{"long":"--all","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Validate every workbench tree","completion":{"kind":"none","values":[]},"env":null},{"long":"--path","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Validate one Markdown file or directory","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan","kind":"string","required":false,"default":null,"description":"Plan id or slug (or use --all / --path)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"pull","aliases":[],"hidden":false,"deprecated":null,"path":["workbench","pull"],"command":"planar workbench pull","summary":"Apply FS→DB changes; report DB→FS drift.","description":"Apply FS→DB changes; report DB→FS drift.","subcommands":[],"flags":[{"long":"--verbose","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Verbose text rendering of the pull result","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan","kind":"string","required":true,"default":null,"description":"Plan id or slug","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"push","aliases":[],"hidden":false,"deprecated":null,"path":["workbench","push"],"command":"planar workbench push","summary":"Apply DB→FS changes atomically; report FS→DB drift.","description":"Apply DB→FS changes atomically; report FS→DB drift.","subcommands":[],"flags":[{"long":"--verbose","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Verbose text rendering of the push result","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null},{"long":"--filter-mode","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Terminal-status filter: 'failures' (default) or 'all'","completion":{"kind":"none","values":[]},"env":null},{"long":"--apply-cleanup","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Remove pre-existing FS files for entities this push would have filtered","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan","kind":"string","required":true,"default":null,"description":"Plan id or slug","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"status","aliases":[],"hidden":false,"deprecated":null,"path":["workbench","status"],"command":"planar workbench status","summary":"Show drift and conflicts without writing.","description":"Show drift and conflicts without writing.","subcommands":[],"flags":[{"long":"--verbose","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Verbose text rendering of the status report","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan","kind":"string","required":false,"default":null,"description":"Plan id or slug (omit for every plan with a tree)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"resolve","aliases":[],"hidden":false,"deprecated":null,"path":["workbench","resolve"],"command":"planar workbench resolve","summary":"Settle a sync conflict by choosing FS or DB.","description":"Settle a sync conflict by choosing FS or DB.","subcommands":[],"flags":[{"long":"--prefer","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Which side to prefer (fs|db)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"event-id","kind":"string","required":true,"default":null,"description":"Conflict event id (sync_events.id)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"sync","aliases":[],"hidden":false,"deprecated":null,"path":["workbench","sync"],"command":"planar workbench sync","summary":"Atomically apply FS and DB changes via a unified sync.","description":"Atomically apply FS and DB changes via a unified sync.","subcommands":[],"flags":[{"long":"--verbose","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Verbose text rendering of the sync result","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan","kind":"string","required":true,"default":null,"description":"Plan id or slug","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"archive","aliases":[],"hidden":false,"deprecated":null,"path":["workbench","archive"],"command":"planar workbench archive","summary":"Archive a feature's workbench filesystem tree.","description":"Archive a feature's workbench filesystem tree.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null},{"long":"--filter-mode","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Terminal-status filter: 'failures' (default) or 'all'","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan","kind":"string","required":true,"default":null,"description":"Plan id or slug","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"restore","aliases":[],"hidden":false,"deprecated":null,"path":["workbench","restore"],"command":"planar workbench restore","summary":"Restore an archived feature's workbench tree.","description":"Restore an archived feature's workbench tree.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null},{"long":"--filter-mode","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Terminal-status filter: 'failures' (default) or 'all'","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan","kind":"string","required":true,"default":null,"description":"Plan id or slug","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"gc","aliases":[],"hidden":false,"deprecated":null,"path":["workbench","gc"],"command":"planar workbench gc","summary":"Remove FS files whose backing entity is terminal in the DB.","description":"Remove FS files whose backing entity is terminal in the DB.","subcommands":[],"flags":[{"long":"--dry-run","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Preview only; do not touch disk","completion":{"kind":"none","values":[]},"env":null},{"long":"--yes","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Discard FS-content drift; remove drifted files anyway","completion":{"kind":"none","values":[]},"env":null},{"long":"--filter-mode","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Terminal-status filter: 'failures' (default) or 'all'","completion":{"kind":"none","values":[]},"env":null},{"long":"--all-scopes","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Walk every plan's workbench tree","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan","kind":"string","required":false,"default":null,"description":"Plan id or slug (omit with --all-scopes)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"list","aliases":[],"hidden":false,"deprecated":null,"path":["workbench","list"],"command":"planar workbench list","summary":"List features with workbench trees.","description":"List features with workbench trees.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"publish","aliases":[],"hidden":false,"deprecated":null,"path":["workbench","publish"],"command":"planar workbench publish","summary":"Render and push workbench files to external system.","description":"Render and push workbench files to external system.","subcommands":[],"flags":[{"long":"--system","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Slug of the registered external system to publish to","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan","kind":"string","required":true,"default":null,"description":"Anchor plan id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"extract-questions","aliases":[],"hidden":false,"deprecated":null,"path":["workbench","extract-questions"],"command":"planar workbench extract-questions","summary":"Parse Open questions from top-level workbench specs (read-only).","description":"Parse Open questions from top-level workbench specs (read-only).","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan","kind":"string","required":true,"default":null,"description":"Anchor plan id or slug","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"edit","aliases":[],"hidden":false,"deprecated":null,"path":["workbench","edit"],"command":"planar workbench edit","summary":"Edit a feature's workbench files in $EDITOR.","description":"Edit a feature's workbench files in $EDITOR.","subcommands":[],"flags":[{"long":"--editor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":"","description":"Editor command to open the workbench files with","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan","kind":"string","required":true,"default":null,"description":"Plan id or slug","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"workspace","aliases":[],"hidden":false,"deprecated":null,"path":["workspace"],"command":"planar workspace","summary":"Manage workspace state directories and their AGENTS.md surfaces.","description":"Workspace administration.\n\n  A workspace is identified by an associations row of kind=org. Each\n  workspace owns a state directory under\n  ${PLANAR_HOME:-~/.planar}/workspaces/<org_id>/ holding the canonical\n  AGENTS.md surface for the org.","subcommands":["init","doctor","routing","regenerate"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"init","aliases":[],"hidden":false,"deprecated":null,"path":["workspace","init"],"command":"planar workspace init","summary":"Initialize a workspace (org-level association).","description":"Initialize a workspace (org-level association).","subcommands":[],"flags":[{"long":"--name","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Human-readable workspace name (default: the directory name)","completion":{"kind":"none","values":[]},"env":null},{"long":"--slug","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Workspace slug (default: derived from the directory name)","completion":{"kind":"none","values":[]},"env":null},{"long":"--scan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":1,"description":"Directory levels to scan for child repos (default: 1)","completion":{"kind":"none","values":[]},"env":null},{"long":"--meta-repo","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Treat the cwd git repository as a workspace container and member project","completion":{"kind":"none","values":[]},"env":null},{"long":"--no-scan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Skip the post-init pipeline (routing build, AGENTS.md regenerate, symlinks)","completion":{"kind":"none","values":[]},"env":null},{"long":"--enrich","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Merge cached LLM enrichment results into the routing table; cannot combine with --no-scan","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"doctor","aliases":[],"hidden":false,"deprecated":null,"path":["workspace","doctor"],"command":"planar workspace doctor","summary":"Scan and fix workspace registration and state consistency.","description":"Scan and fix workspace registration and state consistency.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"routing","aliases":[],"hidden":false,"deprecated":null,"path":["workspace","routing"],"command":"planar workspace routing","summary":"Manage workspace routing table.","description":"Manage workspace routing table.","subcommands":["build","show"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"build","aliases":[],"hidden":false,"deprecated":null,"path":["workspace","routing","build"],"command":"planar workspace routing build","summary":"Build routing table from workspace membership.","description":"Build routing table from workspace membership.","subcommands":[],"flags":[{"long":"--enrich","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Merge cached LLM enrichment results into the routing table","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"workspace","kind":"string","required":false,"default":null,"description":"Workspace to build the routing table for (default: the cwd-derived workspace)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"show","aliases":[],"hidden":false,"deprecated":null,"path":["workspace","routing","show"],"command":"planar workspace routing show","summary":"Display current routing table.","description":"Display current routing table.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"workspace","kind":"string","required":false,"default":null,"description":"Workspace whose routing table to show (default: the cwd-derived workspace)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"regenerate","aliases":[],"hidden":false,"deprecated":null,"path":["workspace","regenerate"],"command":"planar workspace regenerate","summary":"Regenerate AGENTS.md from current state.","description":"Regenerate AGENTS.md from current state.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"workspace","kind":"string","required":false,"default":null,"description":"Workspace to render AGENTS.md for (default: the cwd-derived workspace)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"link","aliases":[],"hidden":false,"deprecated":null,"path":["link"],"command":"planar link","summary":"Link a local entity to an external-system ticket.","description":"Manually record an external_links row linking a local entity to\n  an already-existing external ticket. Use this when the external\n  ticket was created outside of 'ext create'. Does not push any data\n  to the external system.\n\n  <kind:id> is a local entity reference, e.g. task:42, plan:7.","subcommands":[],"flags":[{"long":"--to","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"<system-slug>:<external-id>","completion":{"kind":"none","values":[]},"env":null},{"long":"--role","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Link role: mirror, parent, child, reference (default: reference)","completion":{"kind":"none","values":[]},"env":null},{"long":"--sync","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Sync direction: read-only, write-back, two-way (default: read-only)","completion":{"kind":"none","values":[]},"env":null},{"long":"--propagate","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Not implemented; refuses with exit 6 (use `planar-ext ext propagate`)","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Accepted but not read by this verb; no scope check is made","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"ref","kind":"string","required":true,"default":null,"description":"Entity ref (kind:id)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"unlink","aliases":[],"hidden":false,"deprecated":null,"path":["unlink"],"command":"planar unlink","summary":"Remove an external_links row by link id.","description":"Remove an external_links row by its link id.\n\n  Associated sync_events rows are detached by setting link_id to null\n  rather than cascade-deleted; they are no longer reachable through\n  the deleted link's audit trail.","subcommands":[],"flags":[{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope for the cross-scope guard (currently informational)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"link-id","kind":"string","required":true,"default":null,"description":"External-link id (integer)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"links","aliases":[],"hidden":false,"deprecated":null,"path":["links"],"command":"planar links","summary":"List or remove internal entity_links relationships.","description":"Manage internal cross-cutting entity_links relationships.\n\n  Entity links record typed relationships between any two Planar\n  entities (e.g. a task cites an artifact, a plan blocks another\n  plan). This domain is distinct from the top-level link/unlink\n  commands, which operate on external-system ticket linkage.","subcommands":["add","list","remove","trail"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"add","aliases":[],"hidden":false,"deprecated":null,"path":["links","add"],"command":"planar links add","summary":"Create an entity_links row between two entities.","description":"Create an entity_links row between two entities.","subcommands":[],"flags":[{"long":"--relationship","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Relationship kind: derives-from, depends-on, addresses, verifies, cites, supersedes, touches","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"from-ref","kind":"string","required":true,"default":null,"description":"Source entity ref (kind:id)","completion":{"kind":"none","values":[]}},{"name":"to-ref","kind":"string","required":true,"default":null,"description":"Target entity ref (kind:id)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"list","aliases":[],"hidden":false,"deprecated":null,"path":["links","list"],"command":"planar links list","summary":"List entity_links where the given entity is source or target.","description":"List entity_links where the given entity is source or target.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"ref","kind":"string","required":true,"default":null,"description":"Entity ref (kind:id)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"remove","aliases":[],"hidden":false,"deprecated":null,"path":["links","remove"],"command":"planar links remove","summary":"Delete an entity_links row by its id.","description":"Delete an entity_links row by its id.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"link-id","kind":"string","required":true,"default":null,"description":"Entity link id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"trail","aliases":[],"hidden":false,"deprecated":null,"path":["links","trail"],"command":"planar links trail","summary":"Show the audit trail for an entity_links row.","description":"Show the audit trail for an entity_links row.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"link-id","kind":"string","required":true,"default":null,"description":"Entity link id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"resume","aliases":[],"hidden":false,"deprecated":null,"path":["resume"],"command":"planar resume","summary":"Produce a structured resume packet for the specified task.","description":"Produce a structured 8-section resume packet for the specified\n  task.\n\n  The packet contains:\n    1. Identity       — task id, plan id, title, scope\n    2. State          — status, next_action, last action\n    3. Plan position  — parent plan, completed/current/remaining steps\n    4. Operational    — external_links for the task; refreshed if stale\n    5. Recent activity — session entries from recent sessions\n    6. Decisions and questions\n    7. Linked artifacts\n    8. Audit footer   — previous session vendor and timestamp, plus\n                        the active claim's worktree path (when held)\n                        so the resumer can prepend `cd <path>`","subcommands":["validate"],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"task-id","kind":"string","required":false,"default":null,"description":"Task id (default: derived from the cwd read scope)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"validate","aliases":[],"hidden":false,"deprecated":null,"path":["resume","validate"],"command":"planar resume validate","summary":"Check if a task is resumable.","description":"Check if a task is resumable.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"inherited","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"task-id","kind":"string","required":true,"default":null,"description":"Task id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"handoff","aliases":[],"hidden":false,"deprecated":null,"path":["handoff"],"command":"planar handoff","summary":"Capture a context snapshot and create a validated handoff record.","description":"Capture a context snapshot for the current session and atomically:\n    1. Insert a context_snapshots row.\n    2. Insert a handoffs row with status='pending'.\n    3. Validate the handoff (pending → validated, validated_at set).\n\n  Subcommands manage the handoff lifecycle: create / validate /\n  consume / abandon / list / show.","subcommands":["create","validate","consume","abandon","list","show"],"flags":[{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Expected destination vendor (stored as the handoff's to_vendor)","completion":{"kind":"none","values":[]},"env":null},{"long":"--note","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Free-form handoff message; @<file> reads it from a file","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"task-id","kind":"string","required":false,"default":null,"description":"Task id to hand off (optional)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"create","aliases":[],"hidden":false,"deprecated":null,"path":["handoff","create"],"command":"planar handoff create","summary":"Create a handoff from an existing snapshot.","description":"Create a handoff from an existing snapshot.","subcommands":[],"flags":[{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"inherited","valueName":"VALUE","default":null,"description":"Expected destination vendor (stored as the handoff's to_vendor)","completion":{"kind":"none","values":[]},"env":null},{"long":"--note","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"inherited","valueName":"VALUE","default":null,"description":"Free-form handoff message; @<file> reads it from a file","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"inherited","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"snapshot-id","kind":"string","required":true,"default":null,"description":"Context snapshot id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"validate","aliases":[],"hidden":false,"deprecated":null,"path":["handoff","validate"],"command":"planar handoff validate","summary":"Validate a pending handoff.","description":"Validate a pending handoff.","subcommands":[],"flags":[{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"inherited","valueName":"VALUE","default":null,"description":"Expected destination vendor (stored as the handoff's to_vendor)","completion":{"kind":"none","values":[]},"env":null},{"long":"--note","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"inherited","valueName":"VALUE","default":null,"description":"Free-form handoff message; @<file> reads it from a file","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"inherited","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"handoff-id","kind":"string","required":true,"default":null,"description":"Handoff id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"consume","aliases":[],"hidden":false,"deprecated":null,"path":["handoff","consume"],"command":"planar handoff consume","summary":"Mark a handoff as consumed.","description":"Mark a handoff as consumed.","subcommands":[],"flags":[{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"inherited","valueName":"VALUE","default":null,"description":"Expected destination vendor (stored as the handoff's to_vendor)","completion":{"kind":"none","values":[]},"env":null},{"long":"--note","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"inherited","valueName":"VALUE","default":null,"description":"Free-form handoff message; @<file> reads it from a file","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"inherited","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null},{"long":"--session","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Session id of the resuming session consuming the handoff","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"handoff-id","kind":"string","required":true,"default":null,"description":"Handoff id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"abandon","aliases":[],"hidden":false,"deprecated":null,"path":["handoff","abandon"],"command":"planar handoff abandon","summary":"Abandon a non-terminal handoff.","description":"Abandon a non-terminal handoff.","subcommands":[],"flags":[{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"inherited","valueName":"VALUE","default":null,"description":"Expected destination vendor (stored as the handoff's to_vendor)","completion":{"kind":"none","values":[]},"env":null},{"long":"--note","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"inherited","valueName":"VALUE","default":null,"description":"Free-form handoff message; @<file> reads it from a file","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"inherited","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null},{"long":"--reason","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Reason; accepted but not stored","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"handoff-id","kind":"string","required":true,"default":null,"description":"Handoff id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"list","aliases":[],"hidden":false,"deprecated":null,"path":["handoff","list"],"command":"planar handoff list","summary":"List handoffs.","description":"List handoffs.","subcommands":[],"flags":[{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"inherited","valueName":"VALUE","default":null,"description":"Expected destination vendor (stored as the handoff's to_vendor)","completion":{"kind":"none","values":[]},"env":null},{"long":"--note","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"inherited","valueName":"VALUE","default":null,"description":"Free-form handoff message; @<file> reads it from a file","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"inherited","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Filter by handoff status: pending, validated, consumed, abandoned","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"show","aliases":[],"hidden":false,"deprecated":null,"path":["handoff","show"],"command":"planar handoff show","summary":"Show a handoff's details.","description":"Show a handoff's details.","subcommands":[],"flags":[{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"inherited","valueName":"VALUE","default":null,"description":"Expected destination vendor (stored as the handoff's to_vendor)","completion":{"kind":"none","values":[]},"env":null},{"long":"--note","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"inherited","valueName":"VALUE","default":null,"description":"Free-form handoff message; @<file> reads it from a file","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"inherited","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"handoff-id","kind":"string","required":true,"default":null,"description":"Handoff id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"capture","aliases":[],"hidden":false,"deprecated":null,"path":["capture"],"command":"planar capture","summary":"Manage explicit session capture.","description":"Capture commands manage explicit session management and context\n  capture.\n\n  Automatic capture happens on every write command; use these\n  subcommands for explicit session management, narrative notes,\n  command history, and snapshots.","subcommands":["session","commits","end","note","command","file","snapshot"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"session","aliases":[],"hidden":false,"deprecated":null,"path":["capture","session"],"command":"planar capture session","summary":"Open or reuse a session for the current (vendor, vendor-session-id) tuple.","description":"Open or reuse a session for the current (vendor, vendor-session-id) tuple.","subcommands":[],"flags":[{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Vendor identity (default: $PLANAR_VENDOR, else cli)","completion":{"kind":"none","values":[]},"env":null},{"long":"--vendor-session-id","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Vendor-specific session id (default: $PLANAR_VENDOR_SESSION_ID)","completion":{"kind":"none","values":[]},"env":null},{"long":"--model","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Free-form model identifier for the session","completion":{"kind":"none","values":[]},"env":null},{"long":"--task","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Task id to associate with the session","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"commits","aliases":[],"hidden":false,"deprecated":null,"path":["capture","commits"],"command":"planar capture commits","summary":"Record explicit git commits into a session.","description":"Record explicit git commits into a session.","subcommands":[],"flags":[{"long":"--session","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Record into this session, ended ones allowed (default: the active session)","completion":{"kind":"none","values":[]},"env":null},{"long":"--repo","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Directory of the repo to read commits from (default: .)","completion":{"kind":"none","values":[]},"env":null},{"long":"--since","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Record every commit in <ref>..HEAD; exclusive with positional SHAs","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"end","aliases":[],"hidden":false,"deprecated":null,"path":["capture","end"],"command":"planar capture end","summary":"End the active or specified session.","description":"End the active or specified session.","subcommands":[],"flags":[{"long":"--session","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Session id to end; the positional wins when both are given","completion":{"kind":"none","values":[]},"env":null},{"long":"--summary","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Session summary text; may be @<file>","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"session-id","kind":"string","required":false,"default":null,"description":"Session id to end","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"note","aliases":[],"hidden":false,"deprecated":null,"path":["capture","note"],"command":"planar capture note","summary":"Append a narrative note to the active session.","description":"Append a narrative note to the active session.","subcommands":[],"flags":[{"long":"--session","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Session id (default: the vendor tuple's active session)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"body","kind":"string","required":true,"default":null,"description":"Note text; may be @<file>","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"command","aliases":[],"hidden":false,"deprecated":null,"path":["capture","command"],"command":"planar capture command","summary":"Append a command to the active session.","description":"Append a command to the active session.","subcommands":[],"flags":[{"long":"--session","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Session id (default: the vendor tuple's active session)","completion":{"kind":"none","values":[]},"env":null},{"long":"--outcome","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Summary of the command outcome; may be @<file>","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"command","kind":"string","required":true,"default":null,"description":"Command that was run","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"file","aliases":[],"hidden":false,"deprecated":null,"path":["capture","file"],"command":"planar capture file","summary":"Attach a file to the active session.","description":"Attach a file to the active session.","subcommands":[],"flags":[{"long":"--session","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Session id (default: the vendor tuple's active session)","completion":{"kind":"none","values":[]},"env":null},{"long":"--role","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Role of the file, e.g. implementation target or read for context","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"path","kind":"string","required":true,"default":null,"description":"Path of the file that was touched","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"snapshot","aliases":[],"hidden":false,"deprecated":null,"path":["capture","snapshot"],"command":"planar capture snapshot","summary":"Create a context snapshot.","description":"Create a context snapshot.","subcommands":[],"flags":[{"long":"--session","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Session id to snapshot against (default: the vendor tuple's active session)","completion":{"kind":"none","values":[]},"env":null},{"long":"--task","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Task id for the snapshot (default: the session's bound task)","completion":{"kind":"none","values":[]},"env":null},{"long":"--note","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Snapshot body; may be @<file>; wins over the body positional","completion":{"kind":"none","values":[]},"env":null},{"long":"--next-action","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Next action to record in the snapshot","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"body","kind":"string","required":false,"default":null,"description":"Snapshot body; may be @<file>","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"audit","aliases":[],"hidden":false,"deprecated":null,"path":["audit"],"command":"planar audit","summary":"Cross-plane audit trail commands.","description":"Cross-plane audit trail commands.\n\n  Subcommands inspect external-link history, query attributed session\n  commits, recompute decision publication targets, render session\n  timelines, and walk the full audit trail for any external link.","subcommands":["trail","commits","session","publish-decision","handoff-readiness"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"trail","aliases":[],"hidden":false,"deprecated":null,"path":["audit","trail"],"command":"planar audit trail","summary":"Show audit history for an entity (audit_log + entity_links) or an external link (external_links + sync_events).","description":"Show audit history for an entity (audit_log + entity_links) or an external link (external_links + sync_events).","subcommands":[],"flags":[{"long":"--kind","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Entity kind of the positional id (default: task)","completion":{"kind":"none","values":[]},"env":null},{"long":"--grep","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Search pattern; switches to a query that does not widen through entity links","completion":{"kind":"none","values":[]},"env":null},{"long":"--link","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"External link id; switches to link-scoped (external_links + sync_events) form","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"entity-id","kind":"string","required":false,"default":null,"description":"Entity id to show the audit history of","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"commits","aliases":[],"hidden":false,"deprecated":null,"path":["audit","commits"],"command":"planar audit commits","summary":"List commits attributed to sessions and claims.","description":"List commits attributed to sessions and claims.","subcommands":[],"flags":[{"long":"--session","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Only commits recorded for this session id","completion":{"kind":"none","values":[]},"env":null},{"long":"--task","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Only commits recorded for claims on this task id","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null},{"long":"--shas","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit bare commit SHAs, one per line; mutually exclusive with --json","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"session","aliases":[],"hidden":false,"deprecated":null,"path":["audit","session"],"command":"planar audit session","summary":"Show the timeline for a session.","description":"Show the timeline for a session.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"session-id","kind":"string","required":true,"default":null,"description":"Session id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"publish-decision","aliases":[],"hidden":false,"deprecated":null,"path":["audit","publish-decision"],"command":"planar audit publish-decision","summary":"Post the decision body to linked operational-plane targets.","description":"Post the decision body to linked operational-plane targets.","subcommands":[],"flags":[{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug to resolve against instead of the cwd-derived scope","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"decision-id","kind":"string","required":true,"default":null,"description":"Decision id to publish","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"handoff-readiness","aliases":[],"hidden":false,"deprecated":null,"path":["audit","handoff-readiness"],"command":"planar audit handoff-readiness","summary":"Check resume-readiness for all in-flight tasks.","description":"Check resume-readiness for all in-flight tasks.","subcommands":[],"flags":[{"long":"--threshold","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":90,"description":"Minimum percent of in-flight tasks that must be resume-ready; exits 1 below it (default: 90)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"health","aliases":[],"hidden":false,"deprecated":null,"path":["health"],"command":"planar health","summary":"Report database, handoff, and installed-projection health.","description":"Check database reachability, schema version currency, SQLite\n  integrity, in-flight task resumability, pending handoff staleness,\n  and manifest-owned installed projection freshness. This command is\n  read-only; recovery commands are reported but never run.\n\n  Exit codes:\n    0  all checks pass\n    1  degraded (in-flight tasks not resumable, stale handoffs, stale\n       or missing managed projections, integrity errors, etc.)","subcommands":["hygiene"],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"hygiene","aliases":[],"hidden":false,"deprecated":null,"path":["health","hygiene"],"command":"planar health hygiene","summary":"Report stale plan, task, and question lifecycle state without mutating it.","description":"Find draft plans with zero tasks or only terminal tasks, tasks\n  left doing beyond a threshold, and questions left open beyond a\n  threshold. Suggested repair commands are reported but never run.\n\n  This reporter always exits 0 when the report is produced, even when\n  findings are present.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"inherited","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Limit findings to one association slug","completion":{"kind":"none","values":[]},"env":null},{"long":"--stale-doing","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":7,"description":"Doing-task age threshold in days","completion":{"kind":"none","values":[]},"env":null},{"long":"--stale-open","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":30,"description":"Open-question age threshold in days","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"models","aliases":[],"hidden":false,"deprecated":null,"path":["models"],"command":"planar models","summary":"Discover installed provider CLIs and their model catalogs.","description":"Probe the supported provider CLIs (claude, codex) for\n  installed-state + version, and report their curated model\n  catalogs and the default role→tier→model routing.\n\n  The provider CLIs do not expose a machine-readable model list,\n  so the per-vendor model catalog is curated in-repo; discovery\n  confirms which CLIs are callable on this machine.\n\n  Subcommands:\n    list       Probe + print (read-only).\n    refresh    Probe + print, and write the cache to\n               ${PLANAR_HOME:-~/.planar}/models/catalog.json.","subcommands":["evals","resolve","experiments","outcomes","registry"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"evals","aliases":[],"hidden":false,"deprecated":null,"path":["models","evals"],"command":"planar models evals","summary":"Aggregate completed dispatch outcomes into a per-(work-type, candidate) scorecard and preview-only recommendation.","description":"Evidence-backed candidate ranking over declared-experiment\n  terminal samples in the exact cohort (plan 950 task 5530). Supply the\n  cohort flags to rank: results report sample and success counts, the raw\n  rate, the 95% Wilson lower bound, gate-failure rate, and expected excess\n  iterations. Candidates under --min-samples are labelled insufficient_data\n  and are never ranked or recommended; candidates below --quality-floor are\n  excluded before any iteration or gate-failure ordering, so a fast-but-wrong\n  candidate cannot outrank a slower correct one.\n\n  Without cohort flags this falls back to the LEGACY note-convention\n  scorecard below, which remains inspectable but is not evidence-backed:\n  it predates the routing evidence plane and carries no cohort or\n  independent-quality guarantee.\n\n  Legacy: read-only aggregation (plan 898/904, tech-spec 520 D8) over the\n  `dispatch_shape` / `model_choice` note convention in `session_entries`\n  (agents/orchestrator.md step 8a), joined with `agent_work_claims`\n  (terminal disposition) and `agent_actions` (test-coder expansion\n  outcome). Emits a per-(work-type, candidate) scorecard and a\n  recommended routing-map change. A pair with no completed-dispatch\n  history reports insufficient-data rather than a fabricated score.\n  Writes nothing: no routing-map mutation, no database write. Applying\n  a recommendation is a separate, explicit operator-gated action.","subcommands":[],"flags":[{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Cohort vendor; enables evidence-backed ranking","completion":{"kind":"none","values":[]},"env":null},{"long":"--role","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Cohort role (required with --vendor; ignored without it)","completion":{"kind":"none","values":[]},"env":null},{"long":"--tier","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Cohort tier (small|medium|large) (required with --vendor; ignored without it)","completion":{"kind":"none","values":[]},"env":null},{"long":"--work-type","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Cohort work type (required with --vendor; ignored without it)","completion":{"kind":"none","values":[]},"env":null},{"long":"--complexity","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Cohort complexity (bounded|standard|high-risk) (required with --vendor; ignored without it)","completion":{"kind":"none","values":[]},"env":null},{"long":"--project","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Cohort project id (required with --vendor; ignored without it)","completion":{"kind":"none","values":[]},"env":null},{"long":"--validation-policy","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Cohort validation policy version (required with --vendor; ignored without it)","completion":{"kind":"none","values":[]},"env":null},{"long":"--routing-policy","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Cohort routing policy version (required with --vendor; ignored without it)","completion":{"kind":"none","values":[]},"env":null},{"long":"--min-samples","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Minimum samples before a candidate is ranked (default 5)","completion":{"kind":"none","values":[]},"env":null},{"long":"--quality-floor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Wilson lower-bound floor (default 0.5)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"resolve","aliases":[],"hidden":false,"deprecated":null,"path":["models","resolve"],"command":"planar models resolve","summary":"Resolve a role's routing tier from its authoritative packet, or report the fallback and why.","description":"Read-only. Answers \"what tier should this role run at, and is that\n  answer backed by anything?\". Task-bound roles (coder, test-coder,\n  reviewer, research, janitor) resolve from the task's compiled profile;\n  pre-task roles (planner, spec-reviewer, ingestor, orchestrator) resolve\n  from a planning packet, which establishes readiness but classifies no\n  work because no unit of work exists yet.\n\n  When the authoritative packet is absent or unready the result reports\n  the configured static fallback AND the reason, and never a derived work\n  type — a tier shown without provenance reads identically to one derived\n  from real evidence.","subcommands":[],"flags":[{"long":"--role","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"planner|spec-reviewer|ingestor|orchestrator|coder|test-coder|reviewer|research|janitor","completion":{"kind":"none","values":[]},"env":null},{"long":"--task","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Task id (required for task-bound roles)","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Anchor plan id (pre-task roles)","completion":{"kind":"none","values":[]},"env":null},{"long":"--fallback-tier","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Configured static fallback tier (default medium)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"experiments","aliases":[],"hidden":false,"deprecated":null,"path":["models","experiments"],"command":"planar models experiments","summary":"List declared routing experiments and how much evidence each has produced.","description":"Read-only. Shows each experiment's frozen manifest identity (the\n  cohort it governs, its manifest digest, and when an operator approved\n  it) alongside how many terminal samples it has produced and how many\n  of those count toward a recommendation. The two counts differ whenever\n  a run was recorded but excluded; reporting only the eligible count\n  would understate what actually ran.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"outcomes","aliases":[],"hidden":false,"deprecated":null,"path":["models","outcomes"],"command":"planar models outcomes","summary":"List recorded terminal outcomes, including excluded ones and why they were excluded.","description":"Read-only. Excluded samples are shown deliberately: they are the\n  audit trail of the evidence boundary. Hiding them would make the\n  evidence look thinner than it is and leave no way to check the\n  boundary was applied correctly, and showing them without a named\n  reason would look like a bug.","subcommands":[],"flags":[{"long":"--limit","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Maximum rows to show (default 50)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"registry","aliases":[],"hidden":false,"deprecated":null,"path":["models","registry"],"command":"planar models registry","summary":"Manage opaque operator candidates and host observations.","description":"Manage opaque operator candidates and host observations.","subcommands":["list","add","update","remove","bind","unbind","observe","eligibility","verify-identity","export"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"list","aliases":[],"hidden":false,"deprecated":null,"path":["models","registry","list"],"command":"planar models registry list","summary":"List registrations, bindings, and latest observations.","description":"List registrations, bindings, and latest observations.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"add","aliases":[],"hidden":false,"deprecated":null,"path":["models","registry","add"],"command":"planar models registry add","summary":"Register one exact opaque candidate identifier.","description":"Register one exact opaque candidate identifier.","subcommands":[],"flags":[{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Candidate vendor","completion":{"kind":"none","values":[]},"env":null},{"long":"--id","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Exact opaque candidate identifier","completion":{"kind":"none","values":[]},"env":null},{"long":"--order","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"N","default":null,"description":"Deterministic fallback order","completion":{"kind":"none","values":[]},"env":null},{"long":"--disabled","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Register the candidate disabled (default: enabled)","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"update","aliases":[],"hidden":false,"deprecated":null,"path":["models","registry","update"],"command":"planar models registry update","summary":"Update enabled state and deterministic fallback order.","description":"Update enabled state and deterministic fallback order.","subcommands":[],"flags":[{"long":"--candidate","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"N","default":null,"description":"Registry candidate id","completion":{"kind":"none","values":[]},"env":null},{"long":"--order","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"N","default":null,"description":"Deterministic fallback order","completion":{"kind":"none","values":[]},"env":null},{"long":"--disabled","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Disable the candidate; omitting it enables it","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"remove","aliases":[],"hidden":false,"deprecated":null,"path":["models","registry","remove"],"command":"planar models registry remove","summary":"Remove a candidate when no immutable evidence references it.","description":"Remove a candidate when no immutable evidence references it.","subcommands":[],"flags":[{"long":"--candidate","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"N","default":null,"description":"Registry candidate id","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"bind","aliases":[],"hidden":false,"deprecated":null,"path":["models","registry","bind"],"command":"planar models registry bind","summary":"Allow one role and tier for a candidate.","description":"Allow one role and tier for a candidate.","subcommands":[],"flags":[{"long":"--candidate","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"N","default":null,"description":"Registry candidate id","completion":{"kind":"none","values":[]},"env":null},{"long":"--role","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Role to allow for the candidate","completion":{"kind":"none","values":[]},"env":null},{"long":"--tier","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Tier: small, medium, large","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"unbind","aliases":[],"hidden":false,"deprecated":null,"path":["models","registry","unbind"],"command":"planar models registry unbind","summary":"Remove one explicit role and tier binding.","description":"Remove one explicit role and tier binding.","subcommands":[],"flags":[{"long":"--candidate","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"N","default":null,"description":"Registry candidate id","completion":{"kind":"none","values":[]},"env":null},{"long":"--role","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Role binding to remove","completion":{"kind":"none","values":[]},"env":null},{"long":"--tier","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Tier: small, medium, large","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"observe","aliases":[],"hidden":false,"deprecated":null,"path":["models","registry","observe"],"command":"planar models registry observe","summary":"Append an exact, versioned host capability observation.","description":"Append an exact, versioned host capability observation.","subcommands":[],"flags":[{"long":"--candidate","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"N","default":null,"description":"Registry candidate id","completion":{"kind":"none","values":[]},"env":null},{"long":"--host","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Host id","completion":{"kind":"none","values":[]},"env":null},{"long":"--version","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"N","default":null,"description":"Observation version","completion":{"kind":"none","values":[]},"env":null},{"long":"--availability","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Availability: available, unavailable, unknown","completion":{"kind":"none","values":[]},"env":null},{"long":"--spawn-verification","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Spawn verification: verified, unverified, failed, mismatch","completion":{"kind":"none","values":[]},"env":null},{"long":"--evidence-ref","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Reference to the evidence for this observation","completion":{"kind":"none","values":[]},"env":null},{"long":"--captured-at","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"When the observation was captured (timestamp)","completion":{"kind":"none","values":[]},"env":null},{"long":"--expires-at","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"When the observation expires (timestamp)","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"eligibility","aliases":[],"hidden":false,"deprecated":null,"path":["models","registry","eligibility"],"command":"planar models registry eligibility","summary":"Report every independent eligibility gate and named exclusion reason.","description":"Report every independent eligibility gate and named exclusion reason.","subcommands":[],"flags":[{"long":"--candidate","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"N","default":null,"description":"Registry candidate id","completion":{"kind":"none","values":[]},"env":null},{"long":"--host","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Host id","completion":{"kind":"none","values":[]},"env":null},{"long":"--role","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Role name","completion":{"kind":"none","values":[]},"env":null},{"long":"--tier","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Tier: small, medium, large","completion":{"kind":"none","values":[]},"env":null},{"long":"--now","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Evaluation time (timestamp) for observation expiry","completion":{"kind":"none","values":[]},"env":null},{"long":"--override-supported","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Host supports a role-surface override","completion":{"kind":"none","values":[]},"env":null},{"long":"--policy-permits","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Host policy permits the candidate","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"verify-identity","aliases":[],"hidden":false,"deprecated":null,"path":["models","registry","verify-identity"],"command":"planar models registry verify-identity","summary":"Compare requested and actual spawn identity without aliasing.","description":"Compare requested and actual spawn identity without aliasing.","subcommands":[],"flags":[{"long":"--candidate","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"N","default":null,"description":"Registry candidate id","completion":{"kind":"none","values":[]},"env":null},{"long":"--actual-vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Vendor the spawn actually reported","completion":{"kind":"none","values":[]},"env":null},{"long":"--actual-id","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Candidate identifier the spawn actually reported","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"export","aliases":[],"hidden":false,"deprecated":null,"path":["models","registry","export"],"command":"planar models registry export","summary":"Export the versioned registry compatibility document.","description":"Export the versioned registry compatibility document.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"dashboard","aliases":[],"hidden":false,"deprecated":null,"path":["dashboard"],"command":"planar dashboard","summary":"Operator situational-awareness view of in-flight plans (and, with --agents, live claims).","description":"Roll-up of in-flight plans in the current scope.\n\n  --agents folds in the live claim state from agent_work_claims —\n  active claims, stale claims, and the per-plan 'next available'\n  task list. Without --agents the dashboard is a plain plan summary.\n\n  This is the operator's read surface for agent activity; the\n  `planar agent` subcommand namespace does not exist by design.\n  See `planar-agent` for the ritual (claim/heartbeat/complete) and\n  `planar-watch` for the live streaming view.","subcommands":[],"flags":[{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Limit to a single scope slug","completion":{"kind":"none","values":[]},"env":null},{"long":"--agents","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Fold in live claim state + next-available-work per plan","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"spec","aliases":[],"hidden":false,"deprecated":null,"path":["spec"],"command":"planar spec","summary":"Spec pipeline commands (draft, ingest).","description":"Commands for the planning pipeline spec surface.\n\n  'spec ingest' decomposes workbench planning documents into a\n  structured task graph in the database.\n  'spec draft' generates initial spec artifacts from a goal statement.","subcommands":["ingest"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"ingest","aliases":[],"hidden":false,"deprecated":null,"path":["spec","ingest"],"command":"planar spec ingest","summary":"Decompose workbench spec documents into the task graph.","description":"Decompose workbench spec documents into the task graph.","subcommands":[],"flags":[{"long":"--apply","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Commit the proposed changes; without it, preview only","completion":{"kind":"none","values":[]},"env":null},{"long":"--apply-removals","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Also commit proposed removals (cancel orphan tasks, abandon orphan plans); requires --apply","completion":{"kind":"none","values":[]},"env":null},{"long":"--format","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":"text","description":"Output format: text, json (default: text)","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug to resolve against instead of the cwd-derived scope","completion":{"kind":"none","values":[]},"env":null},{"long":"--strict","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Reject the ingest when scenario coverage is incomplete or a task slug collides","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan","kind":"string","required":true,"default":null,"description":"Plan id or slug to ingest the workbench specs into","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"test-spec","aliases":[],"hidden":false,"deprecated":null,"path":["test-spec"],"command":"planar test-spec","summary":"Test-spec coverage inspectors.","description":"Commands for inspecting test-spec coverage of a plan's tasks.\n\n  'test-spec status' prints a per-milestone breakdown of which tasks\n  have verifying scenarios. This is a read-only complement to the\n  ingest-time coverage gate (see `planar spec ingest --strict`).","subcommands":["status"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"status","aliases":[],"hidden":false,"deprecated":null,"path":["test-spec","status"],"command":"planar test-spec status","summary":"Print per-milestone test-spec coverage for an anchor plan.","description":"Print per-milestone test-spec coverage for an anchor plan.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan","kind":"string","required":true,"default":null,"description":"Plan slug or numeric id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"config","aliases":[],"hidden":false,"deprecated":null,"path":["config"],"command":"planar config","summary":"Manage Planar configuration.","description":"Read, inspect, and validate the Planar configuration file.\n\n  The configuration file lives at ~/.planar/config.toml by default.\n  Set $PLANAR_CONFIG_PATH to use a different path.\n  Resolution order (highest to lowest priority):\n    1. Explicit --config-path flag\n    2. $PLANAR_CONFIG_PATH\n    3. ~/.planar/config.toml","subcommands":["show","edit","validate","init","path"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"show","aliases":[],"hidden":false,"deprecated":null,"path":["config","show"],"command":"planar config show","summary":"Print the resolved configuration.","description":"Print the resolved configuration.","subcommands":[],"flags":[{"long":"--effective","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Show each key with its provenance","completion":{"kind":"none","values":[]},"env":null},{"long":"--raw","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Show the raw on-disk config values","completion":{"kind":"none","values":[]},"env":null},{"long":"--defaults","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Show the embedded defaults","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug whose per-association overrides apply","completion":{"kind":"none","values":[]},"env":null},{"long":"--format","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":"text","description":"Accepted but not read; use --json for JSON output","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"edit","aliases":[],"hidden":false,"deprecated":null,"path":["config","edit"],"command":"planar config edit","summary":"Edit the configuration file in $EDITOR.","description":"Edit the configuration file in $EDITOR.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"validate","aliases":[],"hidden":false,"deprecated":null,"path":["config","validate"],"command":"planar config validate","summary":"Validate configuration file syntax.","description":"Validate configuration file syntax.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"init","aliases":[],"hidden":false,"deprecated":null,"path":["config","init"],"command":"planar config init","summary":"Initialize the configuration file.","description":"Initialize the configuration file.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"path","aliases":[],"hidden":false,"deprecated":null,"path":["config","path"],"command":"planar config path","summary":"Show the configuration file path.","description":"Show the configuration file path.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"templates","aliases":[],"hidden":false,"deprecated":null,"path":["templates"],"command":"planar templates","summary":"Inspect, validate, and render Planar JSON templates.","description":"Manage the template plane: list available templates, show their\n  raw JSON, render them against a DB entity (dry run), validate\n  syntax, initialise the default set on disk, and print resolution\n  paths.\n\n  Templates resolve via a three-level fallback chain:\n    1. ~/.planar/templates/<kind>/<slug>.json (operator overrides)\n    2. ~/.planar/templates/defaults/<kind>/<slug>.json (default copies)\n    3. templates/defaults/<kind>/<slug>.json (embedded in the binary)","subcommands":["list","show","render","validate","init","path"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"list","aliases":[],"hidden":false,"deprecated":null,"path":["templates","list"],"command":"planar templates list","summary":"List available templates.","description":"List available templates.","subcommands":[],"flags":[{"long":"--system","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Only list templates for this external system","completion":{"kind":"none","values":[]},"env":null},{"long":"--set","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Only list templates in this set","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"show","aliases":[],"hidden":false,"deprecated":null,"path":["templates","show"],"command":"planar templates show","summary":"Show a template's raw JSON.","description":"Show a template's raw JSON.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"set","kind":"string","required":true,"default":null,"description":"Template set name","completion":{"kind":"none","values":[]}},{"name":"system","kind":"string","required":true,"default":null,"description":"External system name, e.g. jira, github-issues","completion":{"kind":"none","values":[]}},{"name":"kind","kind":"string","required":true,"default":null,"description":"Template kind, e.g. epic","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"render","aliases":[],"hidden":false,"deprecated":null,"path":["templates","render"],"command":"planar templates render","summary":"Render a template against a database entity (dry run; no writes).","description":"Render a template against a database entity (dry run; no writes).","subcommands":[],"flags":[{"long":"--entity","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Entity ref (kind:id) — task:42, plan:7, scenario:3","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"set","kind":"string","required":true,"default":null,"description":"Template set name","completion":{"kind":"none","values":[]}},{"name":"system","kind":"string","required":true,"default":null,"description":"External system name, e.g. jira, github-issues","completion":{"kind":"none","values":[]}},{"name":"kind","kind":"string","required":true,"default":null,"description":"Template kind, e.g. epic","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"validate","aliases":[],"hidden":false,"deprecated":null,"path":["templates","validate"],"command":"planar templates validate","summary":"Validate template syntax.","description":"Validate template syntax.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"set","kind":"string","required":true,"default":null,"description":"Template set name","completion":{"kind":"none","values":[]}},{"name":"system","kind":"string","required":true,"default":null,"description":"External system name, e.g. jira, github-issues","completion":{"kind":"none","values":[]}},{"name":"kind","kind":"string","required":true,"default":null,"description":"Template kind, e.g. epic","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"init","aliases":[],"hidden":false,"deprecated":null,"path":["templates","init"],"command":"planar templates init","summary":"Extract default templates to disk.","description":"Extract default templates to disk.","subcommands":[],"flags":[{"long":"--force","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Overwrite template files that already exist on disk","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"path","aliases":[],"hidden":false,"deprecated":null,"path":["templates","path"],"command":"planar templates path","summary":"Show template resolution paths.","description":"Show template resolution paths.","subcommands":[],"flags":[{"long":"--system","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Accepted but ignored; the templates root is always printed","completion":{"kind":"none","values":[]},"env":null},{"long":"--set","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Accepted but ignored; the templates root is always printed","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"tree","aliases":[],"hidden":false,"deprecated":null,"path":["tree"],"command":"planar tree","summary":"Render plans, tasks, artifacts, decisions, scenarios, and questions as a hierarchical tree.","description":"Render a hierarchical view of Planar entities for one or all\n  scopes.\n\n  Walks plans (via parent_plan_id), tasks (via plan_id and\n  parent_task_id), and entity_links(derives-from) to gather\n  artifacts, decisions, scenarios, and questions linked to each plan.","subcommands":[],"flags":[{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Limit to a single scope slug","completion":{"kind":"none","values":[]},"env":null},{"long":"--all-scopes","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Include every scope","completion":{"kind":"none","values":[]},"env":null},{"long":"--depth","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":-1,"description":"Max tree depth (-1 = unbounded)","completion":{"kind":"none","values":[]},"env":null},{"long":"--kind","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to a single kind","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to a single status","completion":{"kind":"none","values":[]},"env":null},{"long":"--sort","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Sort key: id, updated, created, unsorted","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"search","aliases":[],"hidden":false,"deprecated":null,"path":["search"],"command":"planar search","summary":"Full-text search across plans, tasks, questions, scenarios, decisions, and artifacts.","description":"Run a full-text search across every searchable entity kind.\n\n  Queries are passed to SQLite's FTS5 MATCH operator directly.\n  Multi-word queries are AND'd unless the operator is given\n  explicitly (OR, NOT, NEAR, \"phrase\"). Tokens are unicode61-folded\n  (case-insensitive, diacritic-stripped).","subcommands":[],"flags":[{"long":"--kind","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to a single kind","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to a single status","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to a scope slug","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Restrict to a plan id","completion":{"kind":"none","values":[]},"env":null},{"long":"--limit","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":50,"description":"Max results","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"query","kind":"string","required":true,"default":null,"description":"FTS5 query string","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"local","aliases":[],"hidden":false,"deprecated":null,"path":["local"],"command":"planar local","summary":"Manage user-local sandbox skills and agents under ~/.planar/local/.","description":"Manage the operator's local sandbox for personal skills and agents.\n\n  Authors a single source file per skill or agent under\n  ~/.planar/local/ and creates per-vendor symlinks (with copy\n  fallback) into each vendor's install directory.\n  Edits to the source file propagate immediately to every vendor.","subcommands":["list","link","unlink","import","migrate"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"list","aliases":[],"hidden":false,"deprecated":null,"path":["local","list"],"command":"planar local list","summary":"List locally-installed skills and agents.","description":"List locally-installed skills and agents.","subcommands":[],"flags":[{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Only list installs for this vendor: claude, codex, copilot","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"link","aliases":[],"hidden":false,"deprecated":null,"path":["local","link"],"command":"planar local link","summary":"Create or reuse symlinks from vendor paths to local source.","description":"Create or reuse symlinks from vendor paths to local source.","subcommands":[],"flags":[{"long":"--dry-run","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Preview the planned installs without touching the filesystem","completion":{"kind":"none","values":[]},"env":null},{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to one vendor: claude, codex, copilot","completion":{"kind":"none","values":[]},"env":null},{"long":"--reconcile","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Run the reconcile pass instead of linking; takes no name","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"name","kind":"string","required":false,"default":null,"description":"Link only the source with this name (default: all)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"unlink","aliases":[],"hidden":false,"deprecated":null,"path":["local","unlink"],"command":"planar local unlink","summary":"Remove symlinks from vendor paths.","description":"Remove symlinks from vendor paths.","subcommands":[],"flags":[{"long":"--purge","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Also delete the source file from the local sandbox","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"name","kind":"string","required":true,"default":null,"description":"Name of the skill or agent to unlink","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"import","aliases":[],"hidden":false,"deprecated":null,"path":["local","import"],"command":"planar local import","summary":"Import a skill or agent from an external directory.","description":"Import a skill or agent from an external directory.","subcommands":[],"flags":[{"long":"--kind","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Target kind: skill, agent (default: skill)","completion":{"kind":"none","values":[]},"env":null},{"long":"--force","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Overwrite a sandbox file of the same name; otherwise collisions are skipped","completion":{"kind":"none","values":[]},"env":null},{"long":"--dry-run","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Preview the planned imports and links without writing","completion":{"kind":"none","values":[]},"env":null},{"long":"--no-link","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Import only; skip the link step","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"path","kind":"string","required":true,"default":null,"description":"Source .md file, skill directory, or directory of sources","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"migrate","aliases":[],"hidden":false,"deprecated":null,"path":["local","migrate"],"command":"planar local migrate","summary":"Migrate skills/agents to new Planar version.","description":"Migrate skills/agents to new Planar version.","subcommands":[],"flags":[{"long":"--dry-run","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Preview the renames without changing disk","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"skills","aliases":[],"hidden":false,"deprecated":null,"path":["skills"],"command":"planar skills","summary":"Retired: rendering and drift detection now live in scriptorium.","description":"The unified skill source tree under skills/src/ is rendered by the\n  external scriptorium binary (plan 918). Planar no longer renders vendor\n  projections nor tracks their install-drift in-band; use `scriptorium\n  check`/`scriptorium status` instead. This command has no subcommands.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"import","aliases":[],"hidden":false,"deprecated":null,"path":["import"],"command":"planar import","summary":"Import an existing repo's state into Planar.","description":"import translates the planning artefacts of an existing\n  repository into Planar's data model. It discovers\n  tech specs, roadmap milestones, ADRs, and backlog files,\n  infers completion status from checkbox state and git history,\n  and produces an ImportPlan for review before committing.","subcommands":[],"flags":[{"long":"--from-github","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Accepted but not read by this build","completion":{"kind":"none","values":[]},"env":null},{"long":"--dry-run","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Accepted but not read; use the default preview (no --apply)","completion":{"kind":"none","values":[]},"env":null},{"long":"--strict","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Accepted but not read by this build","completion":{"kind":"none","values":[]},"env":null},{"long":"--roadmap","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Accepted but not read by this build","completion":{"kind":"none","values":[]},"env":null},{"long":"--apply","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Commit the import plan; without it, preview only","completion":{"kind":"none","values":[]},"env":null},{"long":"--apply-removals","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Also commit proposed removals; requires --apply","completion":{"kind":"none","values":[]},"env":null},{"long":"--no-status-inference","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Accepted but not read by this build","completion":{"kind":"none","values":[]},"env":null},{"long":"--interpret","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Run the optional LLM interpretation pass after the deterministic classifier","completion":{"kind":"none","values":[]},"env":null},{"long":"--accept-spec","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Non-interactive forward-spec selection — slug, comma-separated slugs, or 'all'","completion":{"kind":"none","values":[]},"env":null},{"long":"--no-forward-specs","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Skip forward-spec processing entirely","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug to resolve against instead of the cwd-derived scope","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"repo-root","kind":"string","required":true,"default":null,"description":"Repository root to import from","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"synthesize","aliases":[],"hidden":false,"deprecated":null,"path":["synthesize"],"command":"planar synthesize","summary":"Synthesize fresh planning artifacts from a repo's docs + code + git history.","description":"synthesize reads a repository's existing planning docs, source\n  code, and git history AS INPUT for an LLM synthesis pass. It\n  produces fresh product-spec / tech-spec / roadmap artifacts (NOT a\n  verbatim transcription) and proposes them via the same workbench\n  pipeline as the planner agent.","subcommands":[],"flags":[{"long":"--apply","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Commit the synthesized artifacts; without it, preview only","completion":{"kind":"none","values":[]},"env":null},{"long":"--apply-removals","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Also commit proposed removals; requires --apply","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug to resolve against instead of the cwd-derived scope","completion":{"kind":"none","values":[]},"env":null},{"long":"--code-layout","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Override layout detection: swift, go, node, python, mixed","completion":{"kind":"none","values":[]},"env":null},{"long":"--treat-as-greenfield","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Force greenfield mode even when code is detected","completion":{"kind":"none","values":[]},"env":null},{"long":"--treat-as-nongreenfield","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Force non-greenfield mode even when no code is detected","completion":{"kind":"none","values":[]},"env":null},{"long":"--literal","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Delegate to import (verbatim transcription) instead of synthesizing","completion":{"kind":"none","values":[]},"env":null},{"long":"--accept-spec","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Non-interactive forward-spec selection — slug, comma-separated slugs, or 'all'","completion":{"kind":"none","values":[]},"env":null},{"long":"--no-forward-specs","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Skip forward-spec processing entirely","completion":{"kind":"none","values":[]},"env":null},{"long":"--dry-run","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Preview only; do not write","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"repo-root","kind":"string","required":true,"default":null,"description":"Repository root to synthesize from","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"version","aliases":[],"hidden":false,"deprecated":null,"path":["version"],"command":"planar version","summary":"Print the planar version, commit, and C++ toolchain.","description":"Print the planar version, commit, and C++ toolchain.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"completion","aliases":[],"hidden":false,"deprecated":null,"path":["completion"],"command":"planar completion","summary":"Generate the autocompletion script for the specified shell.","description":"Generate the autocompletion script for the specified shell.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[{"name":"shell","kind":"string","required":true,"default":null,"description":"Shell: bash, zsh, or fish","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"schema","aliases":[],"hidden":false,"deprecated":null,"path":["schema"],"command":"planar schema","summary":"Print the full command tree as a JSON catalog (flags, aliases, positionals).","description":"Print the full command tree as a JSON catalog (flags, aliases, positionals).","subcommands":[],"flags":[{"long":"--command","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Emit only this command's catalog object, by full path (\"planar task update\") or relative to the root (\"task update\"); an unknown path exits 2 with nothing on stdout","completion":{"kind":"none","values":[]},"env":null},{"long":"--compact","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit one {command, summary} row per command instead of the full catalog; with --command, only that command's row","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"report","aliases":[],"hidden":false,"deprecated":null,"path":["report"],"command":"planar report","summary":"Emit the diagnostic bundle: invocation and closed claim-failure aggregates plus health metrics.","description":"Reads the cli_invocations capture log and the always-on observability\ntables (agent_actions, sync_events, agent_work_claims, handoffs) and\nrenders a diagnostic bundle.\n\nInvocation and failure sections render \"logging disabled\" when\n[introspection].cli_log is off; the always-on sections (actions, sync,\nclaims, claim failure categories, handoffs, health) render normally in\neither case.\n\nThe bundle is structurally redacted: queries select only counts,\ncategories, verb paths, statuses, and timestamps — never entity text.\n\nExit codes:\n  0   bundle rendered successfully.\n  2   invalid flag value (--days or --tail must be a positive integer).\n  1   database error.","subcommands":[],"flags":[{"long":"--days","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":30,"description":"Window in days (must be > 0, default 30).","completion":{"kind":"none","values":[]},"env":null},{"long":"--tail","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":20,"description":"Number of failure-tail rows (must be > 0, default 20).","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit stable machine-readable JSON.","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"bench","aliases":[],"hidden":false,"deprecated":null,"path":["bench"],"command":"planar bench","summary":"Record and query benchmark run data (measurement rig).","description":"Record measurement-rig data for the vertical-slice decomposition experiment.\n\n  Arms: strict, eligibility, grouped (or any free-text pilot value).\n  Statuses: running, completed, aborted, error.\n  Touch kinds: declared, actual.\n\n  Workflow: bench start → bench event (repeat) → bench touch (repeat)\n            → bench harvest → bench finish → bench show --json.","subcommands":["start","event","touch","harvest","finish","show"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"start","aliases":[],"hidden":false,"deprecated":null,"path":["bench","start"],"command":"planar bench start","summary":"Mint a new run record and print its run_uid.","description":"Mint a new run record and print its run_uid.\n\n  --task <id> (repeatable): limit the declared-touch snapshot to\n  the given task ids. When omitted, all plan tasks are snapshotted\n  (backward-compatible default). Use when the arm only dispatches\n  a known subset of tasks and meta-tasks with no touches would\n  otherwise inflate the declared set.","subcommands":[],"flags":[{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"N","default":null,"description":"Plan id whose tasks are snapshotted as declared touches","completion":{"kind":"none","values":[]},"env":null},{"long":"--arm","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Experiment arm: strict, eligibility, grouped, or free text","completion":{"kind":"none","values":[]},"env":null},{"long":"--base-sha","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Base commit sha the run starts from","completion":{"kind":"none","values":[]},"env":null},{"long":"--config-hash","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Hash identifying the run configuration","completion":{"kind":"none","values":[]},"env":null},{"long":"--config-json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Run configuration; must be valid JSON","completion":{"kind":"none","values":[]},"env":null},{"long":"--corpus-repo","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Corpus repository the run operates on","completion":{"kind":"none","values":[]},"env":null},{"long":"--task","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":true,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Limit declared-touch snapshot to this task id (repeatable).","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"run-uid","kind":"string","required":true,"default":null,"description":"Run uid to mint","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"event","aliases":[],"hidden":false,"deprecated":null,"path":["bench","event"],"command":"planar bench event","summary":"Append a journal event to a run.","description":"Append a journal event to a run.","subcommands":[],"flags":[{"long":"--kind","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Journal event kind","completion":{"kind":"none","values":[]},"env":null},{"long":"--seq","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"N","default":null,"description":"Event sequence number within the run","completion":{"kind":"none","values":[]},"env":null},{"long":"--payload","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Event payload; must be valid JSON","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"run-uid","kind":"string","required":true,"default":null,"description":"Run uid","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"touch","aliases":[],"hidden":false,"deprecated":null,"path":["bench","touch"],"command":"planar bench touch","summary":"Record a declared or actual file touch for a run.","description":"Record a declared or actual file touch for a run.","subcommands":[],"flags":[{"long":"--task","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"N","default":null,"description":"Task id the touch belongs to","completion":{"kind":"none","values":[]},"env":null},{"long":"--path","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"File path that was touched","completion":{"kind":"none","values":[]},"env":null},{"long":"--kind","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Touch kind: declared, actual","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"run-uid","kind":"string","required":true,"default":null,"description":"Run uid","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"harvest","aliases":[],"hidden":false,"deprecated":null,"path":["bench","harvest"],"command":"planar bench harvest","summary":"Harvest git diff as actual touches for a run/task.","description":"Harvest git diff as actual touches for a run/task.","subcommands":[],"flags":[{"long":"--task","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"N","default":null,"description":"Task id the harvested touches are attributed to","completion":{"kind":"none","values":[]},"env":null},{"long":"--worktree","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Worktree directory to run git diff in","completion":{"kind":"none","values":[]},"env":null},{"long":"--base","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Base ref of a committed range; requires --head (default: working tree)","completion":{"kind":"none","values":[]},"env":null},{"long":"--head","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Head ref of a committed range; requires --base","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"run-uid","kind":"string","required":true,"default":null,"description":"Run uid","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"finish","aliases":[],"hidden":false,"deprecated":null,"path":["bench","finish"],"command":"planar bench finish","summary":"Set the terminal status on a run.","description":"Set the terminal status on a run.","subcommands":[],"flags":[{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Terminal status: completed, aborted, error","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"run-uid","kind":"string","required":true,"default":null,"description":"Run uid","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"show","aliases":[],"hidden":false,"deprecated":null,"path":["bench","show"],"command":"planar bench show","summary":"Show a run's full state (header + events + touches).","description":"Show a run's full state (header + events + touches).","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"run-uid","kind":"string","required":true,"default":null,"description":"Run uid","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"closure","aliases":[],"hidden":false,"deprecated":null,"path":["closure"],"command":"planar closure","summary":"Compute and inspect a task's derived symbol-level closure.","description":"Compute the *derived* closure of a task — the symbols it must hold\nresident, computed by static analysis from the task's declared seed\npaths (task_touch_paths), partitioned by role:\n\n  modify     — the seed's own edited symbols.\n  reference  — the interfaces the seed depends on.\n  transitive — deeper hops (stored, but excluded from the effective\n               closure by default).\n\n  Workflow: closure compute <task-id> → closure show <task-id> --json.","subcommands":["compute","show"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"compute","aliases":[],"hidden":false,"deprecated":null,"path":["closure","compute"],"command":"planar closure compute","summary":"Run the extractor over a task's seeds and persist the closure.","description":"Run the extractor over a task's seeds and persist the closure.","subcommands":[],"flags":[{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug to resolve against instead of the cwd-derived scope","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"task-id","kind":"string","required":true,"default":null,"description":"Task id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"show","aliases":[],"hidden":false,"deprecated":null,"path":["closure","show"],"command":"planar closure show","summary":"Read back a task's persisted closure rows.","description":"Read back a task's persisted closure rows.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"task-id","kind":"string","required":true,"default":null,"description":"Task id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"run","aliases":[],"hidden":false,"deprecated":null,"path":["run"],"command":"planar run","summary":"Record and query operational run traces.","description":"Record operational run traces emitted by workflows.\n\n  Arm defaults to 'op' (or the workflow name when --workflow is given).\n  Statuses: running, completed, aborted, error.\n\n  Workflow: run start → run event (repeat) → run finish → run show --json.\n\n  See `planar bench` for the measurement-rig surface (strict/eligibility/\n  grouped arms, declared/actual touch tracking, git-diff harvest).","subcommands":["start","event","finish","show"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"start","aliases":[],"hidden":false,"deprecated":null,"path":["run","start"],"command":"planar run start","summary":"Mint a new operational run record and print its run_uid as JSON.","description":"Mint a new operational run record and print its run_uid as JSON.","subcommands":[],"flags":[{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"N","default":null,"description":"Plan id to associate the run with","completion":{"kind":"none","values":[]},"env":null},{"long":"--workflow","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Workflow name recorded on the run","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"event","aliases":[],"hidden":false,"deprecated":null,"path":["run","event"],"command":"planar run event","summary":"Append a journal event to a run (seq auto-incremented).","description":"Append a journal event to a run (seq auto-incremented).","subcommands":[],"flags":[{"long":"--kind","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Event kind, a free-form label (e.g. step, note, error)","completion":{"kind":"none","values":[]},"env":null},{"long":"--payload","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Event payload body","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"run-uid","kind":"string","required":true,"default":null,"description":"Run uid returned by run start","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"finish","aliases":[],"hidden":false,"deprecated":null,"path":["run","finish"],"command":"planar run finish","summary":"Set the terminal status on a run.","description":"Set the terminal status on a run.","subcommands":[],"flags":[{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Terminal status: completed, aborted, error","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"run-uid","kind":"string","required":true,"default":null,"description":"Run uid returned by run start","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"show","aliases":[],"hidden":false,"deprecated":null,"path":["run","show"],"command":"planar run show","summary":"Show a run's full state (header + events).","description":"Show a run's full state (header + events).","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"run-uid","kind":"string","required":true,"default":null,"description":"Run uid returned by run start","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"groups","aliases":[],"hidden":false,"deprecated":null,"path":["groups"],"command":"planar groups","summary":"Recommend task slices that minimize closure replication.","description":"Form **slices** — groups of a plan's open (todo) tasks that share a\ncontext window — by minimizing the duplicated closure across slices,\nsubject to a per-slice token budget. Read-only sibling to\n`plan recommend-strategy`: it reports a recommendation, writing nothing.\n\nEach slice reports its member task ids, its unioned effective closure\n(the distinct symbols the slice must hold resident, role modify ∪\nreference), and that union's token cost. No slice's cost exceeds the\nbudget, and the slice-DAG induced by the task `blocks` dependencies is\nalways schedulable (no slice is grouped across a dependency violation).\n\n  --solver greedy|mtkahypar  (default greedy) selects the partitioner.\n  `mtkahypar` is the optional external hypergraph solver: when its binary\n  is absent or fails, the verb degrades to greedy and reports\n  `optimal_available:false` (it never errors on a missing optional dep).\n\n  Workflow: closure compute <task> (per task) → groups recommend <plan>.","subcommands":["recommend"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"recommend","aliases":[],"hidden":false,"deprecated":null,"path":["groups","recommend"],"command":"planar groups recommend","summary":"Recommend closure-minimizing task slices for a plan.","description":"Recommend closure-minimizing task slices for a plan.","subcommands":[],"flags":[{"long":"--budget","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Per-slice window budget, an unsigned integer (default: 128000)","completion":{"kind":"none","values":[]},"env":null},{"long":"--solver","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Solver: greedy, mtkahypar (default: greedy)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan-id","kind":"string","required":true,"default":null,"description":"Plan id","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"explore","aliases":[],"hidden":false,"deprecated":null,"path":["explore"],"command":"planar explore","summary":"Launch the interactive cockpit (same as bare `planar` on a TTY).","description":"Launch the interactive Planar cockpit.\n\n  Equivalent to invoking `planar` with no verb on a terminal. Use\n  `planar explore` when you want to force-launch the cockpit by name,\n  or from a context where bare-invocation detection may not fire.\n\n  --plan, --task, and --scope seed the initial focus.\n\n  Falls back to this help text when stdout is not a TTY, when TERM=dumb,\n  when PLANAR_NO_TUI is set, or when --plain is passed.","subcommands":[],"flags":[{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Seed initial focus on this plan ID","completion":{"kind":"none","values":[]},"env":null},{"long":"--task","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Seed initial focus on this task ID","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Seed scope filter","completion":{"kind":"none","values":[]},"env":null},{"long":"--plain","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Fall back to help/usage instead of launching the cockpit","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"workflow","aliases":[],"hidden":false,"deprecated":null,"path":["workflow"],"command":"planar workflow","summary":"Discover, inspect, and run Lua workflows for planar-execute.","description":"Enumerate, inspect, and invoke shipped and sandbox Lua workflows.\n\n  Shipped workflows live at $PLANAR_HOME/workflows/ (default\n  ~/.planar/workflows/).  Sandbox workflows live at\n  ~/.planar/local/workflows/ and are marked `local`.\n\n  These commands are READ-ONLY w.r.t. SQLite.  `run` delegates\n  execution to `planar-execute` and forwards its output + exit code.","subcommands":["list","show","run"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"list","aliases":[],"hidden":false,"deprecated":null,"path":["workflow","list"],"command":"planar workflow list","summary":"List shipped and sandbox workflows.","description":"List shipped and sandbox workflows.","subcommands":[],"flags":[{"long":"--local","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Show only sandbox (local) workflows","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"show","aliases":[],"hidden":false,"deprecated":null,"path":["workflow","show"],"command":"planar workflow show","summary":"Show @meta and source path for a named workflow.","description":"Show @meta and source path for a named workflow.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"name","kind":"string","required":true,"default":null,"description":"Workflow name","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"run","aliases":[],"hidden":false,"deprecated":null,"path":["workflow","run"],"command":"planar workflow run","summary":"Resolve a workflow by name and exec it via planar-execute.","description":"Resolve <name> across shipped and sandbox workflows, then exec\n  `planar-execute run <path> --phase <phase> [--args <json>]\n  [--worktree <dir>] [--sandbox-root <dir>]`.  The workflow's\n  flow.result JSON streams to stdout; the exit code is forwarded\n  exactly (non-zero on flow.fail or engine error).\n\n  planar-execute resolution order: $PLANAR_EXECUTE_BIN →\n  sibling of argv[0] → PATH.","subcommands":[],"flags":[{"long":"--phase","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Phase function to invoke inside the workflow","completion":{"kind":"none","values":[]},"env":null},{"long":"--args","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":"","description":"JSON args blob forwarded to planar-execute --args","completion":{"kind":"none","values":[]},"env":null},{"long":"--worktree","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":"","description":"Worktree directory forwarded to planar-execute --worktree","completion":{"kind":"none","values":[]},"env":null},{"long":"--sandbox-root","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":"","description":"Sandbox root forwarded to planar-execute --sandbox-root","completion":{"kind":"none","values":[]},"env":null},{"long":"--local","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Restrict resolution to sandbox (local) workflows only","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"name","kind":"string","required":true,"default":null,"description":"Workflow name, resolved across shipped and sandbox workflows","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"feedback","aliases":[],"hidden":false,"deprecated":null,"path":["feedback"],"command":"planar feedback","summary":"Manage structured feedback.","description":"Manage structured feedback.","subcommands":["triage"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"triage","aliases":[],"hidden":false,"deprecated":null,"path":["feedback","triage"],"command":"planar feedback triage","summary":"Review structured feedback triage.","description":"Review structured feedback triage.","subcommands":["list","show","set"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"list","aliases":[],"hidden":false,"deprecated":null,"path":["feedback","triage","list"],"command":"planar feedback triage list","summary":"List triaged findings.","description":"List triaged findings.","subcommands":[],"flags":[{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Only findings on this feedback plan id","completion":{"kind":"none","values":[]},"env":null},{"long":"--severity","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Only findings of this severity: info, low, medium, high, critical","completion":{"kind":"none","values":[]},"env":null},{"long":"--disposition","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Only findings with this disposition (e.g. untriaged, accepted, duplicate)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"show","aliases":[],"hidden":false,"deprecated":null,"path":["feedback","triage","show"],"command":"planar feedback triage show","summary":"Show a triaged finding.","description":"Show a triaged finding.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"finding","kind":"string","required":true,"default":null,"description":"Finding ref (task:id or question:id)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"set","aliases":[],"hidden":false,"deprecated":null,"path":["feedback","triage","set"],"command":"planar feedback triage set","summary":"Set operator-confirmed triage fields.","description":"Set operator-confirmed triage fields.","subcommands":[],"flags":[{"long":"--severity","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Severity: info, low, medium, high, critical","completion":{"kind":"none","values":[]},"env":null},{"long":"--disposition","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Disposition: untriaged, needs-reproduction, accepted, retained-question, dismissed, reported-external, duplicate","completion":{"kind":"none","values":[]},"env":null},{"long":"--reproduction","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Reproduction status: not-run, reproduced, not-reproduced, inconclusive","completion":{"kind":"none","values":[]},"env":null},{"long":"--duplicate-of","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Finding ref this one duplicates (task:id or question:id); required for disposition duplicate","completion":{"kind":"none","values":[]},"env":null},{"long":"--evidence","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Redacted evidence summary text","completion":{"kind":"none","values":[]},"env":null},{"long":"--scope","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Scope slug to resolve against instead of the cwd-derived scope","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"finding","kind":"string","required":true,"default":null,"description":"Finding ref (task:id or question:id)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"help","aliases":[],"hidden":false,"deprecated":null,"path":["help"],"command":"planar help","summary":"Print the root help page (same as `planar --help`).","description":"Print the root help page and exit; the same page as `planar --help`.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}}]}
)CATALOG6542");
  // clang-format on

  // Non-vacuity for the 6130 half specifically: the four empty-string
  // defaults are PRESENT rather than absent.
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

  SECTION("parse failures write stderr ONLY and exit 2") {
    // Decision 1004 (task 6271): the dual-stream split this section used to
    // pin — a formatted message on STDOUT, a CamelCase tag on STDERR — is
    // now a recorded, deliberate divergence from the oracle: the C++ tree
    // moves the whole formatted message to stderr and drops the CamelCase
    // tag, so a parse failure presents identically to a handler refusal
    // (stdout empty, one "error: <message>" line on stderr). The oracle is
    // NOT touched and keeps the split; see decision 1004 for the rationale
    // and task 6316 for why these bytes were already off oracle-comparison.
    auto const bad_verb = run({"nosuchverb"}, "badverb");
    CHECK(bad_verb.code == 2);
    CHECK(bad_verb.out.empty());
    CHECK(bad_verb.err == "error: planar: The following argument was not expected: nosuchverb\n");

    auto const bad_sub = run({"workflow", "nosuchsub"}, "badsub");
    CHECK(bad_sub.code == 2);
    CHECK(bad_sub.out.empty());
    CHECK(bad_sub.err == "error: workflow: The following argument was not expected: nosuchsub\n");

    auto const bad_flag = run({"workflow", "list", "--nosuchflag"}, "badflag");
    CHECK(bad_flag.code == 2);
    CHECK(bad_flag.out.empty());
    CHECK(bad_flag.err == "error: list: The following argument was not expected: --nosuchflag\n");

    auto const missing_pos = run({"workflow", "show"}, "missingpos");
    CHECK(missing_pos.code == 2);
    CHECK(missing_pos.out.empty());
    CHECK(missing_pos.err == "error: name is required\n");

    auto const unlink_no_pos = run({"unlink"}, "ulnopos");
    CHECK(unlink_no_pos.code == 2);
    CHECK(unlink_no_pos.out.empty());
    CHECK(unlink_no_pos.err == "error: link-id is required\n");

    auto const skills_extra = run({"skills", "extra"}, "skextra");
    CHECK(skills_extra.code == 2);
    CHECK(skills_extra.out.empty());
    CHECK(skills_extra.err == "error: skills: The following argument was not expected: extra\n");
  }

  SECTION("leaf help pages render every declared flag and positional") {
    auto const version_help = run({"version", "--help"}, "vhelp");
    CHECK(version_help.code == 0);
    CHECK(version_help.err.empty());
    CHECK(version_help.out == "Print the planar version, commit, and C++ toolchain.\n"
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
                           "          --local             Show only sandbox (local) workflows\n"
                           "          --json              Emit machine-readable JSON instead of text\n");

    auto const show_help = run({"workflow", "show", "--help"}, "wshelp");
    CHECK(show_help.code == 0);
    CHECK(show_help.out == "Show @meta and source path for a named workflow.\n"
                           "\n"
                           "\n"
                           "show [OPTIONS] name\n"
                           "\n"
                           "\n"
                           "POSITIONALS:\n"
                           "  name REQUIRED               Workflow name\n"
                           "\n"
                           "OPTIONS:\n"
                           "  -h,     --help              Print this help message and exit\n"
                           "          --json              Emit machine-readable JSON instead of text\n");

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
                                   "          --anchor-path       File path the annotation is anchored to (required)\n"
                                   "          --line-start :INT   First line of the anchored range\n"
                                   "          --line-end :INT     Last line of the anchored range\n"
                                   "          --commit-sha        Commit the anchored content was captured at, for later\n"
                                   "                              verification\n"
                                   "          --text-hash         Hash of the anchored text, for later drift detection\n"
                                   "          --text              Annotation text\n"
                                   "          --title             Annotation title\n"
                                   "          --slug              Annotation slug\n"
                                   "          --body              Annotation body (literal text)\n"
                                   "          --vendor            Vendor that authored the annotation\n"
                                   "          --plan :INT         Plan id the annotation is associated with\n"
                                   "          --task :INT         Task id the annotation is associated with\n"
                                   "          --tags              Comma-separated tags\n"
                                   "          --scope             Scope slug the annotation is created under\n"
                                   "          --json              Emit machine-readable JSON instead of text\n");

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
                                    "          --anchor-path       Restrict to this anchor path\n"
                                    "          --anchor-kind       Restrict to an anchor kind: file, entity\n"
                                    "          --target-kind       Restrict to a target kind: plan, task (needs --target-id)\n"
                                    "          --target-id :INT    Restrict to this target id (needs --target-kind)\n"
                                    "          --status            Restrict to a status: active, resolved, dismissed, archived\n"
                                    "          --plan :INT         Restrict to annotations on this plan id\n"
                                    "          --task :INT         Restrict to annotations on this task id\n"
                                    "          --vendor            Restrict to annotations by this vendor\n"
                                    "          --tag               Restrict to annotations carrying this tag\n"
                                    "          --scope             Restrict to this scope slug\n"
                                    "          --json              Emit machine-readable JSON instead of text\n");

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
                             "          --json              Emit machine-readable JSON instead of text\n");

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
                             "          --json              Emit machine-readable JSON instead of text\n");
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
                           "  plan REQUIRED               Plan id or slug\n"
                           "\n"
                           "OPTIONS:\n"
                           "  -h,     --help              Print this help message and exit\n"
                           "          --verbose           Verbose text rendering of the push result\n"
                           "          --json              Emit machine-readable JSON instead of text\n"
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
                         "  plan                        Plan id or slug (omit with --all-scopes)\n"
                         "\n"
                         "OPTIONS:\n"
                         "  -h,     --help              Print this help message and exit\n"
                         "          --dry-run           Preview only; do not touch disk\n"
                         "          --yes               Discard FS-content drift; remove drifted files anyway\n"
                         "          --filter-mode       Terminal-status filter: 'failures' (default) or 'all'\n"
                         "          --all-scopes        Walk every plan's workbench tree\n"
                         "          --json              Emit machine-readable JSON instead of text\n");
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

TEST_CASE("a seeded workbench feature tree is pinned", "[cmd][parity][cli-surface][workbench]") {
  // TASK 6542 RETIREMENT: confirmed byte-identical against the oracle at
  // this commit -- both the ordered CLI step outputs (after normalizing
  // each arena's own random root to `<ARENA>`, since `make_arena` keys it
  // off `steady_clock` and it could never be pinned literally, oracle or
  // not) and the RENDERED FILES themselves, which are the real product.
  // Pinned against the C++ binary alone from here; see `the CLI surface
  // is CLI11's now, and pinned` for the pattern.
  auto const space = make_arena("wbtree");
  auto const seed  = std::to_array<std::vector<std::string>>({
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
  });
  for (std::size_t i = 0; i < seed.size(); ++i) {
    auto const tag = std::format("seed{}", i);
    auto const ran = run_pinned(cpp_bin(), seed[i], space.cpp_root, tag);
    INFO("seed step: " << tag << " -> " << ran.err);
    REQUIRE(ran.code == 0);
    if (i == 1) {
      std::vector<std::string> const attach{"assoc", "add", "project:demo", (space.cpp_root / "proj").string()};
      REQUIRE(run_pinned(cpp_bin(), attach, space.cpp_root, std::format("{}attach", tag)).code == 0);
    }
  }

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

  struct step {
    std::string_view         tag;  ///< Case discriminator.
    std::vector<std::string> args; ///< The argv tail.
    int                      code; ///< Expected exit code.
    std::string_view         out;  ///< Expected stdout, arena root already normalized.
    std::string_view         err;  ///< Expected stderr, arena root already normalized.
  };
  // ORDERED against ONE tree: the filter counts only appear once a task is
  // cancelled, and `restore` only means anything after `archive`.
  std::vector<step> const steps{
      {"wblist0",
       {"workbench", "list"},
       0,
       "p1-demo-feature                           draft       no-tree     project:demo\n",
       ""},
      {"wbstat0", {"workbench", "status"}, 0, "no active features found\n", ""},
      {"wbpush",
       {"workbench", "push", "1", "--verbose"},
       0,
       "workbench push: plan 1 (demo-feature)\n"
       "  applied DB->FS: project_demo/p1-demo-feature/README.md\n"
       "  applied DB->FS: project_demo/p1-demo-feature/1-tech-spec-auth.md\n"
       "  applied DB->FS: project_demo/p1-demo-feature/decisions/1-use-sqlite.md\n"
       "  applied DB->FS: project_demo/p1-demo-feature/questions/1-which-format.md\n"
       "  applied DB->FS: project_demo/p1-demo-feature/scenarios/1-round-trip.md\n"
       "  applied DB->FS: project_demo/p1-demo-feature/tasks/cross/1-first-task.md\n"
       "  applied DB->FS: project_demo/p1-demo-feature/tasks/cross/2-second-task.md\n"
       "  applied DB->FS: project_demo/p1-demo-feature/plans/child-ms.md\n",
       ""},
      {"wbpushj",
       {"workbench", "push", "1", "--json"},
       0,
       "{\"applied\":0,\"pending\":0,\"conflicts\":0,\"malformed\":0,\"malformed_files\":[],\"filtered\":0,\"pre_existing_"
       "terminal\":0,\"cleaned\":0,\"filter_mode\":\"failures\",\"entries\":[{\"class\":\"no_op\",\"file_path\":\"project_demo/"
       "p1-demo-feature/"
       "README.md\",\"entity_kind\":\"plan\",\"entity_id\":1,\"conflict_id\":0,\"parse_error\":\"\"},{\"class\":\"no_op\",\"file_"
       "path\":\"project_demo/p1-demo-feature/"
       "1-tech-spec-auth.md\",\"entity_kind\":\"artifact\",\"entity_id\":1,\"conflict_id\":0,\"parse_error\":\"\"},{\"class\":"
       "\"no_op\",\"file_path\":\"project_demo/p1-demo-feature/decisions/"
       "1-use-sqlite.md\",\"entity_kind\":\"decision\",\"entity_id\":1,\"conflict_id\":0,\"parse_error\":\"\"},{\"class\":\"no_"
       "op\",\"file_path\":\"project_demo/p1-demo-feature/questions/"
       "1-which-format.md\",\"entity_kind\":\"question\",\"entity_id\":1,\"conflict_id\":0,\"parse_error\":\"\"},{\"class\":\"no_"
       "op\",\"file_path\":\"project_demo/p1-demo-feature/scenarios/"
       "1-round-trip.md\",\"entity_kind\":\"scenario\",\"entity_id\":1,\"conflict_id\":0,\"parse_error\":\"\"},{\"class\":\"no_"
       "op\",\"file_path\":\"project_demo/p1-demo-feature/tasks/cross/"
       "1-first-task.md\",\"entity_kind\":\"task\",\"entity_id\":1,\"conflict_id\":0,\"parse_error\":\"\"},{\"class\":\"no_op\","
       "\"file_path\":\"project_demo/p1-demo-feature/tasks/cross/"
       "2-second-task.md\",\"entity_kind\":\"task\",\"entity_id\":2,\"conflict_id\":0,\"parse_error\":\"\"},{\"class\":\"no_op\","
       "\"file_path\":\"project_demo/p1-demo-feature/plans/"
       "child-ms.md\",\"entity_kind\":\"plan\",\"entity_id\":2,\"conflict_id\":0,\"parse_error\":\"\"}],\"field_edit_refused\":0,"
       "\"field_edit_refusals\":[]}\n",
       ""},
      {"wbstat1", {"workbench", "status", "1", "--verbose"}, 0, "workbench status: plan 1 (demo-feature)\n", ""},
      {"wblist1",
       {"workbench", "list", "--json"},
       0,
       "[{\"plan\":1,\"slug\":\"demo-feature\",\"status\":\"draft\",\"assoc\":\"project:demo\",\"plan_key\":\"p1\",\"has_fs_"
       "tree\":true}]\n",
       ""},
      {"wblint", {"workbench", "lint", "1"}, 0, "8 files scanned, 0 errors, 0 warnings.\n", ""},
      {"wblintall", {"workbench", "lint", "--all"}, 0, "8 files scanned, 0 errors, 0 warnings.\n", ""},
      {"wbgcdry",
       {"workbench", "gc", "1", "--dry-run"},
       0,
       "workbench gc (--dry-run): would remove 0, keep 8, drifted-skipped 0, errors 0 (mode=failures)\n",
       ""},
      {"wbpull", {"workbench", "pull", "1", "--verbose"}, 0, "workbench pull: plan 1 (demo-feature)\n", ""},
      {"wbbadplan", {"workbench", "push", "999"}, 1, "", "error: plan not found: 999\n"},
      {"wbbadplan0", {"workbench", "push", "0"}, 2, "", "error: invalid plan '0'\n"},
      {"wbchild", {"workbench", "push", "2"}, 1, "", "error: plan not found: 2\n"},
      {"wbbadmode",
       {"workbench", "push", "1", "--filter-mode", "nope"},
       2,
       "",
       "error: invalid --filter-mode 'nope' (expected 'failures' or 'all')\n"},
      {"wbexcl",
       {"workbench", "push", "1", "--filter-mode", "all", "--apply-cleanup"},
       2,
       "",
       "error: --apply-cleanup is mutually exclusive with --filter-mode all\n"},
      {"wblintnone",
       {"workbench", "lint"},
       2,
       "",
       "error: choose exactly one lint target: <plan>, --all, or --path <file-or-directory>\n"},
      {"wbgcnoplan", {"workbench", "gc"}, 2, "", "error: plan argument required unless --all-scopes is set\n"},
      {"wbresbad", {"workbench", "resolve", "abc", "--prefer", "fs"}, 2, "", "error: event-id must be an integer, got 'abc'\n"},
      {"wbresnf", {"workbench", "resolve", "99", "--prefer", "fs"}, 1, "", "error: workbench resolve failed: NotFound\n"},
      {"wbarch",
       {"workbench", "archive", "1", "--json"},
       0,
       "{\"archived\":true,\"plan\":1,\"feature_dir\":\"<ARENA>/workbench/project_demo/p1-demo-feature\"}\n",
       ""},
      {"wbarch2", {"workbench", "archive", "1"}, 0, "archived: <ARENA>/workbench/project_demo/p1-demo-feature (plan 1)\n", ""},
      {"wbrest",
       {"workbench", "restore", "1", "--json"},
       0,
       "{\"restored\":true,\"plan\":1,\"feature_dir\":\"<ARENA>/workbench/project_demo/p1-demo-feature\"}\n",
       ""},
      {"wbgcall",
       {"workbench", "gc", "--all-scopes", "--json"},
       0,
       "{\"removed\":0,\"kept\":8,\"drifted_skipped\":0,\"errors\":0,\"dry_run\":false,\"filter_mode\":\"failures\"}\n",
       ""},
  };
  for (auto const& step : steps) {
    auto const mine = run_pinned(cpp_bin(), step.args, space.cpp_root, step.tag);
    INFO("step: " << step.tag);
    CHECK(mine.code == step.code);
    CHECK(normalize(mine.out, space.cpp_root) == step.out);
    CHECK(normalize(mine.err, space.cpp_root) == step.err);
  }

  // The RENDERED FILES are the real product, and none of the summary
  // lines above would notice a byte-level divergence in them. Every file
  // under the workbench root is pinned whole -- `**Created:**` /
  // `**Updated:**` lines dropped (wall-clock, cannot be pinned), and
  // `.sync`'s third (content hash) and fourth (`last_synced_at`) columns
  // dropped the same way the live diff used to drop them.
  auto const strip_created_updated = [](std::string_view text) {
    std::string kept;
    for (auto const line : std::views::split(text, '\n')) {
      std::string_view view{line.begin(), line.end()};
      if (view.starts_with("**Created:**") || view.starts_with("**Updated:**")) {
        continue;
      }
      kept.append(view);
      kept += '\n';
    }
    return kept;
  };
  auto const strip_sync_tail = [](std::string_view text) {
    std::string trimmed;
    for (auto const line : std::views::split(text, '\n')) {
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
    return trimmed;
  };
  auto const read_rendered = [&](std::string_view rel) {
    auto content = strip_created_updated(read_all(space.cpp_root / "workbench" / rel));
    if (std::string_view{rel}.ends_with(".sync")) {
      content = strip_sync_tail(content);
    }
    return content;
  };

  static constexpr std::string_view feature_dir = "project_demo/p1-demo-feature";
  CHECK(read_rendered(std::format("{}/README.md", feature_dir)) ==
        R"WBFILE6542(---
entity_kind: plan
entity_id: 1
anchor_plan_id: 1
title: Demo Feature
status: draft
---

# Plan 1: Demo Feature

**Status:** draft  

A demo.

)WBFILE6542");
  CHECK(read_rendered(std::format("{}/1-tech-spec-auth.md", feature_dir)) ==
        R"WBFILE6542(---
entity_kind: artifact
entity_id: 1
anchor_plan_id: 1
title: 'Tech Spec: Auth'
status: draft
artifact_kind: tech_spec
---

# Artifact 1: Tech Spec: Auth

**Kind:** tech_spec  
**Status:** draft

## Content

Spec body.

)WBFILE6542");
  CHECK(read_rendered(std::format("{}/decisions/1-use-sqlite.md", feature_dir)) ==
        R"WBFILE6542(---
entity_kind: decision
entity_id: 1
anchor_plan_id: 1
title: Use SQLite
status: proposed
---

# Decision 1: Use SQLite

**Status:** proposed  

## Body

We use SQLite.

## Rationale

Simple.

)WBFILE6542");
  CHECK(read_rendered(std::format("{}/questions/1-which-format.md", feature_dir)) ==
        R"WBFILE6542(---
entity_kind: question
entity_id: 1
anchor_plan_id: 1
title: Which format?
status: open
---

# Question 1: Which format?

**Status:** open  

)WBFILE6542");
  CHECK(read_rendered(std::format("{}/scenarios/1-round-trip.md", feature_dir)) ==
        R"WBFILE6542(---
entity_kind: scenario
entity_id: 1
anchor_plan_id: 1
title: Round trip
status: draft
---

# Scenario 1: Round trip

**Status:** draft  

)WBFILE6542");
  CHECK(read_rendered(std::format("{}/tasks/cross/1-first-task.md", feature_dir)) ==
        R"WBFILE6542(---
entity_kind: task
entity_id: 1
anchor_plan_id: 1
title: First Task
status: todo
priority: 100
---

# Task 1: First Task

**Status:** todo  
**Priority:** 100  

Task body here.

)WBFILE6542");
  CHECK(read_rendered(std::format("{}/tasks/cross/2-second-task.md", feature_dir)) ==
        R"WBFILE6542(---
entity_kind: task
entity_id: 2
anchor_plan_id: 1
title: Second Task
status: todo
priority: 100
---

# Task 2: Second Task

**Status:** todo  
**Priority:** 100  


)WBFILE6542");
  CHECK(read_rendered(std::format("{}/plans/child-ms.md", feature_dir)) ==
        R"WBFILE6542(---
entity_kind: plan
entity_id: 2
anchor_plan_id: 1
title: Child Milestone
status: draft
---

# Plan 2: Child Milestone

**Status:** draft  


)WBFILE6542");
  CHECK(read_rendered(std::format("{}/.sync", feature_dir)) ==
        R"WBFILE6542(1-tech-spec-auth.md	artifact:1
README.md	plan:1
decisions/1-use-sqlite.md	decision:1
plans/child-ms.md	plan:2
questions/1-which-format.md	question:1
scenarios/1-round-trip.md	scenario:1
tasks/cross/1-first-task.md	task:1
tasks/cross/2-second-task.md	task:2
)WBFILE6542");
}

TEST_CASE("init, including the git remote it captures, is pinned", "[cmd][parity][cli-surface][init]") {
  // TASK 6542 RETIREMENT: confirmed byte-identical against the oracle at
  // this commit, after scrubbing each side's own arena root to `$ROOT` —
  // `init` echoes its own database path and project root verbatim, and the
  // arena is random per run (`make_arena` keys it off `steady_clock`), so
  // the root is the one thing that could never be pinned literally, oracle
  // or not. What survives the scrub — schema version, row id, derived slug,
  // derived name, captured remote, key order, omission rules, terminator —
  // is still a byte-for-byte pin. Pinned against the C++ binary alone from
  // here; see `the CLI surface is CLI11's now, and pinned` for the pattern.
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
  // "the first remote" would capture the wrong URL; it is the ORACLE that
  // decided which is right at the time this was confirmed, and it answers
  // `origin`.
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
    int                      code;     ///< Expected exit code.
    std::string_view         out;      ///< Expected stdout, arena root already scrubbed to `$ROOT`.
  };
  std::vector<shape> const shapes{
      {"initplain",
       {"init"},
       false,
       0,
       "planar initialized\n"
       "  db:      $ROOT/planar.db\n"
       "  schema:  41\n"
       "  project: proj (id: 1)\n"
       "  next:    `planar assoc create project:proj --kind project`\n"
       "           `planar assoc add project:proj $ROOT/proj`\n"},
      {"initjson",
       {"init", "--json"},
       false,
       0,
       "{\"ok\":true,\"db\":\"$ROOT/"
       "planar.db\",\"schema_version\":41,\"project_id\":1,\"project_slug\":\"proj\",\"project_name\":"
       "\"proj\",\"root_path\":\"$ROOT/proj\"}\n"},
      {"initskip", {"init", "--skip-project"}, false, 0, "planar initialized\n  db:      $ROOT/planar.db\n  schema:  41\n"},
      {"initskipj",
       {"init", "--skip-project", "--json"},
       false,
       0,
       "{\"ok\":true,\"db\":\"$ROOT/planar.db\",\"schema_version\":41}\n"},
      // Declared, and never read by either binary. Probed rather than
      // assumed: `init` in a non-git directory succeeds WITHOUT it.
      {"initanr",
       {"init", "--allow-no-repo"},
       false,
       0,
       "planar initialized\n"
       "  db:      $ROOT/planar.db\n"
       "  schema:  41\n"
       "  project: proj (id: 1)\n"
       "  next:    `planar assoc create project:proj --kind project`\n"
       "           `planar assoc add project:proj $ROOT/proj`\n"},
      {"initnameslug",
       {"init", "--name", "My Proj", "--slug", "custom-slug", "--json"},
       false,
       0,
       "{\"ok\":true,\"db\":\"$ROOT/planar.db\",\"schema_version\":41,\"project_id\":1,\"project_slug\":\"custom-slug\","
       "\"project_name\":\"My Proj\",\"root_path\":\"$ROOT/proj\"}\n"},
      // THE `git_remote` CASES. Without these the whole column is invisible
      // to this file.
      {"initremote",
       {"init", "--json"},
       true,
       0,
       "{\"ok\":true,\"db\":\"$ROOT/"
       "planar.db\",\"schema_version\":41,\"project_id\":1,\"project_slug\":\"proj\",\"project_name\":"
       "\"proj\",\"root_path\":\"$ROOT/proj\",\"git_remote\":\"git@github.com:example/repo.git\"}\n"},
      {"initremotetext",
       {"init"},
       true,
       0,
       "planar initialized\n"
       "  db:      $ROOT/planar.db\n"
       "  schema:  41\n"
       "  project: proj (id: 1)\n"
       "  next:    `planar assoc create project:proj --kind project`\n"
       "           `planar assoc add project:proj $ROOT/proj`\n"},
  };

  for (auto const& shape : shapes) {
    auto const space = make_arena(shape.tag);
    if (shape.git_repo) {
      if (!seed_repo(space.cpp_root)) {
        WARN("git unavailable — skipping the remote-capture shape " << shape.tag);
        continue;
      }
    }
    auto const mine = run_pinned(cpp_bin(), shape.args, space.cpp_root, shape.tag);

    INFO("shape: " << shape.tag);
    CHECK(mine.code == shape.code);
    CHECK(scrub(mine.out, space.cpp_root) == shape.out);
    CHECK(mine.err.empty());

    auto const json_mode = std::ranges::find(shape.args, "--json") != shape.args.end();
    if (shape.git_repo && json_mode) {
      // The scrub cannot hide this one: the remote URL is not a path, so a
      // side that failed to capture it differs from a side that did.
      CHECK(mine.out.contains("git@github.com:example/repo.git"));
      CHECK_FALSE(mine.out.contains("up.git"));
    }
    if (shape.git_repo && !json_mode) {
      // FINDING, pinned rather than assumed away. The TEXT renderer never
      // mentions the remote at all — `planar init` in a repository with an
      // `origin` prints exactly what it prints without one, even though the
      // column IS written. `--json` is the only operator-visible signal for
      // `projects.git_remote`, which is precisely why this cycle asserts on
      // the ROW in handlers.t.cpp rather than trusting stdout: a text-only
      // test suite cannot distinguish a captured remote from a dropped one.
      CHECK_FALSE(mine.out.contains("git@github.com:example/repo.git"));
    }
  }
}

TEST_CASE("repeated init and --force are pinned", "[cmd][parity][cli-surface][init]") {
  // TASK 6542 RETIREMENT: confirmed byte-identical against the oracle at
  // this commit, after the same `$ROOT` scrub as the sibling init case
  // above. Pinned against the C++ binary alone from here.
  auto const scrub = [](std::string text, const std::filesystem::path& root) {
    auto const needle = root.string();
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 5)) {
      text.replace(at, needle.size(), "$ROOT");
    }
    return text;
  };

  // Ordered against ONE database: the second `init` must see the row the
  // first wrote, which is the whole point. A fresh arena per step would
  // only ever compare first-insert behaviour and the INSERT OR IGNORE rule
  // would go untested.
  struct step {
    std::string_view         tag;  ///< Case discriminator.
    std::vector<std::string> args; ///< The argv tail.
    std::string_view         out;  ///< Expected stdout, arena root already scrubbed to `$ROOT`.
  };
  std::vector<step> const steps{
      {"i1",
       {"init", "--json"},
       "{\"ok\":true,\"db\":\"$ROOT/"
       "planar.db\",\"schema_version\":41,\"project_id\":1,\"project_slug\":\"proj\",\"project_name\":"
       "\"proj\",\"root_path\":\"$ROOT/proj\"}\n"},
      // Idempotent: `--name` is NOT applied on the second run, because the
      // insert is OR IGNORE.
      {"i2",
       {"init", "--name", "Ignored", "--json"},
       "{\"ok\":true,\"db\":\"$ROOT/"
       "planar.db\",\"schema_version\":41,\"project_id\":1,\"project_slug\":\"proj\",\"project_name\":"
       "\"proj\",\"root_path\":\"$ROOT/proj\"}\n"},
      // `--force` repoints the SAME row id rather than inserting a second.
      {"i3",
       {"init", "--force", "--name", "Renamed", "--json"},
       "{\"ok\":true,\"db\":\"$ROOT/"
       "planar.db\",\"schema_version\":41,\"project_id\":1,\"project_slug\":\"proj\",\"project_name\":"
       "\"Renamed\",\"root_path\":\"$ROOT/proj\"}\n"},
      {"i4",
       {"init"},
       "planar initialized\n"
       "  db:      $ROOT/planar.db\n"
       "  schema:  41\n"
       "  project: proj (id: 1)\n"
       "  next:    `planar assoc create project:proj --kind project`\n"
       "           `planar assoc add project:proj $ROOT/proj`\n"},
  };

  auto const space = make_arena("initidem");
  for (auto const& step : steps) {
    auto const mine = run_pinned(cpp_bin(), step.args, space.cpp_root, step.tag);

    INFO("step: " << step.tag);
    CHECK(mine.code == 0);
    CHECK(scrub(mine.out, space.cpp_root) == step.out);
    CHECK(mine.err.empty());
  }
  // The row id never moved: three registrations, one project.
  CHECK(scrub(read_all(space.cpp_root / "i3.out"), space.cpp_root)
            .contains(R"("project_id":1,"project_slug":"proj","project_name":"Renamed")"));
}

TEST_CASE("a seeded local sandbox lifecycle is pinned", "[cmd][parity][cli-surface][local]") {
  // TASK 6542 RETIREMENT: confirmed byte-identical against the oracle at
  // this commit -- both the ordered CLI step outputs and the persisted
  // state (manifests, installed symlink tree). Pinned against the C++
  // binary alone from here; see `the CLI surface is CLI11's now, and
  // pinned` for the pattern.
  auto const space = make_arena("localtree");

  struct source {
    std::string_view rel;  ///< Path under `<localhome>/.planar/local`.
    std::string_view body; ///< File contents.
  };
  // ONE malformed entry only. `manifest::walk_sandbox` appends walk errors in
  // DIRECTORY-ITERATION order, which is unspecified, so the SET of warning
  // lines is the contract and their ORDER is not -- confirmed stable against
  // the oracle at this commit with exactly one malformed entry.
  std::vector<source> const sources{
      {"skills/three/SKILL.md", "---\nname: three\ndescription: Three vendors.\nvendors: [claude, codex, copilot]\n---\n\nB.\n"},
      {"skills/nodesc/SKILL.md", "---\nname: nodesc\n---\n\nNo description, so this LINTS.\n"},
      {"skills/flat.md", "---\nname: flat\ndescription: Legacy flat shape.\n---\n\nFlat.\n"},
      {"agents/an-agent.md", "---\nname: an-agent\ndescription: An agent.\n---\n\nA.\n"},
  };
  {
    std::error_code ec;
    auto const      sandbox = space.cpp_root / "localhome" / ".planar" / "local";
    std::filesystem::create_directories(sandbox / "skills" / "broken", ec); // the one walk error
    std::filesystem::create_directories(sandbox / "agents", ec);
    for (auto const& [rel, body] : sources) {
      auto const path = sandbox / rel;
      std::filesystem::create_directories(path.parent_path(), ec);
      std::ofstream file(path, std::ios::binary | std::ios::trunc);
      REQUIRE(file.is_open());
      file << body;
    }
  }

  struct step {
    std::string_view         tag;  ///< Case discriminator.
    std::vector<std::string> args; ///< The argv tail.
    int                      code; ///< Expected exit code.
    std::string_view         out;  ///< Expected stdout, normalized (see below).
    std::string_view         err;  ///< Expected stderr, normalized (see below).
  };
  // An ORDERED sequence against ONE sandbox: `list` only means anything
  // after `link`, `migrate` only after the flat source has been seen, and
  // `--reconcile` only after something is stale.
  std::vector<step> const steps{
      {"lclist0", {"local", "list"}, 0, "no sandbox installs recorded\n", ""},
      {"lclistj0", {"local", "list", "--json"}, 0, "", ""},
      {"lclinkdry",
       {"local", "link", "--dry-run"},
       0,
       "warning: <ARENA>/localhome/.planar/local/skills/broken/SKILL.md: skill directory missing SKILL.md\n"
       "warning: <ARENA>/localhome/.planar/local/skills/flat.md: legacy flat skill file; run `planar local migrate` to convert "
       "to flat/SKILL.md\n"
       "lint [warning] skill/nodesc.description: description is empty; vendors surface this as the skill summary\n"
       "nodesc (skill)\n"
       "  claude   dry-run [symlink]  ->  <ARENA>/localhome/.claude/commands/local-nodesc.md\n"
       "  codex    dry-run [symlink]  ->  <ARENA>/localhome/.codex/skills/local-nodesc\n"
       "  copilot  dry-run [symlink]  ->  <ARENA>/localhome/.copilot/skills/local-nodesc\n"
       "three (skill)\n"
       "  claude   dry-run [symlink]  ->  <ARENA>/localhome/.claude/commands/local-three.md\n"
       "  codex    dry-run [symlink]  ->  <ARENA>/localhome/.codex/skills/local-three\n"
       "  copilot  dry-run [symlink]  ->  <ARENA>/localhome/.copilot/skills/local-three\n"
       "an-agent (agent)\n"
       "  agents   dry-run [symlink]  ->  <ARENA>/localhome/.planar/agents/local-an-agent.md\n"
       "\n"
       "dry-run: 7 would-be installs across 3 source(s)\n",
       ""},
      {"lclink",
       {"local", "link"},
       0,
       "warning: <ARENA>/localhome/.planar/local/skills/broken/SKILL.md: skill directory missing SKILL.md\n"
       "warning: <ARENA>/localhome/.planar/local/skills/flat.md: legacy flat skill file; run `planar local migrate` to convert "
       "to flat/SKILL.md\n"
       "lint [warning] skill/nodesc.description: description is empty; vendors surface this as the skill summary\n"
       "nodesc (skill)\n"
       "  claude   created [symlink]  ->  <ARENA>/localhome/.claude/commands/local-nodesc.md\n"
       "  codex    created [symlink]  ->  <ARENA>/localhome/.codex/skills/local-nodesc\n"
       "  copilot  created [symlink]  ->  <ARENA>/localhome/.copilot/skills/local-nodesc\n"
       "three (skill)\n"
       "  claude   created [symlink]  ->  <ARENA>/localhome/.claude/commands/local-three.md\n"
       "  codex    created [symlink]  ->  <ARENA>/localhome/.codex/skills/local-three\n"
       "  copilot  created [symlink]  ->  <ARENA>/localhome/.copilot/skills/local-three\n"
       "an-agent (agent)\n"
       "  agents   created [symlink]  ->  <ARENA>/localhome/.planar/agents/local-an-agent.md\n"
       "\n"
       "done: 7 linked, 0 unchanged, 0 skipped across 3 source(s)\n",
       ""},
      {"lclink2",
       {"local", "link"},
       0,
       "warning: <ARENA>/localhome/.planar/local/skills/broken/SKILL.md: skill directory missing SKILL.md\n"
       "warning: <ARENA>/localhome/.planar/local/skills/flat.md: legacy flat skill file; run `planar local migrate` to convert "
       "to flat/SKILL.md\n"
       "lint [warning] skill/nodesc.description: description is empty; vendors surface this as the skill summary\n"
       "nodesc (skill)\n"
       "  claude   unchanged [symlink]  ->  <ARENA>/localhome/.claude/commands/local-nodesc.md\n"
       "  codex    unchanged [symlink]  ->  <ARENA>/localhome/.codex/skills/local-nodesc\n"
       "  copilot  unchanged [symlink]  ->  <ARENA>/localhome/.copilot/skills/local-nodesc\n"
       "three (skill)\n"
       "  claude   unchanged [symlink]  ->  <ARENA>/localhome/.claude/commands/local-three.md\n"
       "  codex    unchanged [symlink]  ->  <ARENA>/localhome/.codex/skills/local-three\n"
       "  copilot  unchanged [symlink]  ->  <ARENA>/localhome/.copilot/skills/local-three\n"
       "an-agent (agent)\n"
       "  agents   unchanged [symlink]  ->  <ARENA>/localhome/.planar/agents/local-an-agent.md\n"
       "\n"
       "done: 0 linked, 7 unchanged, 0 skipped across 3 source(s)\n",
       ""},
      {"lclinkj",
       {"local", "link", "--json"},
       0,
       "{\"Source\":{\"SourcePath\":\"<ARENA>/localhome/.planar/local/skills/nodesc/"
       "SKILL.md\",\"Name\":\"nodesc\",\"Kind\":\"skill\",\"Frontmatter\":{\"Description\":\"\",\"ArgumentHint\":\"\",\"Tier\":"
       "\"\",\"Model\":\"\",\"Shadow\":false,\"Vendors\":[],\"Kind\":\"\"},\"Body\":\"No description, so this "
       "LINTS.\\n\"},\"Records\":[{\"vendor\":\"claude\",\"target_path\":\"<ARENA>/localhome/.claude/commands/"
       "local-nodesc.md\",\"source_path\":\"<ARENA>/localhome/.planar/local/skills/nodesc/"
       "SKILL.md\",\"mode\":\"symlink\",\"action\":\"unchanged\",\"linked_at\":\"<STAMP>\"},{\"vendor\":\"codex\",\"target_"
       "path\":\"<ARENA>/localhome/.codex/skills/local-nodesc\",\"source_path\":\"<ARENA>/localhome/.planar/local/skills/"
       "nodesc\",\"mode\":\"symlink\",\"action\":\"unchanged\",\"linked_at\":\"<STAMP>\"},{\"vendor\":\"copilot\",\"target_"
       "path\":\"<ARENA>/localhome/.copilot/skills/local-nodesc\",\"source_path\":\"<ARENA>/localhome/.planar/local/skills/"
       "nodesc\",\"mode\":\"symlink\",\"action\":\"unchanged\",\"linked_at\":\"<STAMP>\"}]}\n"
       "{\"Source\":{\"SourcePath\":\"<ARENA>/localhome/.planar/local/skills/three/"
       "SKILL.md\",\"Name\":\"three\",\"Kind\":\"skill\",\"Frontmatter\":{\"Description\":\"Three "
       "vendors.\",\"ArgumentHint\":\"\",\"Tier\":\"\",\"Model\":\"\",\"Shadow\":false,\"Vendors\":[\"claude\",\"codex\","
       "\"copilot\"],\"Kind\":\"\"},\"Body\":\"B.\\n\"},\"Records\":[{\"vendor\":\"claude\",\"target_path\":\"<ARENA>/localhome/"
       ".claude/commands/local-three.md\",\"source_path\":\"<ARENA>/localhome/.planar/local/skills/three/"
       "SKILL.md\",\"mode\":\"symlink\",\"action\":\"unchanged\",\"linked_at\":\"<STAMP>\"},{\"vendor\":\"codex\",\"target_"
       "path\":\"<ARENA>/localhome/.codex/skills/local-three\",\"source_path\":\"<ARENA>/localhome/.planar/local/skills/"
       "three\",\"mode\":\"symlink\",\"action\":\"unchanged\",\"linked_at\":\"<STAMP>\"},{\"vendor\":\"copilot\",\"target_path\":"
       "\"<ARENA>/localhome/.copilot/skills/local-three\",\"source_path\":\"<ARENA>/localhome/.planar/local/skills/"
       "three\",\"mode\":\"symlink\",\"action\":\"unchanged\",\"linked_at\":\"<STAMP>\"}]}\n"
       "{\"Source\":{\"SourcePath\":\"<ARENA>/localhome/.planar/local/agents/"
       "an-agent.md\",\"Name\":\"an-agent\",\"Kind\":\"agent\",\"Frontmatter\":{\"Description\":\"An "
       "agent.\",\"ArgumentHint\":\"\",\"Tier\":\"\",\"Model\":\"\",\"Shadow\":false,\"Vendors\":[],\"Kind\":\"\"},\"Body\":\"A."
       "\\n\"},\"Records\":[{\"vendor\":\"agents\",\"target_path\":\"<ARENA>/localhome/.planar/agents/"
       "local-an-agent.md\",\"source_path\":\"<ARENA>/localhome/.planar/local/agents/"
       "an-agent.md\",\"mode\":\"symlink\",\"action\":\"unchanged\",\"linked_at\":\"<STAMP>\"}]}\n",
       ""},
      {"lclist1",
       {"local", "list"},
       0,
       "name          kind     vendor   status   target\n"
       "nodesc        skill    claude   live     <ARENA>/localhome/.claude/commands/local-nodesc.md\n"
       "nodesc        skill    codex    live     <ARENA>/localhome/.codex/skills/local-nodesc\n"
       "nodesc        skill    copilot  live     <ARENA>/localhome/.copilot/skills/local-nodesc\n"
       "three         skill    claude   live     <ARENA>/localhome/.claude/commands/local-three.md\n"
       "three         skill    codex    live     <ARENA>/localhome/.codex/skills/local-three\n"
       "three         skill    copilot  live     <ARENA>/localhome/.copilot/skills/local-three\n"
       "an-agent      agent    agents   live     <ARENA>/localhome/.planar/agents/local-an-agent.md\n",
       ""},
      {"lclistj1",
       {"local", "list", "--json"},
       0,
       "{\"Name\":\"nodesc\",\"Kind\":\"skill\",\"Record\":{\"vendor\":\"claude\",\"target_path\":\"<ARENA>/localhome/.claude/"
       "commands/local-nodesc.md\",\"source_path\":\"<ARENA>/localhome/.planar/local/skills/nodesc/"
       "SKILL.md\",\"mode\":\"symlink\",\"action\":\"live\",\"linked_at\":\"<STAMP>\"}}\n"
       "{\"Name\":\"nodesc\",\"Kind\":\"skill\",\"Record\":{\"vendor\":\"codex\",\"target_path\":\"<ARENA>/localhome/.codex/"
       "skills/local-nodesc\",\"source_path\":\"<ARENA>/localhome/.planar/local/skills/"
       "nodesc\",\"mode\":\"symlink\",\"action\":\"live\",\"linked_at\":\"<STAMP>\"}}\n"
       "{\"Name\":\"nodesc\",\"Kind\":\"skill\",\"Record\":{\"vendor\":\"copilot\",\"target_path\":\"<ARENA>/localhome/.copilot/"
       "skills/local-nodesc\",\"source_path\":\"<ARENA>/localhome/.planar/local/skills/"
       "nodesc\",\"mode\":\"symlink\",\"action\":\"live\",\"linked_at\":\"<STAMP>\"}}\n"
       "{\"Name\":\"three\",\"Kind\":\"skill\",\"Record\":{\"vendor\":\"claude\",\"target_path\":\"<ARENA>/localhome/.claude/"
       "commands/local-three.md\",\"source_path\":\"<ARENA>/localhome/.planar/local/skills/three/"
       "SKILL.md\",\"mode\":\"symlink\",\"action\":\"live\",\"linked_at\":\"<STAMP>\"}}\n"
       "{\"Name\":\"three\",\"Kind\":\"skill\",\"Record\":{\"vendor\":\"codex\",\"target_path\":\"<ARENA>/localhome/.codex/"
       "skills/local-three\",\"source_path\":\"<ARENA>/localhome/.planar/local/skills/"
       "three\",\"mode\":\"symlink\",\"action\":\"live\",\"linked_at\":\"<STAMP>\"}}\n"
       "{\"Name\":\"three\",\"Kind\":\"skill\",\"Record\":{\"vendor\":\"copilot\",\"target_path\":\"<ARENA>/localhome/.copilot/"
       "skills/local-three\",\"source_path\":\"<ARENA>/localhome/.planar/local/skills/"
       "three\",\"mode\":\"symlink\",\"action\":\"live\",\"linked_at\":\"<STAMP>\"}}\n"
       "{\"Name\":\"an-agent\",\"Kind\":\"agent\",\"Record\":{\"vendor\":\"agents\",\"target_path\":\"<ARENA>/localhome/.planar/"
       "agents/local-an-agent.md\",\"source_path\":\"<ARENA>/localhome/.planar/local/agents/"
       "an-agent.md\",\"mode\":\"symlink\",\"action\":\"live\",\"linked_at\":\"<STAMP>\"}}\n",
       ""},
      {"lclistv",
       {"local", "list", "--vendor", "codex"},
       0,
       "name          kind     vendor   status   target\n"
       "nodesc        skill    codex    live     <ARENA>/localhome/.codex/skills/local-nodesc\n"
       "three         skill    codex    live     <ARENA>/localhome/.codex/skills/local-three\n",
       ""},
      {"lclistvn",
       {"local", "list", "--vendor", "nope"},
       0,
       "name          kind     vendor   status   target\n"
       "no rows matched filter\n",
       ""},
      {"lclistve",
       {"local", "list", "--vendor", ""},
       0,
       "name          kind     vendor   status   target\n"
       "no rows matched filter\n",
       ""},
      {"lclinkone",
       {"local", "link", "three", "--vendor", "claude"},
       0,
       "warning: <ARENA>/localhome/.planar/local/skills/broken/SKILL.md: skill directory missing SKILL.md\n"
       "warning: <ARENA>/localhome/.planar/local/skills/flat.md: legacy flat skill file; run `planar local migrate` to convert "
       "to flat/SKILL.md\n"
       "three (skill)\n"
       "  claude   unchanged [symlink]  ->  <ARENA>/localhome/.claude/commands/local-three.md\n"
       "  codex    skipped  ->  <ARENA>/localhome/.codex/skills/local-three\n"
       "  copilot  skipped  ->  <ARENA>/localhome/.copilot/skills/local-three\n"
       "\n"
       "done: 0 linked, 1 unchanged, 2 skipped across 1 source(s)\n",
       ""},
      {"lclinkmiss",
       {"local", "link", "nosuch"},
       1,
       "warning: <ARENA>/localhome/.planar/local/skills/broken/SKILL.md: skill directory missing SKILL.md\n"
       "warning: <ARENA>/localhome/.planar/local/skills/flat.md: legacy flat skill file; run `planar local migrate` to convert "
       "to flat/SKILL.md\n",
       "error: no sandbox source named \"nosuch\" under <ARENA>/localhome/.planar/local\n"},
      {"lcrecpos", {"local", "link", "--reconcile", "stray"}, 2, "", "error: --reconcile takes no positional arguments\n"},
      {"lcmigdry",
       {"local", "migrate", "--dry-run"},
       0,
       "flat                  would migrate  <ARENA>/localhome/.planar/local/skills/flat.md -> "
       "<ARENA>/localhome/.planar/local/skills/flat/SKILL.md\n"
       "\n"
       "done: would migrate 1 skill(s); skipped 0\n",
       ""},
      {"lcmig",
       {"local", "migrate"},
       0,
       "flat                  migrated  <ARENA>/localhome/.planar/local/skills/flat.md -> "
       "<ARENA>/localhome/.planar/local/skills/flat/SKILL.md\n"
       "\n"
       "done: migrated 1 skill(s); skipped 0\n",
       ""},
      {"lcmig2", {"local", "migrate"}, 0, "migrate: no legacy flat skills found; sandbox is already dir-shape\n", ""},
      {"lcmigj", {"local", "migrate", "--json"}, 0, "{\"Migrated\":[],\"Skipped\":[]}\n", ""},
      {"lcunlink",
       {"local", "unlink", "an-agent"},
       0,
       "an-agent (agent)\n"
       "  agents   removed  <-  <ARENA>/localhome/.planar/agents/local-an-agent.md\n",
       ""},
      {"lcunlink2",
       {"local", "unlink", "an-agent"},
       0,
       "no installs found for \"an-agent\" (already unlinked, or no such name)\n",
       ""},
      {"lcunlinkj",
       {"local", "unlink", "nodesc", "--json"},
       0,
       "{\"result\":{\"Name\":\"nodesc\",\"Kind\":\"skill\",\"Removed\":[{\"vendor\":\"claude\",\"target_path\":\"<ARENA>/"
       "localhome/.claude/commands/local-nodesc.md\",\"source_path\":\"<ARENA>/localhome/.planar/local/skills/nodesc/"
       "SKILL.md\",\"mode\":\"symlink\",\"action\":\"removed\",\"linked_at\":\"<STAMP>\"},{\"vendor\":\"codex\",\"target_path\":"
       "\"<ARENA>/localhome/.codex/skills/local-nodesc\",\"source_path\":\"<ARENA>/localhome/.planar/local/skills/"
       "nodesc\",\"mode\":\"symlink\",\"action\":\"removed\",\"linked_at\":\"<STAMP>\"},{\"vendor\":\"copilot\",\"target_path\":"
       "\"<ARENA>/localhome/.copilot/skills/local-nodesc\",\"source_path\":\"<ARENA>/localhome/.planar/local/skills/"
       "nodesc\",\"mode\":\"symlink\",\"action\":\"removed\",\"linked_at\":\"<STAMP>\"}],\"PurgedFile\":\"\"}}\n",
       ""},
      {"lcunlinkghost",
       {"local", "unlink", "ghost", "--purge"},
       0,
       "ghost (skill)\n"
       "  purged source file: <ARENA>/localhome/.planar/local/skills/ghost\n"
       "ghost (agent)\n"
       "  purged source file: <ARENA>/localhome/.planar/local/agents/ghost.md\n"
       "no installs found for \"ghost\" (already unlinked, or no such name)\n",
       ""},
      {"lcrecdry",
       {"local", "link", "--reconcile", "--dry-run"},
       0,
       "reconcile: manifest already consistent with the filesystem\n",
       ""},
      {"lcrec", {"local", "link", "--reconcile"}, 0, "reconcile: manifest already consistent with the filesystem\n", ""},
      {"lclist2",
       {"local", "list"},
       0,
       "name          kind     vendor   status   target\n"
       "three         skill    claude   live     <ARENA>/localhome/.claude/commands/local-three.md\n",
       ""},
      {"lcimpbad",
       {"local", "import", "nope.md", "--kind", "bogus"},
       2,
       "",
       "error: --kind must be skill or agent, got \"bogus\"\n"},
      {"lcimpmiss",
       {"local", "import", "nope.md"},
       0,
       "nope          skipped       reason: invalid-frontmatter\n"
       "\n"
       "imported 0 file(s); skipped 1\n",
       ""},
  };

  // `linked_at` is a wall-clock second stamp that reaches the `--json`
  // payloads, so it cannot be pinned literally. Folding it to a token is
  // what keeps those steps comparable across runs; everything else,
  // including the arena root, is compared for real.
  auto const normalize = [](std::string_view text, const std::filesystem::path& root) {
    std::string out;
    std::string rest{text};
    for (auto const& [needle, token] : std::vector<std::pair<std::string, std::string>>{{root.string(), "<ARENA>"}}) {
      out.clear();
      std::string_view view = rest;
      for (;;) {
        auto const at = view.find(needle);
        if (at == std::string_view::npos) {
          out.append(view);
          break;
        }
        out.append(view.substr(0, at));
        out.append(token);
        view.remove_prefix(at + needle.size());
      }
      rest = out;
    }
    // `"linked_at": "2026-08-25T23:59:59Z"` -> `"linked_at": "<STAMP>"`.
    //
    // The separator between the key and the value is PRESERVED rather than
    // rewritten, because the two payloads this runs over are formatted
    // DIFFERENTLY: the `--json` stdout is compact (`"linked_at":"…"`) and the
    // on-disk `.link-manifest.json` is pretty-printed (`"linked_at": "…"`).
    // Only the VALUE is tokenised, so a side that changed its formatting
    // still diverges loudly.
    constexpr std::string_view k_key = "\"linked_at\"";
    std::string                folded;
    std::string_view           view = rest;
    for (;;) {
      auto const at = view.find(k_key);
      if (at == std::string_view::npos) {
        folded.append(view);
        return folded;
      }
      folded.append(view.substr(0, at));
      folded.append(k_key);
      view.remove_prefix(at + k_key.size());
      // Copy `<ws>*:<ws>*"` verbatim, then swap the quoted value for the
      // token. Anything else between the key and a quote is NOT a string
      // value (a `null`, a number, a different key) and is left alone rather
      // than folded blind.
      std::size_t open      = 0;
      bool        saw_colon = false;
      while (open < view.size() && (view[open] == ' ' || view[open] == '\t' || view[open] == '\n' || view[open] == '\r' ||
                                    (view[open] == ':' && !saw_colon))) {
        saw_colon = saw_colon || view[open] == ':';
        ++open;
      }
      if (!saw_colon || open >= view.size() || view[open] != '"') {
        continue;
      }
      folded.append(view.substr(0, open + 1));
      view.remove_prefix(open + 1);
      auto const close = view.find('"');
      folded.append("<STAMP>\"");
      view.remove_prefix(close == std::string_view::npos ? view.size() : close + 1);
    }
  };

  for (auto const& step : steps) {
    auto const mine = run_pinned(cpp_bin(), step.args, space.cpp_root, step.tag);
    INFO("step: " << step.tag);
    CHECK(mine.code == step.code);
    CHECK(normalize(mine.out, space.cpp_root) == step.out);
    CHECK(normalize(mine.err, space.cpp_root) == step.err);
  }

  // THE PERSISTED STATE IS THE REAL PRODUCT, and no summary line above
  // would notice a divergence inside it. Both manifests are pinned whole
  // (modulo the arena root and the stamp), and so is the SHAPE of the
  // vendor trees the links landed in.
  auto const manifests_of = [&](const std::filesystem::path& root) {
    std::map<std::string, std::string> files;
    auto const                         sandbox = root / "localhome" / ".planar" / "local";
    std::error_code                    ec;
    for (auto const& kind : {"skills", "agents"}) {
      auto const path          = sandbox / kind / ".link-manifest.json";
      files[std::string{kind}] = std::filesystem::exists(path, ec) ? normalize(read_all(path), root) : "<ABSENT>";
    }
    return files;
  };
  {
    auto const mine = manifests_of(space.cpp_root);
    REQUIRE(mine.at("skills") ==
            "{\n  \"version\": 1,\n  \"entries\": [\n    {\n      \"name\": \"three\",\n      \"source_path\": "
            "\"<ARENA>/localhome/.planar/local/skills/three/SKILL.md\",\n      \"links\": [\n        {\n          \"vendor\": "
            "\"claude\",\n          \"target_path\": \"<ARENA>/localhome/.claude/commands/local-three.md\",\n          "
            "\"source_path\": \"<ARENA>/localhome/.planar/local/skills/three/SKILL.md\",\n          \"mode\": \"symlink\",\n     "
            "     \"linked_at\": \"<STAMP>\"\n        }\n      ]\n    }\n  ]\n}\n");
    REQUIRE(mine.at("agents") == "{\n  \"version\": 1,\n  \"entries\": []\n}\n");
  }

  auto const installs_of = [](const std::filesystem::path& root) {
    std::map<std::string, std::string> entries;
    auto const                         home = root / "localhome";
    std::error_code                    ec;
    for (auto const& vendor_dir : {".claude", ".codex", ".copilot", ".planar"}) {
      auto const base = home / vendor_dir;
      if (!std::filesystem::is_directory(base, ec)) {
        continue;
      }
      for (auto const& entry :
           std::filesystem::recursive_directory_iterator(base, std::filesystem::directory_options::skip_permission_denied, ec)) {
        std::error_code entry_ec;
        auto const      rel = std::filesystem::relative(entry.path(), home, entry_ec).generic_string();
        // The sandbox itself lives under `.planar/local`; only the INSTALLS
        // beside it are the product.
        if (rel.starts_with(".planar/local")) {
          continue;
        }
        entries[rel] = std::filesystem::is_symlink(std::filesystem::symlink_status(entry.path(), entry_ec)) ? "symlink"
                       : std::filesystem::is_directory(entry.path(), entry_ec)                              ? "dir"
                                                                                                            : "regular";
      }
    }
    return entries;
  };
  auto const cpp_installs = installs_of(space.cpp_root);
  // NON-VACUITY: an empty walk would make this worthless, so the exact end
  // state is pinned rather than merely "non-empty".
  //
  // MEASURED, not reasoned about: the sequence's last mutating step is
  // `local link --reconcile`, which drops every install whose sandbox
  // source is gone -- and the preceding `unlink`/`--purge` steps removed
  // those sources. So the tree ends as three vendor directories plus
  // `.planar/agents`, with ZERO symlinks surviving. That "reconcile really
  // did remove them" is the property worth pinning here; an earlier draft
  // of this block asserted seven entries with `local-three` still linked,
  // which is what the sequence looks like several steps earlier.
  CHECK(cpp_installs.size() == 4);
  CHECK(cpp_installs.at(".claude/commands") == "dir");
  CHECK(cpp_installs.at(".codex/skills") == "dir");
  CHECK(cpp_installs.at(".copilot/skills") == "dir");
  CHECK(cpp_installs.at(".planar/agents") == "dir");
  // No symlink survives reconcile. Asserting the count alone would pass if
  // a stale symlink replaced a directory entry one-for-one.
  for (auto const& [rel, kind] : cpp_installs) {
    INFO("surviving install: " << rel);
    CHECK(kind == "dir");
  }
}

TEST_CASE("closure show and groups recommend over seeded rows are pinned", "[cmd][parity][cli-surface][closure]") {
  // TASK 6542 RETIREMENT: confirmed byte-identical against the oracle at
  // this commit. Pinned against the C++ binary alone from here; see `the
  // CLI surface is CLI11's now, and pinned` for the pattern.
  //
  // CONFIGURE NOTE (decision 1032): captured under a build with the
  // mtkahypar solver ON, per the parity lane's standing configure. It does
  // not matter here -- `groups.cpp` defaults `requested_solver` to
  // `greedy` unless `--solver mtkahypar` is passed explicitly (see that
  // file's own comment), and NO step below passes it. Every step either
  // omits `--solver` or names `greedy`/`bogus` explicitly, so every
  // expectation pinned here holds on EITHER arm of that configure -- unlike
  // task 6543's bug in a different test today, this case never reaches the
  // solver-dependent path at all. The solver-degradation behavior itself is
  // pinned separately, against this binary alone, in
  // closure_groups_leaves.t.cpp.
  auto const space = make_arena("clogrp");
  auto const seed  = std::to_array<std::vector<std::string>>({
      {"init", "--name", "demo", "--slug", "demo", "--allow-no-repo"},
      {"assoc", "create", "project:demo", "--kind", "project"},
      {"plan", "create", "Demo", "--slug", "demo-plan", "--summary", "S"},
      {"task", "add", "T1", "--plan", "1", "--editor=false"},
      {"task", "add", "T2", "--plan", "1", "--editor=false"},
      {"task", "add", "T3", "--plan", "1", "--editor=false"},
      {"task", "add", "T4", "--plan", "1", "--editor=false"},
  });
  for (std::size_t i = 0; i < seed.size(); ++i) {
    auto const tag = std::format("cgseed{}", i);
    auto const ran = run_pinned(cpp_bin(), seed[i], space.cpp_root, tag);
    INFO("seed step: " << tag << " -> " << ran.err);
    REQUIRE(ran.code == 0);
    if (i == 1) {
      std::vector<std::string> const attach{"assoc", "add", "project:demo", (space.cpp_root / "proj").string()};
      REQUIRE(run_pinned(cpp_bin(), attach, space.cpp_root, std::format("{}attach", tag)).code == 0);
    }
  }

  // `closures` has no ported writer -- `closure compute` is the unported
  // half of this family and needs tree-sitter -- so the rows are seeded
  // through `sqlite3` directly. Every value is pinned, `created_at`
  // included.
  //
  // The fixture is built to discriminate: two `modify` rows whose PATH
  // order and SYMBOL order DISAGREE, one `transitive` row heavy enough
  // (9000) that a slice including it could not report the asserted cost, a
  // `done` task (weight 5000) that must not be grouped, and a dependency
  // edge.
  constexpr std::string_view k_seed_sql =
      "insert into closures (task_id,repo_id,path,symbol,role,token_weight,extractor_version,created_at) values"
      " (1,1,'z/last.zig','a.aaa','modify',10,'m2-closure-0.1','2026-01-01T00:00:00.000Z'),"
      " (1,1,'a/first.zig','z.zzz','modify',20,'m2-closure-0.1','2026-01-01T00:00:00.000Z'),"
      " (1,1,'m/mid.zig','m.mmm','reference',5,'m2-closure-0.1','2026-01-01T00:00:00.000Z'),"
      " (1,1,'t/tr.zig','t.ttt','transitive',9000,'m2-closure-0.1','2026-01-01T00:00:00.000Z'),"
      " (1,1,'z/last.zig','a.aaa','modify',7,'m2-closure-0.2','2026-01-01T00:00:00.000Z'),"
      " (2,1,'b/two.zig','b.bbb','modify',30,'m2-closure-0.1','2026-01-01T00:00:00.000Z'),"
      " (3,1,'c/three.zig','c.ccc','modify',40,'m2-closure-0.1','2026-01-01T00:00:00.000Z'),"
      " (4,1,'d/four.zig','d.ddd','modify',5000,'m2-closure-0.1','2026-01-01T00:00:00.000Z');"
      "update tasks set status='done' where id=4;"
      "insert into entity_links (from_kind,from_id,to_kind,to_id,relationship,created_at)"
      " values ('task',2,'task',1,'depends-on','2026-01-01T00:00:00.000Z');";
  {
    auto const sql_path = space.cpp_root / "seed.sql";
    {
      std::ofstream file(sql_path, std::ios::binary | std::ios::trunc);
      REQUIRE(file.is_open());
      file << k_seed_sql;
    }
    auto const line =
        std::format("sqlite3 {} < {}", shell_quote((space.cpp_root / "planar.db").string()), shell_quote(sql_path.string()));
    REQUIRE(std::system(line.c_str()) == 0);
  }

  struct step {
    std::string_view         tag;  ///< Case discriminator.
    std::vector<std::string> args; ///< The argv tail.
    int                      code; ///< Expected exit code.
    std::string_view         out;  ///< Expected stdout.
    std::string_view         err;  ///< Expected stderr.
  };
  std::vector<step> const steps{
      {"cs1",
       {"closure", "show", "1"},
       0,
       "closure for task 1 (5 rows):\n"
       "  [modify] a/first.zig::z.zzz  w=20\n"
       "  [modify] z/last.zig::a.aaa  w=10\n"
       "  [modify] z/last.zig::a.aaa  w=7\n"
       "  [reference] m/mid.zig::m.mmm  w=5\n"
       "  [transitive] t/tr.zig::t.ttt  w=9000\n",
       ""},
      {"cs1j",
       {"closure", "show", "1", "--json"},
       0,
       "{\"task_id\":1,\"rows\":[{\"id\":2,\"repo_id\":1,\"path\":\"a/"
       "first.zig\",\"symbol\":\"z.zzz\",\"role\":\"modify\",\"token_weight\":20,\"extractor_version\":\"m2-closure-0.1\","
       "\"created_at\":\"2026-01-01T00:00:00.000Z\"},{\"id\":1,\"repo_id\":1,\"path\":\"z/"
       "last.zig\",\"symbol\":\"a.aaa\",\"role\":\"modify\",\"token_weight\":10,\"extractor_version\":\"m2-closure-0.1\","
       "\"created_at\":\"2026-01-01T00:00:00.000Z\"},{\"id\":5,\"repo_id\":1,\"path\":\"z/"
       "last.zig\",\"symbol\":\"a.aaa\",\"role\":\"modify\",\"token_weight\":7,\"extractor_version\":\"m2-closure-0.2\","
       "\"created_at\":\"2026-01-01T00:00:00.000Z\"},{\"id\":3,\"repo_id\":1,\"path\":\"m/"
       "mid.zig\",\"symbol\":\"m.mmm\",\"role\":\"reference\",\"token_weight\":5,\"extractor_version\":\"m2-closure-0.1\","
       "\"created_at\":\"2026-01-01T00:00:00.000Z\"},{\"id\":4,\"repo_id\":1,\"path\":\"t/"
       "tr.zig\",\"symbol\":\"t.ttt\",\"role\":\"transitive\",\"token_weight\":9000,\"extractor_version\":\"m2-closure-0.1\","
       "\"created_at\":\"2026-01-01T00:00:00.000Z\"}]}\n",
       ""},
      {"cs4",
       {"closure", "show", "4"},
       0,
       "closure for task 4 (1 rows):\n"
       "  [modify] d/four.zig::d.ddd  w=5000\n",
       ""},
      {"cs999",
       {"closure", "show", "999"},
       0,
       "closure for task 999 (0 rows):\n"
       "  (none — run `planar closure compute 999` first)\n",
       ""},
      {"cs999j", {"closure", "show", "999", "--json"}, 0, "{\"task_id\":999,\"rows\":[]}\n", ""},
      {"csbad", {"closure", "show", "notanint"}, 2, "", "error: task id must be an integer, got 'notanint'\n"},
      {"cssep",
       {"closure", "show", "1_0"},
       0,
       "closure for task 10 (0 rows):\n"
       "  (none — run `planar closure compute 10` first)\n",
       ""},
      {"cspad",
       {"closure", "show", "007"},
       0,
       "closure for task 7 (0 rows):\n"
       "  (none — run `planar closure compute 7` first)\n",
       ""},
      {"csplus",
       {"closure", "show", "+12"},
       0,
       "closure for task 12 (0 rows):\n"
       "  (none — run `planar closure compute 12` first)\n",
       ""},
      {"csovf",
       {"closure", "show", "9223372036854775808"},
       2,
       "",
       "error: task id must be an integer, got '9223372036854775808'\n"},
      {"csmax",
       {"closure", "show", "9223372036854775807"},
       0,
       "closure for task 9223372036854775807 (0 rows):\n"
       "  (none — run `planar closure compute 9223372036854775807` first)\n",
       ""},
      {"gr1",
       {"groups", "recommend", "1"},
       0,
       "plan:1  budget:128000  open:3  solver:greedy  optimal_available:false  selected_greedy:false  slices:2  total_cost:105\n"
       "slice 1  cost:65  tasks:[1, 2]\n"
       "    - a.aaa\n"
       "    - b.bbb\n"
       "    - m.mmm\n"
       "    - z.zzz\n"
       "slice 2  cost:40  tasks:[3]\n"
       "    - c.ccc\n",
       ""},
      {"gr1j",
       {"groups", "recommend", "1", "--json"},
       0,
       "{\"plan_id\":1,\"budget\":128000,\"open_tasks\":3,\"solver\":\"greedy\",\"optimal_available\":false,\"selected_greedy\":"
       "false,\"slices\":[{\"task_ids\":[1,2],\"union_symbols\":[\"a.aaa\",\"b.bbb\",\"m.mmm\",\"z.zzz\"],\"cost\":65},{\"task_"
       "ids\":[3],\"union_symbols\":[\"c.ccc\"],\"cost\":40}],\"summary\":{\"slices\":2,\"total_cost\":105}}\n",
       ""},
      {"grb",
       {"groups", "recommend", "1", "--budget", "25", "--json"},
       0,
       "{\"plan_id\":1,\"budget\":25,\"open_tasks\":3,\"solver\":\"greedy\",\"optimal_available\":false,\"selected_greedy\":"
       "false,\"slices\":[{\"task_ids\":[1],\"union_symbols\":[\"a.aaa\",\"m.mmm\",\"z.zzz\"],\"cost\":35},{\"task_ids\":[2],"
       "\"union_symbols\":[\"b.bbb\"],\"cost\":30},{\"task_ids\":[3],\"union_symbols\":[\"c.ccc\"],\"cost\":40}],\"summary\":{"
       "\"slices\":3,\"total_cost\":105}}\n",
       ""},
      {"grb0",
       {"groups", "recommend", "1", "--budget", "0", "--json"},
       0,
       "{\"plan_id\":1,\"budget\":0,\"open_tasks\":3,\"solver\":\"greedy\",\"optimal_available\":false,\"selected_greedy\":false,"
       "\"slices\":[{\"task_ids\":[1],\"union_symbols\":[\"a.aaa\",\"m.mmm\",\"z.zzz\"],\"cost\":35},{\"task_ids\":[2],\"union_"
       "symbols\":[\"b.bbb\"],\"cost\":30},{\"task_ids\":[3],\"union_symbols\":[\"c.ccc\"],\"cost\":40}],\"summary\":{\"slices\":"
       "3,\"total_cost\":105}}\n",
       ""},
      {"grbmax",
       {"groups", "recommend", "1", "--budget", "4294967295", "--json"},
       0,
       "{\"plan_id\":1,\"budget\":4294967295,\"open_tasks\":3,\"solver\":\"greedy\",\"optimal_available\":false,\"selected_"
       "greedy\":false,\"slices\":[{\"task_ids\":[1,2],\"union_symbols\":[\"a.aaa\",\"b.bbb\",\"m.mmm\",\"z.zzz\"],\"cost\":65},{"
       "\"task_ids\":[3],\"union_symbols\":[\"c.ccc\"],\"cost\":40}],\"summary\":{\"slices\":2,\"total_cost\":105}}\n",
       ""},
      {"grbovf",
       {"groups", "recommend", "1", "--budget", "4294967296"},
       2,
       "",
       "error: --budget must be a non-negative integer, got '4294967296'\n"},
      {"grbneg",
       {"groups", "recommend", "1", "--budget", "-1"},
       2,
       "",
       "error: --budget must be a non-negative integer, got '-1'\n"},
      {"grbbad",
       {"groups", "recommend", "1", "--budget", "xyz"},
       2,
       "",
       "error: --budget must be a non-negative integer, got 'xyz'\n"},
      {"grmiss", {"groups", "recommend", "999"}, 1, "", "error: plan 999 not found\n"},
      {"grbadid", {"groups", "recommend", "abc"}, 2, "", "error: plan id must be an integer, got 'abc'\n"},
      {"grsolverbad",
       {"groups", "recommend", "1", "--solver", "bogus"},
       2,
       "",
       "error: --solver must be 'greedy' or 'mtkahypar', got 'bogus'\n"},
      {"grsolvergreedy",
       {"groups", "recommend", "1", "--solver", "greedy", "--json"},
       0,
       "{\"plan_id\":1,\"budget\":128000,\"open_tasks\":3,\"solver\":\"greedy\",\"optimal_available\":false,\"selected_greedy\":"
       "false,\"slices\":[{\"task_ids\":[1,2],\"union_symbols\":[\"a.aaa\",\"b.bbb\",\"m.mmm\",\"z.zzz\"],\"cost\":65},{\"task_"
       "ids\":[3],\"union_symbols\":[\"c.ccc\"],\"cost\":40}],\"summary\":{\"slices\":2,\"total_cost\":105}}\n",
       ""},
      // `--solver mtkahypar` is DELIBERATELY ABSENT (see the configure note
      // above): every step here already runs on the greedy path regardless
      // of whether the solver is compiled in.
  };

  for (auto const& step : steps) {
    auto const mine = run_pinned(cpp_bin(), step.args, space.cpp_root, step.tag);
    INFO("step: " << step.tag);
    CHECK(mine.code == step.code);
    CHECK(mine.out == step.out);
    CHECK(mine.err == step.err);
  }

  // NON-VACUITY. Both leaves are read-only, so a seed that silently failed
  // would make every comparison above a trivially-equal pair of empty
  // answers. This asserts the fixture actually reached the binary.
  auto const populated =
      run_pinned(cpp_bin(), std::vector<std::string>{"closure", "show", "1", "--json"}, space.cpp_root, "cgnonvac");
  CHECK(populated.out.contains("\"symbol\":\"z.zzz\""));
  CHECK(populated.out.contains("\"role\":\"transitive\""));
  auto const grouped =
      run_pinned(cpp_bin(), std::vector<std::string>{"groups", "recommend", "1", "--json"}, space.cpp_root, "cgnonvac2");
  CHECK(grouped.out.contains("\"open_tasks\":3"));
  // ...and the two filters this fixture exists to exercise really do exclude.
  CHECK_FALSE(grouped.out.contains("t.ttt"));
  CHECK_FALSE(grouped.out.contains("d.ddd"));
}

TEST_CASE("extract-questions and the workbench edit round trip are pinned", "[cmd][parity][cli-surface][workbench]") {
  // TASK 6542 RETIREMENT: confirmed byte-identical against the oracle at
  // this commit. Pinned against the C++ binary alone from here; see `the
  // CLI surface is CLI11's now, and pinned` for the pattern.
  //
  // THE FIXTURE IS BUILT BY A REAL `workbench push`, NOT BY HAND. This leaf
  // reads only TOP-LEVEL `.md` files and skips `README.md`, so the obvious
  // feature tree — a README plus `questions/` and `tasks/` subdirectories —
  // makes it return `[]`, and every comparison below would pass while
  // comparing two empty lists. Hand-authoring a `.md` does not escape it
  // either: the file must satisfy `workbench::parse::parse`, and a
  // malformed one is silently skipped, so the result is empty again for a
  // different reason. The `REQUIRE`s after the seed are what make that
  // failure loud instead of green.
  auto const        space       = make_arena("wbq");
  std::string const bullet_body = "Intro line.\n"
                                  "\n"
                                  "## Open Questions\n"
                                  "\n"
                                  "- Should we cache the result? It would help a lot on repeated reads and we think it "
                                  "matters.\n"
                                  "- What about eviction?\n"
                                  "  - nested bullet counts too\n"
                                  "\n"
                                  "## Next Section\n"
                                  "\n"
                                  "- not a question\n";
  std::string const h3_body     = "Preamble.\n"
                                  "\n"
                                  "## Open Questions\n"
                                  "\n"
                                  "### Which serializer?\n"
                                  "\n"
                                  "JSON is the default.\n"
                                  "It has two lines.\n"
                                  "\n"
                                  "### Do we version the payload?\n"
                                  "\n"
                                  "Yes, probably.\n"
                                  "\n"
                                  "# Terminator\n";

  auto const seed = std::to_array<std::vector<std::string>>({
      {"init", "--name", "demo", "--slug", "demo"},
      {"assoc", "create", "project:demo", "--kind", "project"},
      {"plan", "create", "Demo Feature", "--slug", "demo-feature", "--summary", "A demo."},
      // BOTH extraction branches, plus a spec with no section at all, plus
      // a question entity so the tree has a real subdirectory to ignore.
      {"artifact", "add", "Bullet Spec", "--kind", "tech_spec", "--plan", "1", "--body", bullet_body, "--editor=false"},
      {"artifact", "add", "H3 Spec", "--kind", "tech_spec", "--plan", "1", "--body", h3_body, "--editor=false"},
      {"artifact", "add", "Empty Spec", "--kind", "tech_spec", "--plan", "1", "--body", "No questions here.", "--editor=false"},
      {"question", "add", "Which format?", "--plan", "1"},
      {"plan", "create", "Unpushed", "--slug", "unpushed", "--summary", "no tree"},
  });
  for (std::size_t i = 0; i < seed.size(); ++i) {
    auto const tag = std::format("wbqseed{}", i);
    auto const ran = run_pinned(cpp_bin(), seed[i], space.cpp_root, tag);
    INFO("seed step: " << tag << " -> " << ran.err);
    REQUIRE(ran.code == 0);
    if (i == 1) {
      std::vector<std::string> const attach{"assoc", "add", "project:demo", (space.cpp_root / "proj").string()};
      REQUIRE(run_pinned(cpp_bin(), attach, space.cpp_root, std::format("{}attach", tag)).code == 0);
    }
  }
  REQUIRE(run_pinned(cpp_bin(), std::array<std::string, 3>{"workbench", "push", "1"}, space.cpp_root, "wbqpush").code == 0);
  // THE ANTI-VACUITY GUARD. A top-level, non-README spec must exist
  // before anything below is asserted.
  REQUIRE(std::filesystem::exists(space.cpp_root / "workbench" / "project_demo" / "p1-demo-feature" / "1-bullet-spec.md"));

  // A STUB EDITOR. It records its argv and then mutates the body so the
  // trailing `pull` has something real to apply.
  auto const script = space.cpp_root / "stub-editor";
  {
    std::ofstream file(script, std::ios::binary | std::ios::trunc);
    REQUIRE(file.good());
    file << "#!/bin/sh\n"
         << "printf '%s\\n' \"$1\" >> " << (space.cpp_root / "argv-witness").string() << "\n"
         << "f=\"$1/1-bullet-spec.md\"\n"
         // `sed -i ''` is BSD-only: GNU sed takes the '' as the script and the
         // script as a filename. The temp-file form is portable (task 6936).
         << "sed 's/^Intro line\\./XYZZY-PARITY-EDIT/' \"$f\" > \"$f.tmp\" && mv \"$f.tmp\" \"$f\"\n"
         << "exit 0\n";
    file.close();
    std::error_code ec;
    std::filesystem::permissions(script, std::filesystem::perms::owner_all, ec);
    REQUIRE(!ec);
  }

  struct step {
    std::string_view         tag;  ///< Case discriminator.
    std::vector<std::string> args; ///< The argv tail.
    int                      code; ///< Expected exit code.
    std::string_view         out;  ///< Expected stdout.
    std::string_view         err;  ///< Expected stderr.
  };
  std::vector<step> const steps{
      {"eqtext",
       {"workbench", "extract-questions", "1"},
       0,
       "1-bullet-spec.md (artifact 1): 3 question(s)\n"
       "  [line 12] Should we cache the result? — It would help a lot on repeated reads and we think it matter…\n"
       "  [line 13] What about eviction?\n"
       "  [line 14] nested bullet counts too\n"
       "2-h3-spec.md (artifact 2): 2 question(s)\n"
       "  [line 12] Which serializer? — JSON is the default.\n"
       "It has two lines.\n"
       "  [line 17] Do we version the payload? — Yes, probably.\n"
       "3-empty-spec.md (artifact 3): 0 question(s)\n",
       ""},
      {"eqjson",
       {"workbench", "extract-questions", "1", "--json"},
       0,
       "[{\"artifact_id\":1,\"file\":\"1-bullet-spec.md\",\"questions\":[{\"title\":\"Should we cache the "
       "result?\",\"body\":\"It would help a lot on repeated reads and we think it "
       "matters.\",\"source_line\":12},{\"title\":\"What about eviction?\",\"body\":\"\",\"source_line\":13},{\"title\":\"nested "
       "bullet counts "
       "too\",\"body\":\"\",\"source_line\":14}]},{\"artifact_id\":2,\"file\":\"2-h3-spec.md\",\"questions\":[{\"title\":\"Which "
       "serializer?\",\"body\":\"JSON is the default.\\nIt has two lines.\",\"source_line\":12},{\"title\":\"Do we version the "
       "payload?\",\"body\":\"Yes, "
       "probably.\",\"source_line\":17}]},{\"artifact_id\":3,\"file\":\"3-empty-spec.md\",\"questions\":[]}]\n",
       ""},
      // An unpushed plan HINTS at exit 0 rather than failing.
      {"eqnotree",
       {"workbench", "extract-questions", "2"},
       0,
       "workbench tree not found for plan 2; run 'workbench push 2' first\n",
       ""},
      {"eqmissing", {"workbench", "extract-questions", "999"}, 1, "", "error: plan not found: 999\n"},
      {"eqinvalid", {"workbench", "extract-questions", "0"}, 2, "", "error: invalid plan '0'\n"},
      {"edmissing", {"workbench", "edit", "999"}, 1, "", "error: plan not found: 999\n"},
      {"edinvalid", {"workbench", "edit", "0"}, 2, "", "error: invalid plan '0'\n"},
  };
  for (auto const& step : steps) {
    auto const mine = run_pinned(cpp_bin(), step.args, space.cpp_root, step.tag);
    INFO("step: " << step.tag);
    CHECK(mine.code == step.code);
    CHECK(mine.out == step.out);
    CHECK(mine.err == step.err);
  }

  // NON-VACUITY, asserted on the PAYLOAD directly rather than only via
  // the pinned bytes above.
  auto const text =
      run_pinned(cpp_bin(), std::vector<std::string>{"workbench", "extract-questions", "1"}, space.cpp_root, "eqnonvac");
  CHECK(text.out.contains("1-bullet-spec.md (artifact 1): 3 question(s)"));
  CHECK(text.out.contains("2-h3-spec.md (artifact 2): 2 question(s)"));
  CHECK(text.out.contains("3-empty-spec.md (artifact 3): 0 question(s)"));
  // The bullet branch's 60-byte elision and the H3 branch's joined body.
  CHECK(text.out.contains("Should we cache the result? — It would help a lot on repeated reads and we think it matter…"));
  CHECK(text.out.contains("Which serializer? — JSON is the default.\nIt has two lines."));
  // The subdirectory and the README were NOT scanned, and the section
  // terminator held.
  CHECK_FALSE(text.out.contains("README.md"));
  CHECK_FALSE(text.out.contains("which-format"));
  CHECK_FALSE(text.out.contains("not a question"));

  // `workbench edit` needs `PLANAR_EDITOR`, which the pinned environment
  // does not carry, so it is prefixed onto the argv by running the binary
  // THROUGH `env`.
  {
    std::vector<std::string> const args{std::format("PLANAR_EDITOR={}", script.string()), cpp_bin().string(), "workbench", "edit",
                                        "1"};
    auto const                     ran = run_pinned("/usr/bin/env", args, space.cpp_root, "wbedit");
    INFO("edit -> " << ran.err);
    CHECK(ran.code == 0);
  }
  auto const witness = read_all(space.cpp_root / "argv-witness");
  // THE SPAWN HAPPENED...
  REQUIRE_FALSE(witness.empty());
  // ...and it was handed the arena's FEATURE DIRECTORY — not a temp file,
  // which is what the drafting quartet's editor gets and what an
  // implementer carrying that witness over would have asserted.
  CHECK(witness == (space.cpp_root / "workbench" / "project_demo" / "p1-demo-feature").string() + "\n");
  // The trailing `pull` applied the stub's edit, so the round trip is
  // witnessed end to end rather than only at the spawn.
  auto const shown = run_pinned(cpp_bin(), std::vector<std::string>{"artifact", "show", "1"}, space.cpp_root, "wbeditshow");
  INFO("artifact show -> " << shown.err);
  CHECK(shown.out.contains("XYZZY-PARITY-EDIT"));
}

// --- `schema --command` and `schema --compact` (task 7204) ---------------
//
// Expectations are derived from the task's contract, not from the emitter:
// a single catalog object for one path, a two-key row per command, exit 2
// with an empty stdout for an unknown path, and a bare verb-shaped value
// (`task`) that is a lookup rather than a reordered subcommand.

namespace {

/// @brief Count non-overlapping occurrences of `needle` in `text`.
auto count_of(std::string_view text, std::string_view needle) -> std::size_t {
  std::size_t n = 0;
  for (auto at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) {
    ++n;
  }
  return n;
}

} // namespace

TEST_CASE("planar schema --command emits exactly one command's object", "[cmd][parity][schema][7204]") {
  auto const arena = make_arena("schema7204one");
  auto const one =
      run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command", "planar task update"}, arena.cpp_root, "one");
  CHECK(one.code == 0);
  CHECK(one.err.empty());
  CHECK(one.out.starts_with(R"({"name":")"));
  CHECK(one.out.ends_with("}\n"));
  CHECK(one.out.contains(R"("command":"planar task update")"));
  CHECK(count_of(one.out, R"("command":")") == 1);
  // The test spec asked for under 4 KB; the real object is 4,349 bytes
  // because it carries the inherited flags. The bound is 8 KB by a recorded
  // deviation on task 7204, not a silent loosening.
  CHECK(one.out.size() < 8192);

  // The relative spelling resolves to the same bytes.
  auto const rel = run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command=task update"}, arena.cpp_root, "rel");
  CHECK(rel.code == 0);
  CHECK(rel.out == one.out);

  // The object is the one the full catalog carries, byte for byte.
  auto const full = run_pinned(cpp_bin(), std::vector<std::string>{"schema"}, arena.cpp_root, "full");
  REQUIRE(full.code == 0);
  CHECK(full.out.contains(one.out.substr(0, one.out.size() - 1)));
}

TEST_CASE("planar schema --command with an unknown path exits 2, names it, prints nothing", "[cmd][parity][schema][7204]") {
  auto const arena = make_arena("schema7204bad");
  auto const bad   = run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command", "planar task update nonesuch"},
                                arena.cpp_root, "bad");
  CHECK(bad.code == 2);
  CHECK(bad.out.empty());
  CHECK(bad.err.contains("planar task update nonesuch"));
}

TEST_CASE("planar schema --compact is one two-key row per command", "[cmd][parity][schema][7204]") {
  auto const arena   = make_arena("schema7204compact");
  auto const full    = run_pinned(cpp_bin(), std::vector<std::string>{"schema"}, arena.cpp_root, "full");
  auto const compact = run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--compact"}, arena.cpp_root, "compact");
  REQUIRE(full.code == 0);
  REQUIRE(compact.code == 0);
  CHECK(compact.err.empty());
  auto const commands = count_of(full.out, R"("path":[)");
  CHECK(commands > 1);
  CHECK(count_of(compact.out, R"({"command":")") == commands);
  CHECK(count_of(compact.out, R"(,"summary":")") == commands);
  CHECK_FALSE(compact.out.contains(R"("flags")"));
  CHECK_FALSE(compact.out.contains(R"("name":)"));
  CHECK(compact.out.size() < 40 * 1024);
  CHECK(compact.out.size() < full.out.size());

  // Both flags: that one command's row, bare.
  auto const row =
      run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--compact", "--command", "task update"}, arena.cpp_root, "row");
  CHECK(row.code == 0);
  CHECK(row.out.starts_with(R"({"command":"planar task update","summary":")"));
  CHECK(count_of(row.out, R"("command":")") == 1);
  CHECK(compact.out.contains(row.out.substr(0, row.out.size() - 1)));
}

TEST_CASE("planar schema --command task is a lookup, not the task subcommand", "[cmd][parity][schema][7204]") {
  auto const arena = make_arena("schema7204edge");
  auto const bare  = run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command", "task"}, arena.cpp_root, "bare");
  auto const full = run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command", "planar task"}, arena.cpp_root, "full");
  CHECK(bare.code == 0);
  CHECK(bare.err.empty());
  CHECK(bare.out.starts_with(R"({"name":"task",)"));
  CHECK(bare.out.contains(R"("command":"planar task")"));
  CHECK(bare.out == full.out);
  // Same value placed before the verb, and in the `=` form.
  auto const before = run_pinned(cpp_bin(), std::vector<std::string>{"--command", "task", "schema"}, arena.cpp_root, "before");
  CHECK(before.code == 0);
  CHECK(before.out == bare.out);
  auto const eq = run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command=task"}, arena.cpp_root, "eq");
  CHECK(eq.out == bare.out);
}
