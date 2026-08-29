// @file assoc_detect.t.cpp
// @brief In-process tests for `planar assoc detect`, the last `assoc` leaf
// wired by plan 996 (task 6325).
//
// ## THIS SUITE EXISTS BECAUSE THE ENGINE TESTS CANNOT REACH THE PROBES
//
// `association.t.cpp` covers `proposals_from_signals` exhaustively, and it
// is a PURE function — it is handed the signals. What it cannot cover is
// where those signals come from: a real `git remote get-url origin`, a
// real parent directory, a real marker file on disk. Every case here
// builds an actual directory tree (and, for the remote arm, an actual git
// repository) so the gathering half is exercised rather than assumed.
//
// ## THE FIXTURE IS ASSERTED BEFORE ANYTHING IS COMPARED AGAINST IT
//
// A detector's whole output is DERIVED data. An empty fixture yields an
// empty proposal set, and then every "the output does not contain X"
// assertion passes while testing nothing — the failure mode that made a
// whole scope-resolver suite vacuous at task 6256. Two defences are used
// throughout:
//
//   1. A dedicated first case (`the fixture itself produces signal`)
//      pins the arena's own shape — the repos that exist, the remote that
//      is set, the markers on disk — before any behavioural case runs.
//   2. Every individual case asserts a NON-EMPTY, fully-spelled payload.
//      No case in this file asserts only an absence.
//
// ## ABSOLUTE PATHS, FOR A REASON SPECIFIC TO THIS VERB
//
// Task 6256: `assoc add <slug> .` stores the literal `.` in
// `projects.root_path`. `assoc detect --apply` resolves its project by
// `root_path` STRING EQUALITY against the cwd, so a fixture seeded with a
// relative path could never match and `--apply` would refuse for the wrong
// reason — while a test asserting only "exit 1" would happily pass. Every
// project here is registered through `init` from an absolute cwd, and the
// apply cases assert the SUCCESS path as well as the refusal.
//
// ## `--apply` DOES NOT ECHO WHAT THE PREVIEW SHOWED
//
// The oracle re-enriches after applying, so a successful `--apply` labels
// every row `already a member` — never the `will create` the very same
// invocation would have printed a moment earlier. Asserting the preview's
// labels on the apply path is the natural mistake and it is wrong.
//
// Include-before-import is deliberate (see db/db.t.cpp).

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.tree;

namespace {

using planar::cmd::context;

/// @brief One handler invocation's observable result.
struct invocation {
  int         code = 0; ///< The exit code.
  std::string out;      ///< Everything written to stdout.
  std::string err;      ///< Everything written to stderr.
};

/// @brief A scratch arena. `repo` sits one level under `parent`, so the
/// `path:` proposal is always `path:<parent's name>` and never the repo's.
struct fixture {
  std::filesystem::path                           root;    ///< The scratch root.
  std::filesystem::path                           repo;    ///< The cwd every case dispatches from.
  std::map<std::string, std::string, std::less<>> vars;    ///< The environment map.
  std::filesystem::path                           db_path; ///< Inside `root`; never the operator's.
};

/// @brief Run a shell command for FIXTURE CONSTRUCTION only (`git init`,
/// `git remote add`). Never used to produce a value under test.
/// @param dir The working directory.
/// @param cmd The command line.
/// @return True on exit 0.
auto fixture_sh(const std::filesystem::path& dir, std::string_view cmd) -> bool {
  auto const full = std::format("cd {} && {} >/dev/null 2>&1", dir.string(), cmd);
  return std::system(full.c_str()) == 0;
}

/// @brief Build an arena whose PARENT directory is named `parent_name`.
///
/// The parent's name is a parameter because it is the `path:` proposal's
/// entire input, and several cases need to control it (to exercise
/// sanitization, or to make the resulting slug unmistakable).
/// @param tag A short discriminator so a failure names its own case.
/// @param parent_name The directory name that becomes the `path:` slug.
/// @return The fixture, with `repo` created but empty.
auto make_fixture(std::string_view tag, std::string_view parent_name) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_detect_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  auto const      repo = root / parent_name / "repo";
  std::error_code ec;
  std::filesystem::create_directories(repo, ec);
  return fixture{
      .root    = root,
      .repo    = repo,
      .vars    = {{"PLANAR_DB", (root / "planar.db").string()},
                  {"PLANAR_HOME", (root / "home").string()},
                  {"PLANAR_LOCAL_HOME", (root / "localhome").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", repo.string()}},
      .db_path = root / "planar.db",
  };
}

/// @brief Dispatch `args` against the real tree and table, from the
/// fixture's repo directory.
/// @param fx The fixture.
/// @param args The argv tail.
/// @return The captured invocation.
auto dispatch(const fixture& fx, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv), planar::cmd::map_env(fx.vars), fx.repo, fx.db_path, out, err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::handlers(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

/// @brief Drop an empty ecosystem marker file into the repo.
/// @param fx The fixture.
/// @param name The marker filename (e.g. `go.mod`).
void touch_marker(const fixture& fx, std::string_view name) {
  std::ofstream f{fx.repo / name};
  REQUIRE(f.good());
}

/// @brief Register the repo as a project, so `--apply` has an endpoint.
/// @param fx The fixture.
void register_project(const fixture& fx) {
  auto const init = dispatch(fx, {"init", "--json"});
  REQUIRE(init.code == 0);
}

} // namespace

TEST_CASE("assoc detect: the fixture itself produces signal", "[cmd][assoc][detect][fixture]") {
  // THE ANTI-VACUITY CASE. Everything below depends on this arena actually
  // carrying the three signals, so they are pinned here, on disk, before
  // any behavioural comparison runs.
  auto const fx = make_fixture("fixture_shape", "acme-workspace");
  REQUIRE(fixture_sh(fx.repo, "git init -q -b main"));
  REQUIRE(fixture_sh(fx.repo, "git remote add origin git@github.com:AcmeCorp/My_Repo.git"));
  touch_marker(fx, "go.mod");

  // 1. The tree is shaped the way the `path:` arm needs: the repo's PARENT
  //    is the named directory, so the slug comes from `acme-workspace` and
  //    not from `repo`.
  REQUIRE(std::filesystem::exists(fx.repo));
  REQUIRE(fx.repo.parent_path().filename() == "acme-workspace");
  // 2. The marker is really on disk.
  REQUIRE(std::filesystem::exists(fx.repo / "go.mod"));
  // 3. The git remote is really set — checked through git itself, not
  //    through the verb under test.
  REQUIRE(fixture_sh(fx.repo, "git remote get-url origin"));

  // 4. And the verb therefore answers with FOUR proposals, not zero. Every
  //    later case in this file is comparing against a payload of this
  //    shape; if this line fails, none of them mean anything.
  auto const json = dispatch(fx, {"assoc", "detect", "--json"});
  REQUIRE(json.code == 0);
  REQUIRE_FALSE(json.out.empty());
  REQUIRE(std::ranges::count(json.out, '\n') == 4);
}

TEST_CASE("assoc detect: all four arms, in host/org/path/lang order", "[cmd][assoc][detect][json]") {
  auto const fx = make_fixture("all_arms", "acme-workspace");
  REQUIRE(fixture_sh(fx.repo, "git init -q -b main"));
  REQUIRE(fixture_sh(fx.repo, "git remote add origin git@github.com:AcmeCorp/My_Repo.git"));
  touch_marker(fx, "go.mod");

  auto const json = dispatch(fx, {"assoc", "detect", "--json"});
  CHECK(json.code == 0);
  CHECK(json.err.empty());
  // Newline-delimited objects, NOT a JSON array — the whole payload is not
  // parseable by one `JSON.parse`, and the trailing newline is the
  // handler's terminator. Spelled out in full rather than probed with
  // `contains`, so a reordering or a dropped field fails here.
  CHECK(json.out == R"({"slug":"host:github.com","kind":"host","source":"auto:git-remote","reason":"from git remote host",)"
                    R"("assoc_exists":false,"member_exists":false,"action":"will create"})"
                    "\n"
                    R"({"slug":"org:acmecorp","kind":"org","source":"auto:git-remote","reason":"from git remote org",)"
                    R"("assoc_exists":false,"member_exists":false,"action":"will create"})"
                    "\n"
                    R"({"slug":"path:acme-workspace","kind":"path","source":"auto:path","reason":"from parent directory",)"
                    R"("assoc_exists":false,"member_exists":false,"action":"will create"})"
                    "\n"
                    R"({"slug":"lang:go","kind":"lang","source":"auto:lang","reason":"from detected language ecosystem",)"
                    R"("assoc_exists":false,"member_exists":false,"action":"will create"})"
                    "\n");

  // The text arm over the SAME arena. ONE space after the padded slug, TWO
  // after the closing paren.
  auto const text = dispatch(fx, {"assoc", "detect"});
  CHECK(text.code == 0);
  CHECK(text.out == "proposed associations:\n"
                    "  host:github.com          (from git remote host)  [will create]\n"
                    "  org:acmecorp             (from git remote org)  [will create]\n"
                    "  path:acme-workspace      (from parent directory)  [will create]\n"
                    "  lang:go                  (from detected language ecosystem)  [will create]\n");
}

TEST_CASE("assoc detect: the language arms are independent, and first marker wins", "[cmd][assoc][detect][lang]") {
  // Each ecosystem on its OWN arena. Inferring three of these from one
  // multi-marker fixture would miss a table wired to the wrong tag, since
  // only the winning entry is ever observable in a conflict fixture.
  struct arm {
    std::string_view marker;
    std::string_view slug;
  };
  for (auto const& [marker, slug] : {arm{"go.mod", "lang:go"}, arm{"Cargo.toml", "lang:rust"},
                                     arm{"package.json", "lang:javascript"}, arm{"pyproject.toml", "lang:python"}}) {
    INFO("marker " << marker << " -> " << slug);
    auto const fx = make_fixture("lang_arm", "ws");
    touch_marker(fx, marker);
    auto const json = dispatch(fx, {"assoc", "detect", "--json"});
    CHECK(json.code == 0);
    // TWO proposals: the parent-directory one always fires too. Asserting
    // the lang line alone would not notice the path arm silently dying.
    CHECK(json.out.contains(std::format(R"("slug":"{}")", slug)));
    CHECK(json.out.contains(R"("slug":"path:ws")"));
    CHECK(std::ranges::count(json.out, '\n') == 2);
  }

  // NO marker at all: the lang arm contributes nothing and the path arm
  // still does. The present case is asserted alongside the absent one, so
  // this cannot pass by the whole verb failing.
  {
    auto const fx   = make_fixture("lang_none", "ws");
    auto const json = dispatch(fx, {"assoc", "detect", "--json"});
    CHECK(json.code == 0);
    CHECK_FALSE(json.out.contains(R"("kind":"lang")"));
    CHECK(json.out.contains(R"("slug":"path:ws")"));
    CHECK(std::ranges::count(json.out, '\n') == 1);
  }

  // CONFLICT: all four markers present. Exactly ONE lang proposal is
  // emitted and it is `go` — not because go is more specific or sorts
  // first, but because `go.mod` is tested first. Reordering the table
  // would change this answer, which is why it is pinned.
  {
    auto const fx = make_fixture("lang_conflict", "ws");
    for (auto const* m : {"Cargo.toml", "package.json", "pyproject.toml", "go.mod"}) {
      touch_marker(fx, m);
    }
    auto const json = dispatch(fx, {"assoc", "detect", "--json"});
    CHECK(json.code == 0);
    CHECK(json.out.contains(R"("slug":"lang:go")"));
    CHECK_FALSE(json.out.contains(R"("slug":"lang:rust")"));
    CHECK_FALSE(json.out.contains(R"("slug":"lang:javascript")"));
    CHECK_FALSE(json.out.contains(R"("slug":"lang:python")"));
    CHECK(std::ranges::count(json.out, '\n') == 2); // path + exactly one lang
  }
}

TEST_CASE("assoc detect: a git repo with no origin loses only the remote arms", "[cmd][assoc][detect][git]") {
  auto const fx = make_fixture("no_origin", "ws");
  REQUIRE(fixture_sh(fx.repo, "git init -q -b main"));
  touch_marker(fx, "go.mod");
  // Deliberately NO `git remote add`.

  auto const json = dispatch(fx, {"assoc", "detect", "--json"});
  CHECK(json.code == 0);
  CHECK(json.err.empty()); // a missing origin is not an error, not even a warning
  CHECK_FALSE(json.out.contains(R"("kind":"host")"));
  CHECK_FALSE(json.out.contains(R"("kind":"org")"));
  // The other two arms survive — without these the case would pass against
  // a verb that had failed outright.
  CHECK(json.out.contains(R"("slug":"path:ws")"));
  CHECK(json.out.contains(R"("slug":"lang:go")"));
  CHECK(std::ranges::count(json.out, '\n') == 2);
}

TEST_CASE("assoc detect without --apply writes NOTHING", "[cmd][assoc][detect][readonly]") {
  auto const fx = make_fixture("preview_readonly", "ws");
  touch_marker(fx, "go.mod");
  register_project(fx);

  // The table starts empty, and `init` does not populate it — pinned
  // rather than assumed, because if `init` auto-detected then the
  // "unchanged" assertion below would be comparing two identical non-empty
  // listings and would no longer prove the preview is read-only.
  auto const before = dispatch(fx, {"assoc", "list", "--json"});
  REQUIRE(before.code == 0);
  REQUIRE(before.out == "[]\n");

  // The preview genuinely proposes something...
  auto const preview = dispatch(fx, {"assoc", "detect", "--json"});
  REQUIRE(preview.code == 0);
  REQUIRE(std::ranges::count(preview.out, '\n') == 2);
  CHECK(preview.out.contains(R"("action":"will create")"));

  // ...and the table is still empty afterwards. Exit 0 alone would be
  // satisfied by a verb that had written both rows.
  auto const after = dispatch(fx, {"assoc", "list", "--json"});
  CHECK(after.code == 0);
  CHECK(after.out == "[]\n");
}

TEST_CASE("assoc detect --apply refuses in an unregistered directory", "[cmd][assoc][detect][apply][refusal]") {
  // The REFUSAL, probed before any success. No `register_project` here.
  auto const fx = make_fixture("apply_unregistered", "ws");
  touch_marker(fx, "go.mod");

  auto const applied = dispatch(fx, {"assoc", "detect", "--apply", "--json"});
  CHECK(applied.code == 1);
  CHECK(applied.out.empty()); // it dies before printing any proposal
  // The message names the cwd AND the remedy. Pinned as a whole sentence:
  // a refusal that merely said "not found" would satisfy the exit code.
  CHECK(applied.err == std::format("error: no project registered at cwd ({}); run `planar init` first\n", fx.repo.string()));

  // Nothing was written on the way to the refusal.
  auto const after = dispatch(fx, {"assoc", "list", "--json"});
  CHECK(after.code == 0);
  CHECK(after.out == "[]\n");

  // The PREVIEW in the very same unregistered directory still works — so
  // the refusal above is `--apply`'s, not a general "unregistered cwd"
  // failure. Without this the case would not distinguish the two.
  auto const preview = dispatch(fx, {"assoc", "detect", "--json"});
  CHECK(preview.code == 0);
  CHECK(preview.out.contains(R"("slug":"lang:go")"));
}

TEST_CASE("assoc detect --apply creates rows, relabels its own output, and is idempotent", "[cmd][assoc][detect][apply]") {
  auto const fx = make_fixture("apply_success", "ws");
  touch_marker(fx, "go.mod");
  register_project(fx);

  auto const before = dispatch(fx, {"assoc", "list", "--json"});
  REQUIRE(before.out == "[]\n"); // the seed is empty, so the rows below are this verb's

  auto const applied = dispatch(fx, {"assoc", "detect", "--apply", "--json"});
  CHECK(applied.code == 0);
  // EVERY row reads `already a member`, because the handler re-enriches
  // AFTER applying. The preview labels (`will create`) are gone by the
  // time this prints — asserting them here is the natural mistake.
  CHECK(applied.out == R"({"slug":"path:ws","kind":"path","source":"auto:path","reason":"from parent directory",)"
                       R"("assoc_exists":true,"member_exists":true,"action":"already a member"})"
                       "\n"
                       R"({"slug":"lang:go","kind":"lang","source":"auto:lang","reason":"from detected language ecosystem",)"
                       R"("assoc_exists":true,"member_exists":true,"action":"already a member"})"
                       "\n");

  // The rows really landed, with `auto_detected` true and name == slug.
  auto const after = dispatch(fx, {"assoc", "list", "--json"});
  CHECK(after.code == 0);
  CHECK(after.out.contains(R"("slug":"lang:go","name":"lang:go","kind":"lang","auto_detected":true)"));
  CHECK(after.out.contains(R"("slug":"path:ws","name":"path:ws","kind":"path","auto_detected":true)"));

  // The MEMBERSHIP landed too — creating the associations without linking
  // them would satisfy the listing assertion on its own.
  auto const membership = dispatch(fx, {"assoc", "members", "lang:go", "--json"});
  CHECK(membership.code == 0);
  CHECK(membership.out.contains(std::format(R"("root_path":"{}")", fx.repo.string())));

  // Re-applying changes nothing and does not fail on the unique index.
  auto const again = dispatch(fx, {"assoc", "detect", "--apply", "--json"});
  CHECK(again.code == 0);
  CHECK(again.out == applied.out);
  auto const after_again = dispatch(fx, {"assoc", "list", "--json"});
  CHECK(after_again.out == after.out);
}

TEST_CASE("assoc detect labels an existing association distinctly from an existing membership", "[cmd][assoc][detect][label]") {
  auto const fx = make_fixture("labels", "ws");
  touch_marker(fx, "go.mod");
  register_project(fx);

  // Hand-create ONE of the two proposed associations, with a name of its
  // own and no membership. This is the only fixture shape in which the
  // middle label is reachable at all.
  auto const created = dispatch(fx, {"assoc", "create", "path:ws", "--kind", "path", "--name", "Hand Made", "--json"});
  REQUIRE(created.code == 0);

  auto const preview = dispatch(fx, {"assoc", "detect", "--json"});
  CHECK(preview.code == 0);
  // All three labels are now observable at once: the hand-made one is
  // `already exists, will add`, its sibling is still `will create`. The
  // contrast within a single payload is what proves the label is computed
  // per row rather than for the run.
  CHECK(preview.out.contains(R"("slug":"path:ws","kind":"path","source":"auto:path","reason":"from parent directory",)"
                             R"("assoc_exists":true,"member_exists":false,"action":"already exists, will add"})"));
  CHECK(preview.out.contains(R"("slug":"lang:go","kind":"lang","source":"auto:lang",)"
                             R"("reason":"from detected language ecosystem",)"
                             R"("assoc_exists":false,"member_exists":false,"action":"will create"})"));

  // Applying links the pre-existing association WITHOUT rewriting it: the
  // operator's name and its `auto_detected:false` both survive. An upsert
  // in place of the oracle's `insert or ignore` would relabel a human's
  // association as machine-generated, silently.
  auto const applied = dispatch(fx, {"assoc", "detect", "--apply", "--json"});
  CHECK(applied.code == 0);
  auto const listed = dispatch(fx, {"assoc", "list", "--json"});
  CHECK(listed.out.contains(R"("slug":"path:ws","name":"Hand Made","kind":"path","auto_detected":false)"));
  // ...while the row this verb DID create is marked auto-detected, so the
  // assertion above is about preservation and not about the flag never
  // being set.
  CHECK(listed.out.contains(R"("slug":"lang:go","name":"lang:go","kind":"lang","auto_detected":true)"));

  // And the membership was created against that preserved row.
  auto const membership = dispatch(fx, {"assoc", "members", "path:ws", "--json"});
  CHECK(membership.code == 0);
  CHECK(membership.out.contains(std::format(R"("root_path":"{}")", fx.repo.string())));
}

TEST_CASE("assoc detect: the parent directory name is sanitized, not used raw", "[cmd][assoc][detect][slug]") {
  // A directory name carrying a space, a dot and a run of unkeepable
  // characters. `_` and `-` survive verbatim; `.` and the junk run each
  // collapse to a single dash; the whole thing lowercases.
  auto const fx   = make_fixture("slugify", "My Weird.Dir++Name_x");
  auto const json = dispatch(fx, {"assoc", "detect", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out.contains(R"("slug":"path:my-weird-dir-name_x")"));
  CHECK(std::ranges::count(json.out, '\n') == 1);
}

TEST_CASE("assoc detect: the empty answer is a different SHAPE in each arm", "[cmd][assoc][detect][empty]") {
  // Reaching zero proposals takes work: the parent-directory arm fires for
  // essentially every real cwd. A parent whose name sanitizes away to
  // nothing is the one shape that gets there without needing the
  // filesystem root.
  auto const fx = make_fixture("empty", "+++");
  // No marker, no git repo — so the path arm is the only candidate, and it
  // declines.
  auto const json = dispatch(fx, {"assoc", "detect", "--json"});
  CHECK(json.code == 0);
  CHECK(json.err.empty());
  // NOT `[]`, and not zero bytes: an object naming a `proposals` key that
  // the non-empty payload never emits. Reproduced from the oracle rather
  // than normalized.
  CHECK(json.out == "{\"proposals\":[]}\n");

  auto const text = dispatch(fx, {"assoc", "detect"});
  CHECK(text.code == 0);
  // A SENTENCE, and a different one from `assoc list`'s parenthesized
  // `(no associations)`.
  CHECK(text.out == "no proposed associations\n");

  // The SAME arena with one marker added stops being empty — so the two
  // assertions above are about this input, not about a verb that always
  // answers empty.
  touch_marker(fx, "go.mod");
  auto const nonempty = dispatch(fx, {"assoc", "detect", "--json"});
  CHECK(nonempty.code == 0);
  CHECK(nonempty.out.contains(R"("slug":"lang:go")"));
}
