// @file tree_leaf.t.cpp
// @brief Handler-level coverage for the `planar tree` leaf (plan 996,
// task 6278).
//
// The engine's own byte-for-byte cases live in
// `src/lib/engine/tree/walk.t.cpp`. What this file pins is the half the
// engine cannot see: flag PARSING, the cwd-derived scope path, and the
// exit-code mapping — three arms that were each captured from the oracle
// in a pinned scratch arena and that do NOT agree with one another.
//
// ## The three empty-value meanings, captured not inferred
//
// This one verb answers "what does an empty flag value mean" three
// different ways, which is why none of them may be read across from a
// sibling verb:
//
// ```
// $ planar tree --kind ''            -> exit 2, `error: unknown kind ''`
// $ planar tree --status ''          -> exit 0, empty tree, zero counts
// $ planar tree --scope ''           -> exit 0, the GLOBAL scope
// ```
//
// And the two refusals do not share an exit code either — an unknown
// `--kind` is 2 while an unknown `--scope` is 1.
//
// ## `--sort` IS WIRED (task 6281)
//
// The oracle declared the flag, stored it, and never read it: `--sort
// updated` and `--sort bogus` were both byte-identical to a bare `tree` and
// a bogus key did not refuse. Reproduced under D2 until decision 1067 ended
// that rule. It now orders siblings, and an unrecognised key REFUSES at
// exit 2 -- silently accepting a typo'd key was the worse half of the
// defect, because the operator got some other order and no sign of it.

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
                         std::format("planar_treeleaf_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "home", ec);
  std::filesystem::create_directories(root / "proj", ec);
  std::filesystem::create_directories(root / "fakehome", ec);
  std::filesystem::create_directories(root / "outside", ec);
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_HOME", (root / "home").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()}},
      .db_path = root / "planar.db",
  };
}

// @brief Dispatch `args` against the real tree and table, from `cwd`.
auto dispatch_in(const fixture& fx, const std::filesystem::path& cwd, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv), planar::cmd::map_env(fx.vars), cwd, fx.db_path, out, err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::handlers(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

auto dispatch(const fixture& fx, std::vector<std::string> args) -> invocation {
  return dispatch_in(fx, fx.root / "proj", std::move(args));
}

// @brief Seed a scope with a plan, a child plan, and two tasks.
//
// Through the CLI, so the scope state is established by the same code
// paths an operator would take.
auto seed(const fixture& fx) -> void {
  REQUIRE(dispatch(fx, {"init", "--json", "--allow-no-repo"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "feat", "--name", "feat", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "feat", (fx.root / "proj").string(), "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Demo", "--slug", "demo", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Sub", "--slug", "sub", "--parent", "1", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "First task", "--plan", "2", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "Second task", "--plan", "2", "--json"}).code == 0);
}

} // namespace

TEST_CASE("tree: the cwd-derived scope renders a non-empty hierarchy", "[cmd][tree]") {
  // The anti-vacuity gate for every case below: prove the fixture actually
  // produces rows before any other case trusts a green.
  auto const fx = make_fixture("basic");
  seed(fx);

  auto const got = dispatch(fx, {"tree"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());

  // Depth and siblings both present, not just "some output".
  CHECK(got.out.find("plan:1 [draft]  Demo\n") != std::string::npos);
  CHECK(got.out.find("plan:2 [draft]  Sub\n") != std::string::npos);
  CHECK(got.out.find("task:1  First task  [todo, pri:100]") != std::string::npos);
  CHECK(got.out.find("task:2  Second task  [todo, pri:100]") != std::string::npos);
  CHECK(got.out.find("├── ") != std::string::npos);
  CHECK(got.out.find("└── ") != std::string::npos);
  CHECK(got.out.ends_with("2 plans, 2 tasks, 0 artifacts, 0 decisions, 0 scenarios, 0 questions\n"));
}

TEST_CASE("tree: --json emits a single scope OBJECT with a trailing newline", "[cmd][tree]") {
  auto const fx = make_fixture("json");
  seed(fx);

  auto const got = dispatch(fx, {"tree", "--json"});
  REQUIRE(got.code == 0);
  CHECK(got.err.empty());
  CHECK(got.out.starts_with(R"({"kind":"scope",)"));
  CHECK(got.out.ends_with("}\n"));
  // Non-vacuous: the payload carries the seeded entities, not just a shell.
  CHECK(got.out.find(R"("title":"Demo")") != std::string::npos);
  CHECK(got.out.find(R"("title":"First task")") != std::string::npos);
  // A scope root must NOT leak the entity fields (the task-2377 split).
  CHECK(got.out.starts_with(R"({"kind":"scope","title":)"));
  CHECK(got.out.find(R"("kind":"scope","id":)") == std::string::npos);
}

TEST_CASE("tree: an unknown --kind refuses at exit 2 and quotes the value", "[cmd][tree]") {
  auto const fx = make_fixture("kindbad");
  seed(fx);

  auto const bogus = dispatch(fx, {"tree", "--kind", "bogus"});
  CHECK(bogus.code == 2);
  CHECK(bogus.out.empty());
  CHECK(bogus.err == "error: unknown kind 'bogus'\n");

  // The EMPTY kind refuses identically. It does NOT mean "all kinds", and
  // reading it that way is the obvious wrong assumption.
  auto const empty = dispatch(fx, {"tree", "--kind", ""});
  CHECK(empty.code == 2);
  CHECK(empty.out.empty());
  CHECK(empty.err == "error: unknown kind ''\n");

  // Non-vacuous: a VALID kind on the same fixture succeeds and prints.
  auto const ok = dispatch(fx, {"tree", "--kind", "plan"});
  CHECK(ok.code == 0);
  CHECK(ok.out.find("plan:1 [draft]  Demo") != std::string::npos);
}

TEST_CASE("tree: an unknown --status is exit 0 and EMPTY, not a refusal", "[cmd][tree]") {
  // The asymmetry with `--kind`. Only `--kind` validates its argument;
  // a typo'd status is an empty result an operator has to notice for
  // themselves.
  auto const fx = make_fixture("statbad");
  seed(fx);

  auto const unknown = dispatch(fx, {"tree", "--status", "nosuchstatus"});
  CHECK(unknown.code == 0);
  CHECK(unknown.err.empty());
  CHECK(unknown.out.ends_with("0 plans, 0 tasks, 0 artifacts, 0 decisions, 0 scenarios, 0 questions\n"));

  // An EMPTY status behaves the same way — it matches the literal empty
  // string, which nothing carries. It does NOT mean "any status".
  auto const empty = dispatch(fx, {"tree", "--status", ""});
  CHECK(empty.code == 0);
  CHECK(empty.out.ends_with("0 plans, 0 tasks, 0 artifacts, 0 decisions, 0 scenarios, 0 questions\n"));

  // Non-vacuous: the status the rows DO carry matches.
  auto const draft = dispatch(fx, {"tree", "--status", "draft"});
  CHECK(draft.code == 0);
  CHECK(draft.out.find("plan:1 [draft]  Demo") != std::string::npos);
}

TEST_CASE("tree: an unknown --scope refuses at exit 1, and an EMPTY --scope is global", "[cmd][tree]") {
  auto const fx = make_fixture("scope");
  seed(fx);

  // Exit 1 here, where an unknown --kind was exit 2. The two refusals do
  // not share a bucket.
  auto const missing = dispatch(fx, {"tree", "--scope", "no-such-scope"});
  CHECK(missing.code == 1);
  CHECK(missing.out.empty());
  CHECK(missing.err == "error: scope slug not found\n");

  // An empty --scope means GLOBAL — not cross-scope, not the cwd scope,
  // and not an error. Global is empty here, so it renders the bare label.
  auto const empty = dispatch(fx, {"tree", "--scope", ""});
  CHECK(empty.code == 0);
  CHECK(empty.err.empty());
  CHECK(empty.out == "global\n\n0 plans, 0 tasks, 0 artifacts, 0 decisions, 0 scenarios, 0 questions\n");

  // ...and it is the SAME output as an explicit `--scope global`, which is
  // what makes "empty means global" the right reading rather than "empty
  // means the default cwd scope" — the cwd scope is NOT empty.
  auto const global = dispatch(fx, {"tree", "--scope", "global"});
  CHECK(global.out == empty.out);
  auto const cwd_scoped = dispatch(fx, {"tree"});
  CHECK(cwd_scoped.out != empty.out);
}

TEST_CASE("tree: a cwd outside every registered scope refuses at exit 1", "[cmd][tree]") {
  auto const fx = make_fixture("outside");
  seed(fx);

  auto const got = dispatch_in(fx, fx.root / "outside", {"tree"});
  CHECK(got.code == 1);
  CHECK(got.out.empty());
  CHECK(got.err == "error: cwd is not inside any registered Planar scope; cd into a registered scope or pass --scope global\n");

  // The remedy the message names actually works from the same cwd, so the
  // refusal is a routing problem and not a dead end.
  auto const remedy = dispatch_in(fx, fx.root / "outside", {"tree", "--scope", "global"});
  CHECK(remedy.code == 0);
}

TEST_CASE("tree: --sort updated REORDERS top-level plans", "[cmd][tree][6281]") {
  // The half a refusal test cannot cover. A break-probe that made the
  // `updated` arm a no-op SURVIVED against the refusal case alone: nothing
  // asserted the ORDER actually changes, so `--sort updated` could have gone
  // on silently doing nothing -- which is the exact defect 6281 reported.
  auto const fx = make_fixture("sortorder");
  REQUIRE(dispatch(fx, {"init", "--json", "--allow-no-repo"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Alpha", "--slug", "alpha", "--scope", "global", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Beta", "--slug", "beta", "--scope", "global", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Gamma", "--slug", "gamma", "--scope", "global", "--json"}).code == 0);

  // Touch plan 1 LAST, so `updated_at` order is the reverse of id order for
  // it. Without this the two keys agree and the case proves nothing.
  REQUIRE(dispatch(fx, {"plan", "update", "1", "--summary", "touched", "--json"}).code == 0);

  auto const by_id = dispatch(fx, {"tree", "--scope", "global"});
  REQUIRE(by_id.code == 0);
  auto const alpha_id = by_id.out.find("Alpha");
  auto const gamma_id = by_id.out.find("Gamma");
  REQUIRE(alpha_id != std::string::npos);
  REQUIRE(gamma_id != std::string::npos);
  CHECK(alpha_id < gamma_id); // id order: 1 before 3

  auto const by_updated = dispatch(fx, {"tree", "--scope", "global", "--sort", "updated"});
  REQUIRE(by_updated.code == 0);
  // Plan 1 was touched last, so it leads under `updated` -- and plan 3 now
  // precedes plan 2, which id order never does.
  auto const beta_up  = by_updated.out.find("Beta");
  auto const gamma_up = by_updated.out.find("Gamma");
  REQUIRE(beta_up != std::string::npos);
  REQUIRE(gamma_up != std::string::npos);
  CHECK(gamma_up < beta_up);

  // And the two renderings genuinely differ, which is the blunt form of the
  // same claim.
  CHECK(by_updated.out != by_id.out);
}

TEST_CASE("tree: --sort id and unsorted match a bare tree; a bogus key REFUSES", "[cmd][tree][6281]") {
  // INVERTED AT TASK 6281. Every key used to be byte-identical to a bare
  // `tree` and a bogus key exited 0.
  auto const fx = make_fixture("sort");
  seed(fx);

  auto const plain = dispatch(fx, {"tree"});
  REQUIRE(plain.code == 0);
  REQUIRE_FALSE(plain.out.empty()); // the comparisons below need real bytes

  // `id` is the default, and `unsorted` leaves the walk's own order (which
  // every query emits as `order by id`) untouched -- so both still match.
  for (auto const* key : {"id", "unsorted"}) {
    INFO("--sort " << key);
    auto const sorted = dispatch(fx, {"tree", "--sort", key});
    CHECK(sorted.code == 0);
    CHECK(sorted.err.empty());
    CHECK(sorted.out == plain.out);
  }

  // A typo'd key is the half that mattered: accepting it silently handed the
  // operator some other order with no indication.
  auto const bogus = dispatch(fx, {"tree", "--sort", "bogus"});
  CHECK(bogus.code == 2);
  CHECK(bogus.err == "error: unknown --sort value 'bogus'; expected one of: id, updated, created, unsorted\n");
  CHECK(bogus.out.empty());

  // An EMPTY value is a value, not an absent flag, so it refuses too.
  auto const empty = dispatch(fx, {"tree", "--sort", ""});
  CHECK(empty.code == 2);
  CHECK(empty.err.contains("unknown --sort value"));
}

TEST_CASE("tree: --depth caps descent, and 0 is unbounded", "[cmd][tree]") {
  auto const fx = make_fixture("depth");
  seed(fx);

  auto const unbounded = dispatch(fx, {"tree"});
  REQUIRE(unbounded.out.find("task:1  First task") != std::string::npos);

  // Depth 1 stops before the tasks under the child plan.
  auto const depth1 = dispatch(fx, {"tree", "--depth", "1"});
  CHECK(depth1.code == 0);
  CHECK(depth1.out.find("plan:2 [draft]  Sub") != std::string::npos);
  CHECK(depth1.out.find("task:1  First task") == std::string::npos);

  // 0 and -1 are both UNBOUNDED, not "no levels".
  CHECK(dispatch(fx, {"tree", "--depth", "0"}).out == unbounded.out);
  CHECK(dispatch(fx, {"tree", "--depth", "-1"}).out == unbounded.out);
  CHECK(depth1.out != unbounded.out);
}

TEST_CASE("tree: --all-scopes renders every scope root, separated by a blank line", "[cmd][tree]") {
  auto const fx = make_fixture("allscopes");
  seed(fx);

  auto const got = dispatch(fx, {"tree", "--all-scopes"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK(got.out.starts_with("global\n\n"));
  CHECK(got.out.find("assoc:feat\n") != std::string::npos);
  CHECK(got.out.find("repo:proj\n") != std::string::npos);
  // ONE footer for the whole run, counting across every root.
  CHECK(got.out.ends_with("2 plans, 2 tasks, 0 artifacts, 0 decisions, 0 scenarios, 0 questions\n"));

  // Multiple roots serialise as an ARRAY, where a single root was an
  // object. The asymmetry is the oracle's.
  auto const as_json = dispatch(fx, {"tree", "--all-scopes", "--json"});
  CHECK(as_json.code == 0);
  CHECK(as_json.out.starts_with("[{\"kind\":\"scope\""));
  CHECK(as_json.out.ends_with("]\n"));
}

TEST_CASE("tree: an empty scope is exit 0 with a label, not an error", "[cmd][tree]") {
  // A read verb that finds nothing is not a failure. Worth pinning because
  // exit 0 with plausible-looking output is the shape that hides a broken
  // filter, so the label AND the zeroed footer are both asserted.
  auto const fx = make_fixture("emptyscope");
  REQUIRE(dispatch(fx, {"init", "--json", "--allow-no-repo"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "bare", "--name", "bare", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "bare", (fx.root / "proj").string(), "--json"}).code == 0);

  auto const got = dispatch(fx, {"tree"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK(got.out == "assoc:bare\n\n0 plans, 0 tasks, 0 artifacts, 0 decisions, 0 scenarios, 0 questions\n");
}
