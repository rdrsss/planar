// @file promotion_test_spec_leaves.t.cpp
// @brief Handler-level coverage for the three leaves task 6299 ported:
// `planar promote`, `planar demote` and `planar test-spec status`.
//
// The engines' own byte-for-byte cases live in
// `src/lib/engine/promotion/promotion.t.cpp` and
// `src/lib/engine/planning/test_spec_status.t.cpp` — both buckets were
// ported complete, renderers included, before any handler existed to call
// them. What this file pins is the half those cannot see: ref DECODING,
// the pre-read that runs before the engine, and the exit-code mapping.
//
// ## Two bad-kind refusals that land in DIFFERENT buckets
//
// Captured in a pinned arena against `zig/zig-out/bin/planar`, not inferred
// from either engine:
//
// ```
// $ planar promote bogus:1   --to org:acme  -> exit 2, `invalid ref 'bogus:1': expected kind:id`
// $ planar promote session:1 --to org:acme  -> exit 1, `reading entity scope: InvalidScope`
// ```
//
// Both are "that kind cannot be promoted", and they do not share an exit
// code or a message. `session` IS a member of `entity_kind`, so it survives
// `parse_ref` and is refused one layer later by the pre-read; `bogus` is
// not, so `parse_ref` refuses it first. A port that validated promotability
// in one place would have collapsed them.
//
// ## `plan:0` and `plan:-1` are MALFORMED, not absent
//
// `parse_ref` accepts only positive integers, so a non-positive id never
// reaches the database: exit 2 with the invalid-ref wording, not the exit 1
// `reading entity scope: NotFound` an absent-but-well-formed id gets.
//
// ## `demote --from` is accepted and DISCARDED
//
// `demote plan:1 --from nonexistent-slug` does not refuse on the slug. The
// engine call takes no source scope. Pinned so a later "obvious fix" that
// starts validating it fails a test instead of breaking callers.
//
// ## `test-spec status` on a real MILESTONE reports "not found"
//
// Both the numeric and the slug lookup add `parent_plan_id is null`. Plan 2
// / slug `m1` exist and are perfectly valid, and both report
// `plan '2' not found` / `plan 'm1' not found` at exit 1. Reproduced
// without improvement.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;

using planar::cmd::context;

namespace {

// @brief One handler invocation's observable result.
struct invocation {
  int         code = 0; // The exit code.
  std::string out;      // Everything written to stdout.
  std::string err;      // Everything written to stderr.
};

// @brief A scratch root plus the environment and database path.
struct fixture {
  std::filesystem::path                           root;
  std::map<std::string, std::string, std::less<>> vars;
  std::filesystem::path                           db_path;
};

auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_promoleaf_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "home", ec);
  std::filesystem::create_directories(root / "proj", ec);
  std::filesystem::create_directories(root / "fakehome", ec);
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_HOME", (root / "home").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()}},
      .db_path = root / "planar.db",
  };
}

auto dispatch(const fixture& fx, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv), planar::cmd::map_env(fx.vars), fx.root / "proj", std::make_shared<planar::cmd::database>(fx.db_path, err), out, err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::make_handler_table(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

// @brief Seed two associations, an anchor plan, a milestone under it, and a
// task on the milestone. Through the CLI, so the scope state is established
// by the same code paths an operator would take.
//
// TWO associations is load-bearing: with only one, every `promote` either
// succeeds into it or reports `scope_unchanged`, and the association-to-
// association move — the case whose `--json` envelope carries a non-null
// `previous_scope_id` — is unreachable.
auto seed(const fixture& fx) -> void {
  REQUIRE(dispatch(fx, {"init", "--name", "Proj", "--slug", "proj", "--allow-no-repo", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "project:proj", "--kind", "project", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "org:acme", "--kind", "org", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "project:proj", (fx.root / "proj").string(), "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Anchor", "--slug", "anchor", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "M1 Foundation", "--slug", "m1", "--parent", "1", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "T one", "--plan", "2", "--slug", "t-one", "--no-editor", "--json"}).code == 0);
}

} // namespace

// The anti-vacuity gate for every case below. A fixture that silently
// matched nothing — a plan that was never created, an association whose
// row a CHECK constraint rejected — would let every refusal case below pass
// for the wrong reason, because "refused" and "there was nothing there"
// look identical. This case proves the rows exist and carry the scope the
// later cases move them out of, BEFORE anything else runs.
TEST_CASE("promotion fixture: the seeded plan really is association-scoped", "[cmd][promote][fixture]") {
  auto const fx = make_fixture("shape");
  seed(fx);

  auto const plans = dispatch(fx, {"plan", "list", "--json"});
  REQUIRE(plans.code == 0);
  CHECK(plans.out.find(R"("id":1,"scope_kind":"association","scope_id":1,"title":"Anchor")") != std::string::npos);
  CHECK(plans.out.find(R"("slug":"m1")") != std::string::npos);
  CHECK(plans.out.find(R"("parent_plan_id":1)") != std::string::npos);

  // BOTH associations exist. Without the second one the promote-between-
  // associations case below cannot be reached at all.
  auto const assocs = dispatch(fx, {"assoc", "list", "--json"});
  REQUIRE(assocs.code == 0);
  CHECK(assocs.out.find(R"("slug":"project:proj")") != std::string::npos);
  CHECK(assocs.out.find(R"("slug":"org:acme")") != std::string::npos);
}

TEST_CASE("promote: a malformed ref refuses at exit 2 and quotes the ref", "[cmd][promote]") {
  auto const fx = make_fixture("badref");
  seed(fx);

  for (auto const& ref : {"plan", "plan:", ":1"}) {
    INFO("malformed ref: " << ref);
    auto const got = dispatch(fx, {"promote", ref, "--to", "org:acme"});
    CHECK(got.code == 2);
    CHECK(got.out.empty());
    CHECK(got.err == std::format("error: invalid ref '{}': expected kind:id\n", ref));
  }

  // Non-vacuous: a WELL-FORMED ref on the same fixture succeeds, so the
  // refusals above are about the ref and not about the fixture.
  auto const ok = dispatch(fx, {"promote", "plan:1", "--to", "org:acme"});
  CHECK(ok.code == 0);
  CHECK(ok.out == "plan:1 promoted to association org:acme  (was: association:1)\n");
}

TEST_CASE("promote: a non-positive id is MALFORMED, not absent", "[cmd][promote]") {
  auto const fx = make_fixture("nonpos");
  seed(fx);

  for (auto const& ref : {"plan:0", "plan:-1"}) {
    INFO("non-positive id: " << ref);
    auto const got = dispatch(fx, {"promote", ref, "--to", "org:acme"});
    CHECK(got.code == 2);
    CHECK(got.err == std::format("error: invalid ref '{}': expected kind:id\n", ref));
  }

  // The contrast that makes the above meaningful: a well-formed but ABSENT
  // id reaches the pre-read and gets a different code AND a different
  // message.
  auto const absent = dispatch(fx, {"promote", "plan:999", "--to", "org:acme"});
  CHECK(absent.code == 1);
  CHECK(absent.err == "error: reading entity scope: NotFound\n");
}

TEST_CASE("promote: the two bad-kind refusals do NOT share a bucket", "[cmd][promote]") {
  auto const fx = make_fixture("badkind");
  seed(fx);

  // Not an `entity_kind` at all -> refused by `parse_ref`, exit 2.
  auto const unknown = dispatch(fx, {"promote", "bogus:1", "--to", "org:acme"});
  CHECK(unknown.code == 2);
  CHECK(unknown.err == "error: invalid ref 'bogus:1': expected kind:id\n");

  // A REAL `entity_kind` that is not promotable -> survives `parse_ref`,
  // refused by the pre-read, exit 1, carrying the Zig `@errorName` tag.
  auto const unpromotable = dispatch(fx, {"promote", "session:1", "--to", "org:acme"});
  CHECK(unpromotable.code == 1);
  CHECK(unpromotable.err == "error: reading entity scope: InvalidScope\n");
}

TEST_CASE("promote: a slug ref names the VERB in its refusal", "[cmd][promote]") {
  auto const fx = make_fixture("slugref");
  seed(fx);

  auto const promoting = dispatch(fx, {"promote", "plan:some-slug", "--to", "org:acme"});
  CHECK(promoting.code == 2);
  CHECK(promoting.err == "error: promote requires a numeric id (got slug 'plan:some-slug')\n");

  // The same ref through `demote` says `demote`, not `promote` — the two
  // handlers each interpolate their own name.
  auto const demoting = dispatch(fx, {"demote", "plan:some-slug"});
  CHECK(demoting.code == 2);
  CHECK(demoting.err == "error: demote requires a numeric id (got slug 'plan:some-slug')\n");
}

TEST_CASE("promote: the target-scope refusals are all exit 1", "[cmd][promote]") {
  auto const fx = make_fixture("target");
  seed(fx);

  // A `repo:` target is refused BEFORE any lookup — `proj` is a real
  // project slug and it still refuses on the prefix alone.
  auto const repo = dispatch(fx, {"promote", "plan:1", "--to", "repo:proj"});
  CHECK(repo.code == 1);
  CHECK(repo.err == "error: repo: scopes are not supported\n");

  auto const unknown = dispatch(fx, {"promote", "plan:1", "--to", "nonexistent"});
  CHECK(unknown.code == 1);
  CHECK(unknown.err == "error: no association with slug 'nonexistent'\n");

  // Already there: the seeded plan is in `project:proj` to begin with.
  auto const same = dispatch(fx, {"promote", "plan:1", "--to", "project:proj"});
  CHECK(same.code == 1);
  CHECK(same.err == "error: plan:1 is already at scope 'project:proj'\n");
}

TEST_CASE("promote/demote: the --json envelope reports the row's NEW scope", "[cmd][promote]") {
  auto const fx = make_fixture("json");
  seed(fx);

  // association -> association. `previous_scope_id` is non-null here, which
  // is the arm a single-association fixture cannot reach.
  auto const moved = dispatch(fx, {"promote", "plan:1", "--to", "org:acme", "--json"});
  REQUIRE(moved.code == 0);
  CHECK(moved.err.empty());
  CHECK(moved.out == R"({"ok":true,"kind":"plan","id":1,"scope_kind":"association","scope_id":2,)"
                     R"("previous_scope_kind":"association","previous_scope_id":1})"
                     "\n");

  // association -> global. `scope_id` goes null; `previous_scope_id` holds
  // the association it came from.
  auto const down = dispatch(fx, {"demote", "plan:1", "--json"});
  REQUIRE(down.code == 0);
  CHECK(down.out == R"({"ok":true,"kind":"plan","id":1,"scope_kind":"global","scope_id":null,)"
                    R"("previous_scope_kind":"association","previous_scope_id":2})"
                    "\n");
}

TEST_CASE("demote: --from is accepted and DISCARDED", "[cmd][demote]") {
  auto const fx = make_fixture("from");
  seed(fx);

  // A slug that does not exist is not a refusal: the engine call takes no
  // source scope.
  auto const got = dispatch(fx, {"demote", "plan:1", "--from", "nonexistent-slug"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK(got.out == "plan:1 demoted to global  (was: association:1)\n");

  // Demoting again IS a refusal — so the success above was a real state
  // change and not a no-op that happened to exit 0.
  auto const again = dispatch(fx, {"demote", "plan:1", "--from", "nonexistent-slug"});
  CHECK(again.code == 1);
  CHECK(again.err == "error: plan:1 is already at global scope\n");
}

TEST_CASE("test-spec status: a real MILESTONE reports 'not found'", "[cmd][test-spec]") {
  auto const fx = make_fixture("anchor");
  seed(fx);

  // Plan 2 / slug `m1` both exist — the fixture case above proves it — and
  // both report not-found because the lookup requires an ANCHOR.
  for (auto const& arg : {"2", "m1"}) {
    INFO("milestone argument: " << arg);
    auto const got = dispatch(fx, {"test-spec", "status", arg});
    CHECK(got.code == 1);
    CHECK(got.out.empty());
    CHECK(got.err == std::format("error: plan '{}' not found\n", arg));
  }

  for (auto const& arg : {"999", "nope"}) {
    INFO("absent argument: " << arg);
    auto const got = dispatch(fx, {"test-spec", "status", arg});
    CHECK(got.code == 1);
    CHECK(got.err == std::format("error: plan '{}' not found\n", arg));
  }

  // Non-vacuous: the ANCHOR resolves, by id and by slug alike.
  for (auto const& arg : {"1", "anchor"}) {
    INFO("anchor argument: " << arg);
    auto const got = dispatch(fx, {"test-spec", "status", arg});
    CHECK(got.code == 0);
    CHECK(got.out.starts_with("test-spec status for plan 1 (anchor)\n"));
  }
}

TEST_CASE("test-spec status: --json is NDJSON, one line per milestone plus a summary", "[cmd][test-spec]") {
  auto const fx = make_fixture("ndjson");
  seed(fx);

  auto const got = dispatch(fx, {"test-spec", "status", "1", "--json"});
  REQUIRE(got.code == 0);
  CHECK(got.err.empty());
  // NOT a single document: two newline-terminated objects, and the payload
  // never opens an array.
  CHECK_FALSE(got.out.starts_with("["));
  CHECK(got.out == R"({"plan_id":1,"title":"Anchor","total_tasks":0,"tasks_with_slug":0,"tasks_covered":0,)"
                   R"("happy":0,"empty":0,"error":0,"edge":0,"other":0})"
                   "\n"
                   R"({"anchor_plan_id":1,"total_tasks":0,"tasks_with_slug":0,"tasks_covered":0,"total_scenarios":0})"
                   "\n");

  // The text arm on the same fixture carries the signed counts and the
  // footer sentence.
  auto const text = dispatch(fx, {"test-spec", "status", "1"});
  REQUIRE(text.code == 0);
  CHECK(text.out.find("milestone") != std::string::npos);
  CHECK(text.out.find("+0") != std::string::npos);
  CHECK(text.out.ends_with("0 scenarios total; 0 of 0 slug-bearing tasks covered (0 total tasks).\n"));
}
