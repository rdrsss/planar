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

#include "parity_strict.hpp"

#include <sys/wait.h>

import std;
import cli11;
import planar.cliapp.schema;
import planar.cmd.planar.tree;
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

  // The half of this probe that needs the oracle is CONDITIONAL rather than
  // gating the whole case: the C++ assertions above stand on their own. But
  // silently dropping half a safety probe is exactly what task 6071 is
  // about, so strict mode still refuses.
  PLANAR_REQUIRE_ORACLE(oracle_available() || !::planar::parity::strict_mode(),
                        "zig reference binary not built — the oracle half of the environment probe cannot run");
  if (oracle_available()) {
    auto const ref = run_pinned(zig_bin(), std::array<std::string, 2>{"annotate", "list"}, space.zig_root, "envpin");
    REQUIRE(ref.code == 0);
    CHECK(std::filesystem::exists(space.zig_root / "planar.db"));
    CHECK_FALSE(std::filesystem::exists(space.zig_root / "fakehome" / ".planar" / "planar.db"));
  }
}

TEST_CASE("C++ and Zig agree byte-for-byte on the no-database leaves", "[cmd][parity][oracle]") {
  PLANAR_REQUIRE_ORACLE(
      oracle_available(),
      "zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");

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
  PLANAR_REQUIRE_ORACLE(
      oracle_available(),
      "zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");

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
  PLANAR_REQUIRE_ORACLE(
      oracle_available(),
      "zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");

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

// "C++ and Zig agree byte-for-byte on the three ported ext leaves" removed
// at plan 996, task 6419 (decision 999): `ext register`/`ext list` moved to
// `planar-ext`, which has no Zig oracle counterpart to diff against. See
// `src/cmd/planar-ext/ext_leaves.t.cpp` and `ext_create_leaf.t.cpp` for their
// C++-only coverage now.

TEST_CASE("C++ and Zig agree on unlink over a seeded external link", "[cmd][parity][oracle]") {
  PLANAR_REQUIRE_ORACLE(
      oracle_available(),
      "zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");

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

// "C++ and Zig agree on the three sync write leaves over seeded links"
// removed at plan 996, task 6419 (decisions 996, 999): `sync pull`/`push`/
// `resolve` moved to `planar-ext`, and decision 996 makes `sync pull` a
// deliberate, recorded divergence from the oracle (it no longer applies the
// remote to local planning tables) -- there is no longer a byte-identical
// oracle shape to pin here. See `src/cmd/planar-ext/sync_leaves.t.cpp`.

TEST_CASE("C++ and Zig agree on promote, demote and test-spec status", "[cmd][parity][oracle]") {
  PLANAR_REQUIRE_ORACLE(
      oracle_available(),
      "zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");

  // Both arenas are seeded by the ORACLE, as in the `sync` case above, so
  // the two databases start byte-identical and every row id below is the
  // same on both sides. Every seed verb here IS ported, so seeding each
  // arena with its own binary would also work — but it would make an id
  // drift in a seed verb read as a divergence in `promote`, which is the
  // opposite of what this case is for.
  //
  // TWO associations, and that is load-bearing: with one, `promote` can
  // only ever move a row in from global or report `scope_unchanged`, and
  // the association-to-association arm — the only one whose `--json`
  // envelope carries a non-null `previous_scope_id` — is unreachable.
  auto const                                  space = make_arena("promotion");
  std::vector<std::vector<std::string>> const seed{
      {"init", "--name", "Proj", "--slug", "proj", "--allow-no-repo", "--json"},
      {"assoc", "create", "project:proj", "--kind", "project", "--json"},
      {"assoc", "create", "org:acme", "--kind", "org", "--json"},
      {"plan", "create", "Anchor", "--slug", "anchor", "--scope", "project:proj", "--json"},
      {"plan", "create", "M1 Foundation", "--slug", "m1", "--parent", "1", "--scope", "project:proj", "--json"},
      {"task", "add", "T one", "--plan", "2", "--slug", "t-one", "--no-editor", "--json"},
  };
  for (std::size_t i = 0; i < seed.size(); ++i) {
    auto const tag = std::format("pseed{}", i);
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
  // ORDER IS LOAD-BEARING for the mutating steps: each `promote`/`demote`
  // observes the scope the previous one left behind, which is what makes
  // the `scope_unchanged` and already-global arms reachable at all.
  std::vector<step> const steps{
      // --- refusals, before any success ---
      {"pref1", {"promote", "plan", "--to", "org:acme"}},
      {"pref2", {"promote", "plan:", "--to", "org:acme"}},
      {"pref3", {"promote", ":1", "--to", "org:acme"}},
      {"pslug", {"promote", "plan:some-slug", "--to", "org:acme"}},
      // The two bad-kind arms, which land in different buckets: `bogus` is
      // not an `entity_kind` (exit 2, parse_ref), `session` is one but is
      // not promotable (exit 1, the pre-read).
      {"pbogus", {"promote", "bogus:1", "--to", "org:acme"}},
      {"psess", {"promote", "session:1", "--to", "org:acme"}},
      // Non-positive ids are MALFORMED, not absent — a different code and a
      // different message from `plan:999`.
      {"pzero", {"promote", "plan:0", "--to", "org:acme"}},
      {"pneg", {"promote", "plan:-1", "--to", "org:acme"}},
      {"pabsent", {"promote", "plan:999", "--to", "org:acme"}},
      {"prepo", {"promote", "plan:1", "--to", "repo:proj"}},
      {"punk", {"promote", "plan:1", "--to", "nonexistent"}},
      {"psame", {"promote", "plan:1", "--to", "project:proj"}},
      {"dref", {"demote", "plan"}},
      {"dslug", {"demote", "plan:some-slug"}},
      {"dabsent", {"demote", "plan:999"}},
      // --- successes, walking one row around the scope graph ---
      {"pmove", {"promote", "plan:1", "--to", "org:acme"}},
      {"pmovej", {"promote", "plan:1", "--to", "project:proj", "--json"}},
      {"ddown", {"demote", "plan:1", "--json"}},
      {"dagain", {"demote", "plan:1"}},
      // `--from` is read and DISCARDED: a slug that does not exist is not a
      // refusal, because the engine call takes no source scope.
      {"dfrom", {"demote", "plan:1", "--from", "nonexistent-slug"}},
      {"pback", {"promote", "plan:1", "--to", "org:acme"}},
      {"ptask", {"promote", "task:1", "--to", "org:acme", "--json"}},
      {"dtask", {"demote", "task:1", "--from", "org:acme"}},
      // --- test-spec status: a real MILESTONE reports "not found" ---
      {"tsmid", {"test-spec", "status", "2"}},
      {"tsmslug", {"test-spec", "status", "m1"}},
      {"tsabsent", {"test-spec", "status", "999"}},
      {"tsnope", {"test-spec", "status", "nope"}},
      {"tsid", {"test-spec", "status", "1"}},
      {"tsidj", {"test-spec", "status", "1", "--json"}},
      {"tsslug", {"test-spec", "status", "anchor"}},
      {"tsslugj", {"test-spec", "status", "anchor", "--json"}},
  };
  for (auto const& [tag, args] : steps) {
    auto const mine = run_pinned(cpp_bin(), args, space.cpp_root, tag);
    auto const ref  = run_pinned(zig_bin(), args, space.zig_root, tag);
    INFO("step: " << tag);
    CHECK(mine.code == ref.code);
    CHECK(mine.out == ref.out);
    CHECK(mine.err == ref.err);
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
  auto const mine_scopes = scopes(space.cpp_root);
  CHECK(mine_scopes == scopes(space.zig_root));
  // Non-empty, so the diff above cannot be two empty strings agreeing — and
  // the plan really did end up back in an association rather than global,
  // which is the state the last mutating step left.
  CHECK_FALSE(mine_scopes.empty());
  CHECK(mine_scopes.contains("plan:1|association|2\n"));
  CHECK(mine_scopes.contains("task:1|global|-1\n"));
}

TEST_CASE("C++ and Zig agree on workspace doctor's diagnose-and-repair pass", "[cmd][parity][oracle]") {
  PLANAR_REQUIRE_ORACLE(
      oracle_available(),
      "zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");

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
  PLANAR_REQUIRE_ORACLE(
      oracle_available(),
      "zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");

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
  auto theirs = planar::cmd::parity::parse_catalog(ref.out);
  REQUIRE(theirs.has_value());

  // Plan 996, task 6419: the oracle still declares the `ext`/`sync` family
  // (14 catalog entries — 2 group nodes `ext`/`sync`, the `ext register`
  // sub-group, and 11 leaves including the still-unimplemented `ext
  // propagate`), which moved to `planar-ext` and has no oracle counterpart
  // (decision 999). Removed from the ORACLE side, root's own subcommand
  // list included, before either comparison below runs.
  for (auto const& moved :
       {"planar ext", "planar ext create", "planar ext list", "planar ext propagate", "planar ext propagate-one",
        "planar ext register", "planar ext register github", "planar ext register jira", "planar ext test", "planar sync",
        "planar sync pull", "planar sync push", "planar sync resolve", "planar sync status"}) {
    theirs->erase(moved);
  }
  auto& oracle_root_subcommands = theirs->at("planar").subcommands;
  std::erase(oracle_root_subcommands, "ext");
  std::erase(oracle_root_subcommands, "sync");

  // Non-vacuous: an empty left-hand side would pass trivially, and the
  // named entries below are the ones whose declarations this task most
  // easily could have got wrong.
  //
  // 261 -> 247 on BOTH sides at plan 996, task 6419: `mine` never declared
  // the 14 `ext`/`sync` entries in the first place (they moved to
  // `planar-ext`), and `theirs` had them stripped just above so the
  // comparison below is exact-set equal again, not merely non-empty.
  CHECK(mine->size() == 247);
  CHECK(theirs->size() == 247);
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
  // The 10 moved LEAVES (not counting the 4 group/deferred entries above)
  // are GENUINELY gone from `mine` — asserted explicitly rather than left
  // to the strip loop above alone, so a typo there reads as a wrong
  // exact-set assertion rather than a silently-passing missing check.
  for (auto const& moved :
       {"planar ext register jira", "planar ext register github", "planar ext list", "planar ext test", "planar ext create",
        "planar ext propagate-one", "planar sync pull", "planar sync push", "planar sync status", "planar sync resolve"}) {
    INFO("moved to planar-ext at task 6419: " << moved);
    CHECK_FALSE(mine->contains(moved));
  }

  auto const problems = planar::cmd::parity::diff_against_oracle(*mine, *theirs);
  INFO("declaration mismatches:\n" << std::format("{}", problems));
  CHECK(problems.empty());

  // Empty again now that `theirs` no longer carries the 14 moved/deferred
  // entries: any NEW divergence here is a real regression, not an
  // expected one.
  auto const missing = planar::cmd::parity::oracle_only_commands(*mine, *theirs);
  INFO("declared by the oracle and NOT by this binary:\n" << std::format("{}", missing));
  CHECK(missing.empty());
}

TEST_CASE("all three catalogs are byte-identical to the oracle's", "[cmd][parity][oracle][catalog]") {
  PLANAR_REQUIRE_ORACLE(
      oracle_available(),
      "zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");
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

  // Plan 996, task 6419: the oracle's document still declares the whole
  // `ext`/`sync` family — the two group nodes, the `ext register`
  // sub-group, the still-unimplemented `ext propagate`, and the 10 moved
  // leaves — which moved to `planar-ext` (decision 999 — this binary owes
  // them no parity). Excised from the ORACLE side before the byte
  // comparison, one command object at a time, so a stray divergence
  // beyond this exact set still fails loudly instead of being swallowed
  // by a looser check. Root's OWN `"subcommands"` array also names `ext`
  // and `sync`, which `strip_command` does not touch (it only removes
  // each leaf's own object) — pulled out separately by the same two
  // exact, order-preserving neighbour anchors `parse_catalog`'s own
  // subcommand-order pin (`"the CLI surface is CLI11's now, and pinned"`,
  // below) confirms are stable.
  auto stripped = ref.out;
  for (auto const& path : std::initializer_list<std::initializer_list<std::string_view>>{{"ext", "register", "jira"},
                                                                                         {"ext", "register", "github"},
                                                                                         {"ext", "register"},
                                                                                         {"ext", "list"},
                                                                                         {"ext", "test"},
                                                                                         {"ext", "create"},
                                                                                         {"ext", "propagate-one"},
                                                                                         {"ext", "propagate"},
                                                                                         {"ext"},
                                                                                         {"sync", "pull"},
                                                                                         {"sync", "push"},
                                                                                         {"sync", "status"},
                                                                                         {"sync", "resolve"},
                                                                                         {"sync"}}) {
    stripped = strip_command(std::move(stripped), path);
  }
  {
    auto const before_ext = stripped.find(R"("workspace","ext","link")");
    REQUIRE(before_ext != std::string::npos);
    stripped.replace(before_ext, std::string_view{R"("workspace","ext","link")"}.size(), R"("workspace","link")");
    auto const before_sync = stripped.find(R"("links","sync","resume")");
    REQUIRE(before_sync != std::string::npos);
    stripped.replace(before_sync, std::string_view{R"("links","sync","resume")"}.size(), R"("links","resume")");
  }
  CHECK(stripped == actual.out);
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
  PLANAR_REQUIRE_ORACLE(
      oracle_available(),
      "zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");

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
    // `$PLANAR_WORKBENCH_ROOT` (task 6305), not the `~/.planar/workbench`
    // fallback this used to read through `HOME`.
    auto const      wb = root / "workbench";
    std::error_code ec;
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
  PLANAR_REQUIRE_ORACLE(
      oracle_available(),
      "zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");

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
  PLANAR_REQUIRE_ORACLE(
      oracle_available(),
      "zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");

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

// --- the parity strictness switch (task 6071) ----------------------------

TEST_CASE("PLANAR_PARITY_STRICT turns an absent oracle from a skip into a failure", "[cmd][parity][strictness]") {
  // The switch itself, tested rather than assumed. Its whole purpose is to
  // stop a run of twenty-six oracle-gated cases from reporting as a clean
  // pass when the oracle was never consulted -- and at the M10 cutover,
  // when zig/ is deleted, that is the permanent state of every one of them.
  //
  // Only `strict_mode()` is exercised directly. The macro around it cannot
  // be: it expands to Catch2's SKIP or FAIL, both of which act on the
  // RUNNING case, so a test that called it would skip or fail ITSELF rather
  // than report what it did. The macro is two lines of dispatch over this
  // predicate; the predicate is where a mistake would hide.
  //
  // Mutating the environment is safe because catch_discover_tests runs each
  // TEST_CASE as its own process, and it is restored regardless.
  char const* const original = std::getenv("PLANAR_PARITY_STRICT");
  std::string const saved    = original == nullptr ? std::string{} : std::string{original};
  bool const        was_set  = original != nullptr;

  ::unsetenv("PLANAR_PARITY_STRICT");
  bool const when_unset = ::planar::parity::strict_mode();

  REQUIRE(::setenv("PLANAR_PARITY_STRICT", "1", 1) == 0);
  bool const when_one = ::planar::parity::strict_mode();

  // `0` and the empty string are explicitly OFF, so a CI job that exports
  // the variable unconditionally can turn it off BY VALUE rather than
  // having to unset it.
  REQUIRE(::setenv("PLANAR_PARITY_STRICT", "0", 1) == 0);
  bool const when_zero = ::planar::parity::strict_mode();
  REQUIRE(::setenv("PLANAR_PARITY_STRICT", "", 1) == 0);
  bool const when_empty = ::planar::parity::strict_mode();

  // Anything else truthy counts, so `PLANAR_PARITY_STRICT=yes` is not a
  // silent no-op.
  REQUIRE(::setenv("PLANAR_PARITY_STRICT", "yes", 1) == 0);
  bool const when_word = ::planar::parity::strict_mode();

  if (was_set) {
    ::setenv("PLANAR_PARITY_STRICT", saved.c_str(), 1);
  } else {
    ::unsetenv("PLANAR_PARITY_STRICT");
  }

  CHECK_FALSE(when_unset);
  CHECK(when_one);
  CHECK_FALSE(when_zero);
  CHECK_FALSE(when_empty);
  CHECK(when_word);
}

TEST_CASE("C++ and Zig agree over a seeded local sandbox lifecycle", "[cmd][parity][oracle][local]") {
  PLANAR_REQUIRE_ORACLE(
      oracle_available(),
      "zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");

  // SEED EACH ARENA INDEPENDENTLY, never by copying one. The manifest records
  // ABSOLUTE `source_path` / `target_path` values, so a copied arena would
  // carry the other side's paths and the "divergence" would be the fixture.
  // (The same mistake the workbench case above documents.)
  auto const space = make_arena("localtree");

  struct source {
    std::string_view rel;  ///< Path under `<localhome>/.planar/local`.
    std::string_view body; ///< File contents.
  };
  // ONE malformed entry only. `manifest::walk_sandbox` appends walk errors in
  // DIRECTORY-ITERATION order, which is unspecified on both sides, so the SET
  // of warning lines is the contract and their ORDER is not. A single entry
  // makes the two agree without weakening the comparison.
  std::vector<source> const sources{
      {"skills/three/SKILL.md", "---\nname: three\ndescription: Three vendors.\nvendors: [claude, codex, copilot]\n---\n\nB.\n"},
      {"skills/nodesc/SKILL.md", "---\nname: nodesc\n---\n\nNo description, so this LINTS.\n"},
      {"skills/flat.md", "---\nname: flat\ndescription: Legacy flat shape.\n---\n\nFlat.\n"},
      {"agents/an-agent.md", "---\nname: an-agent\ndescription: An agent.\n---\n\nA.\n"},
  };
  for (auto const& root : {space.cpp_root, space.zig_root}) {
    std::error_code ec;
    auto const      sandbox = root / "localhome" / ".planar" / "local";
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
  };
  // An ORDERED sequence against ONE sandbox per binary: `list` only means
  // anything after `link`, `migrate` only after the flat source has been
  // seen, and `--reconcile` only after something is stale.
  std::vector<step> const steps{
      {"lclist0", {"local", "list"}},
      {"lclistj0", {"local", "list", "--json"}},
      {"lclinkdry", {"local", "link", "--dry-run"}},
      {"lclink", {"local", "link"}},
      {"lclink2", {"local", "link"}},
      {"lclinkj", {"local", "link", "--json"}},
      {"lclist1", {"local", "list"}},
      {"lclistj1", {"local", "list", "--json"}},
      {"lclistv", {"local", "list", "--vendor", "codex"}},
      {"lclistvn", {"local", "list", "--vendor", "nope"}},
      {"lclistve", {"local", "list", "--vendor", ""}},
      {"lclinkone", {"local", "link", "three", "--vendor", "claude"}},
      {"lclinkmiss", {"local", "link", "nosuch"}},
      {"lcrecpos", {"local", "link", "--reconcile", "stray"}},
      {"lcmigdry", {"local", "migrate", "--dry-run"}},
      {"lcmig", {"local", "migrate"}},
      {"lcmig2", {"local", "migrate"}},
      {"lcmigj", {"local", "migrate", "--json"}},
      {"lcunlink", {"local", "unlink", "an-agent"}},
      {"lcunlink2", {"local", "unlink", "an-agent"}},
      {"lcunlinkj", {"local", "unlink", "nodesc", "--json"}},
      {"lcunlinkghost", {"local", "unlink", "ghost", "--purge"}},
      {"lcrecdry", {"local", "link", "--reconcile", "--dry-run"}},
      {"lcrec", {"local", "link", "--reconcile"}},
      {"lclist2", {"local", "list"}},
      {"lcimpbad", {"local", "import", "nope.md", "--kind", "bogus"}},
      {"lcimpmiss", {"local", "import", "nope.md"}},
  };

  // `linked_at` is a wall-clock second stamp that reaches the `--json`
  // payloads, so it cannot agree across two processes. Folding it to a token
  // is what keeps those steps comparable without dropping them; everything
  // else, including the arena root, is compared for real.
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
    // A fold keyed on the compact spelling matches stdout and silently misses
    // the manifest, which is exactly the bug that made this case fail about
    // one run in three — the two binaries are invoked ~a second apart, so the
    // unfolded manifest stamps disagree whenever that gap crosses a second
    // boundary. Only the VALUE is tokenised, so a side that changed its
    // formatting still diverges loudly.
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

  for (auto const& [tag, args] : steps) {
    auto const mine = run_pinned(cpp_bin(), args, space.cpp_root, tag);
    auto const ref  = run_pinned(zig_bin(), args, space.zig_root, tag);
    INFO("step: " << tag);
    CHECK(mine.code == ref.code);
    CHECK(normalize(mine.out, space.cpp_root) == normalize(ref.out, space.zig_root));
    CHECK(normalize(mine.err, space.cpp_root) == normalize(ref.err, space.zig_root));
  }

  // THE PERSISTED STATE IS THE REAL PRODUCT, and no summary line above would
  // notice a divergence inside it. Both manifests are compared byte for byte
  // (modulo the arena root and the stamp), and so is the SHAPE of the vendor
  // trees the links landed in — a port that recorded the right manifest and
  // created the wrong symlink would pass every stdout comparison.
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
    auto const ref  = manifests_of(space.zig_root);
    // NON-VACUITY OF THE FOLD. A stamp fold that stops matching is invisible:
    // it does not fail, it just leaves two wall-clock stamps in place and
    // reds the case whenever the two runs straddle a second. That is the
    // exact defect this guard exists to catch — the skills manifest ends the
    // sequence with `three` linked to claude, so it MUST carry a folded
    // stamp and MUST NOT carry a raw one.
    REQUIRE(mine.at("skills").contains("<STAMP>"));
    REQUIRE(ref.at("skills").contains("<STAMP>"));
    CHECK_FALSE(mine.at("skills").contains("\"linked_at\": \"2"));
    CHECK_FALSE(ref.at("skills").contains("\"linked_at\": \"2"));
    for (auto const& kind : {"skills", "agents"}) {
      // Compared per KIND, not as one `std::map` — Catch2 cannot stringify
      // the map (it prints `{ {?}, {?} }`) and the divergence was invisible.
      INFO("manifest kind: " << kind);
      CHECK(mine.at(kind) == ref.at(kind));
    }
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
  // NON-VACUITY: the comparison below is worthless if both sides installed
  // nothing. The sequence ends with `three` still linked to claude.
  CHECK_FALSE(cpp_installs.empty());
  CHECK(cpp_installs == installs_of(space.zig_root));
}

TEST_CASE("C++ and Zig agree on closure show and groups recommend over seeded rows", "[cmd][parity][oracle][closure]") {
  PLANAR_REQUIRE_ORACLE(
      oracle_available(),
      "zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");

  auto const                                  space = make_arena("clogrp");
  std::vector<std::vector<std::string>> const seed{
      {"init", "--name", "demo", "--slug", "demo", "--allow-no-repo"},
      {"assoc", "create", "project:demo", "--kind", "project"},
      {"plan", "create", "Demo", "--slug", "demo-plan", "--summary", "S"},
      {"task", "add", "T1", "--plan", "1", "--editor=false"},
      {"task", "add", "T2", "--plan", "1", "--editor=false"},
      {"task", "add", "T3", "--plan", "1", "--editor=false"},
      {"task", "add", "T4", "--plan", "1", "--editor=false"},
  };
  for (auto const& root : {space.cpp_root, space.zig_root}) {
    for (std::size_t i = 0; i < seed.size(); ++i) {
      auto const tag = std::format("cgseed{}_{}", root == space.cpp_root ? "c" : "z", i);
      auto const ran = run_pinned(zig_bin(), seed[i], root, tag);
      INFO("seed step: " << tag << " -> " << ran.err);
      REQUIRE(ran.code == 0);
      if (i == 1) {
        std::vector<std::string> const attach{"assoc", "add", "project:demo", (root / "proj").string()};
        REQUIRE(run_pinned(zig_bin(), attach, root, std::format("{}attach", tag)).code == 0);
      }
    }
  }

  // `closures` has no ported writer on EITHER side — `closure compute` is the
  // unported half of this family and needs tree-sitter — so the rows are
  // seeded through `sqlite3` directly, identically into both arenas. Every
  // value is pinned, `created_at` included, so the two databases are
  // byte-comparable in the columns these leaves render.
  //
  // The fixture is built to discriminate: two `modify` rows whose PATH order
  // and SYMBOL order DISAGREE, one `transitive` row heavy enough (9000) that
  // a slice including it could not report the asserted cost, a `done` task
  // (weight 5000) that must not be grouped, and a dependency edge.
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
  for (auto const& root : {space.cpp_root, space.zig_root}) {
    auto const sql_path = root / "seed.sql";
    {
      std::ofstream file(sql_path, std::ios::binary | std::ios::trunc);
      REQUIRE(file.is_open());
      file << k_seed_sql;
    }
    auto const line = std::format("sqlite3 {} < {}", shell_quote((root / "planar.db").string()), shell_quote(sql_path.string()));
    REQUIRE(std::system(line.c_str()) == 0);
  }

  struct step {
    std::string_view         tag;  ///< Case discriminator.
    std::vector<std::string> args; ///< The argv tail.
  };
  std::vector<step> const steps{
      {"cs1", {"closure", "show", "1"}},
      {"cs1j", {"closure", "show", "1", "--json"}},
      {"cs4", {"closure", "show", "4"}},
      {"cs999", {"closure", "show", "999"}},
      {"cs999j", {"closure", "show", "999", "--json"}},
      {"csbad", {"closure", "show", "notanint"}},
      {"cssep", {"closure", "show", "1_0"}},
      {"cspad", {"closure", "show", "007"}},
      {"csplus", {"closure", "show", "+12"}},
      {"csovf", {"closure", "show", "9223372036854775808"}},
      {"csmax", {"closure", "show", "9223372036854775807"}},
      {"gr1", {"groups", "recommend", "1"}},
      {"gr1j", {"groups", "recommend", "1", "--json"}},
      {"grb", {"groups", "recommend", "1", "--budget", "25", "--json"}},
      {"grb0", {"groups", "recommend", "1", "--budget", "0", "--json"}},
      {"grbmax", {"groups", "recommend", "1", "--budget", "4294967295", "--json"}},
      {"grbovf", {"groups", "recommend", "1", "--budget", "4294967296"}},
      {"grbneg", {"groups", "recommend", "1", "--budget", "-1"}},
      {"grbbad", {"groups", "recommend", "1", "--budget", "xyz"}},
      {"grmiss", {"groups", "recommend", "999"}},
      {"grbadid", {"groups", "recommend", "abc"}},
      {"grsolverbad", {"groups", "recommend", "1", "--solver", "bogus"}},
      {"grsolvergreedy", {"groups", "recommend", "1", "--solver", "greedy", "--json"}},
      // `--solver mtkahypar` is DELIBERATELY ABSENT. The optional solver is
      // not ported here, so this build always degrades to greedy — which is
      // byte-identical to the oracle ON A HOST WITHOUT THE SOLVER INSTALLED
      // and genuinely different on a host with it. Pinning either outcome
      // would make the case pass or fail on what happens to be on the
      // machine. The degradation itself IS pinned, against this binary
      // alone, in closure_groups_leaves.t.cpp.
  };

  for (auto const& [tag, args] : steps) {
    auto const mine = run_pinned(cpp_bin(), args, space.cpp_root, tag);
    auto const ref  = run_pinned(zig_bin(), args, space.zig_root, tag);
    INFO("step: " << tag);
    CHECK(mine.code == ref.code);
    CHECK(mine.out == ref.out);
    CHECK(mine.err == ref.err);
  }

  // NON-VACUITY. Both leaves are read-only, so a seed that silently failed
  // would make every comparison above a trivially-equal pair of empty
  // answers. This asserts the fixture actually reached the binaries.
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

TEST_CASE("C++ and Zig agree on extract-questions and on the workbench edit round trip", "[cmd][parity][oracle][workbench]") {
  PLANAR_REQUIRE_ORACLE(
      oracle_available(),
      "zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");

  // THE FIXTURE IS BUILT BY A REAL `workbench push`, NOT BY HAND. This leaf
  // reads only TOP-LEVEL `.md` files and skips `README.md`, so the obvious
  // feature tree — a README plus `questions/` and `tasks/` subdirectories —
  // makes it return `[]` on BOTH binaries, and every comparison below would
  // pass while comparing two empty lists. Hand-authoring a `.md` does not
  // escape it either: the file must satisfy `workbench::parse::parse`, and
  // a malformed one is silently skipped, so the result is empty again for a
  // different reason. The `REQUIRE`s after the seed are what make that
  // failure loud instead of green.
  //
  // Seeded independently per arena, never copied: `projects.root_path` is
  // absolute (see the `wbtree` case above for what copying cost).
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

  std::vector<std::vector<std::string>> const seed{
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
  };
  for (auto const& root : {space.cpp_root, space.zig_root}) {
    for (std::size_t i = 0; i < seed.size(); ++i) {
      auto const tag = std::format("wbqseed{}_{}", root == space.cpp_root ? "c" : "z", i);
      auto const ran = run_pinned(zig_bin(), seed[i], root, tag);
      INFO("seed step: " << tag << " -> " << ran.err);
      REQUIRE(ran.code == 0);
      if (i == 1) {
        std::vector<std::string> const attach{"assoc", "add", "project:demo", (root / "proj").string()};
        REQUIRE(run_pinned(zig_bin(), attach, root, std::format("{}attach", tag)).code == 0);
      }
    }
    REQUIRE(run_pinned(zig_bin(), std::array<std::string, 3>{"workbench", "push", "1"}, root, "wbqpush").code == 0);
    // THE ANTI-VACUITY GUARD. A top-level, non-README spec must exist in
    // each arena before anything is compared.
    REQUIRE(std::filesystem::exists(root / "workbench" / "project_demo" / "p1-demo-feature" / "1-bullet-spec.md"));
  }

  // A STUB EDITOR per arena. It records its argv and then mutates the body
  // so the trailing `pull` has something real to apply.
  for (auto const& root : {space.cpp_root, space.zig_root}) {
    auto const    script = root / "stub-editor";
    std::ofstream file(script, std::ios::binary | std::ios::trunc);
    REQUIRE(file.good());
    file << "#!/bin/sh\n"
         << "printf '%s\\n' \"$1\" >> " << (root / "argv-witness").string() << "\n"
         << "sed -i '' 's/^Intro line\\./XYZZY-PARITY-EDIT/' \"$1/1-bullet-spec.md\"\n"
         << "exit 0\n";
    file.close();
    std::error_code ec;
    std::filesystem::permissions(script, std::filesystem::perms::owner_all, ec);
    REQUIRE(!ec);
  }

  struct step {
    std::string_view         tag;  ///< Case discriminator.
    std::vector<std::string> args; ///< The argv tail.
  };
  std::vector<step> const steps{
      {"eqtext", {"workbench", "extract-questions", "1"}},
      {"eqjson", {"workbench", "extract-questions", "1", "--json"}},
      // An unpushed plan HINTS at exit 0 rather than failing.
      {"eqnotree", {"workbench", "extract-questions", "2"}},
      {"eqmissing", {"workbench", "extract-questions", "999"}},
      {"eqinvalid", {"workbench", "extract-questions", "0"}},
      {"edmissing", {"workbench", "edit", "999"}},
      {"edinvalid", {"workbench", "edit", "0"}},
  };
  for (auto const& [tag, args] : steps) {
    auto const mine = run_pinned(cpp_bin(), args, space.cpp_root, tag);
    auto const ref  = run_pinned(zig_bin(), args, space.zig_root, tag);
    INFO("step: " << tag);
    CHECK(mine.code == ref.code);
    CHECK(mine.out == ref.out);
    CHECK(mine.err == ref.err);
  }

  // NON-VACUITY, asserted on the PAYLOAD rather than on the agreement: two
  // identical empty captures satisfy every CHECK above.
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
  for (auto const& root : {space.cpp_root, space.zig_root}) {
    auto const                     bin = root == space.cpp_root ? cpp_bin() : zig_bin();
    std::vector<std::string> const args{std::format("PLANAR_EDITOR={}", (root / "stub-editor").string()), bin.string(),
                                        "workbench", "edit", "1"};
    auto const                     ran = run_pinned("/usr/bin/env", args, root, "wbedit");
    INFO("edit -> " << ran.err);
    CHECK(ran.code == 0);
  }
  auto const mine_edit = read_all(space.cpp_root / "argv-witness");
  auto const ref_edit  = read_all(space.zig_root / "argv-witness");
  // THE SPAWN HAPPENED on both sides...
  REQUIRE_FALSE(mine_edit.empty());
  REQUIRE_FALSE(ref_edit.empty());
  // ...and each was handed its OWN arena's FEATURE DIRECTORY — not a temp
  // file, which is what the drafting quartet's editor gets and what an
  // implementer carrying that witness over would have asserted.
  CHECK(mine_edit == (space.cpp_root / "workbench" / "project_demo" / "p1-demo-feature").string() + "\n");
  CHECK(ref_edit == (space.zig_root / "workbench" / "project_demo" / "p1-demo-feature").string() + "\n");
  // The trailing `pull` applied the stub's edit on both sides, so the
  // round trip is witnessed end to end rather than only at the spawn.
  for (auto const& root : {space.cpp_root, space.zig_root}) {
    auto const shown = run_pinned(root == space.cpp_root ? cpp_bin() : zig_bin(),
                                  std::vector<std::string>{"artifact", "show", "1"}, root, "wbeditshow");
    INFO("artifact show -> " << shown.err);
    CHECK(shown.out.contains("XYZZY-PARITY-EDIT"));
  }
}
