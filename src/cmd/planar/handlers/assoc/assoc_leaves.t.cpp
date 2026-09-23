// @file assoc_leaves.t.cpp
// @brief In-process tests for the two `planar assoc` leaves wired by plan
// 996, task 6279: `list` and `remove`.
//
// ## THE FIXTURE SEEDS WITH ABSOLUTE PATHS, AND THAT IS LOAD-BEARING HERE
// ## IN A WAY IT IS NOT ANYWHERE ELSE
//
// `assoc add <slug> .` USED to store the literal string `.` in
// `projects.root_path` (task 6256, fixed 2026-09-12: a relative repo-path is
// now resolved against the invocation cwd and lexically normalised, while an
// absolute one still goes in verbatim). Every other suite that tripped on
// this lost the *cwd-derivation* arm and passed vacuously on an empty
// answer. This suite would have lost something worse: `assoc remove` matches
// `root_path` by STRING EQUALITY, so a fixture that seeded with `.` and
// removed with an absolute path got `NotAMember` — and a "the membership is
// gone" assertion was then satisfied by a membership that was never created.
//
// The absolute-path seeding and the assert-it-EXISTS-before-removing
// discipline both STAY. They cost nothing, and they are what makes a vacuous
// seed fail at the seed rather than pass at the end — which is a property
// worth keeping whether or not `.` happens to work today.
//
// ## `assoc list --kind` BINDS THE HYPHENATED WIRE FORM, AND ONE KIND
// ## PROVES IT
//
// Seven of the eight kinds spell identically in C++ and in the `kind`
// column. The eighth is `ad-hoc`, whose C++ enumerator is `ad_hoc` because
// a hyphen is not an identifier character. A filter that bound the
// enumerator's spelling would match no row and answer `(no associations)`
// — which is a legal-looking answer, not a crash. `ad-hoc` is also the
// DEFAULT kind, so it is the one an operator filters on most and the one a
// fixture built from explicit `--kind` values never exercises. There is a
// case below that creates an association with NO `--kind` and then filters
// for `ad-hoc`; it is the only case in this file that would fail on the
// wrong bind form.
//
// ## EVERY FILTER CASE ASSERTS THE PRESENT ROW TOO
//
// "Filtering for `org` does not list the `project` row" passes just as
// happily against a filter that matches NOTHING, a broken `where` clause,
// or an empty table. So each filter case pins the full byte payload of
// both the matching and the non-matching filter over the SAME seeded
// table, and the unfiltered listing over it as well. The three together
// cannot all hold unless the predicate actually discriminates.
//
// ## THE COLUMN WIDTHS COME FROM THE ORACLE'S FORMAT STRING, NOT FROM
// ## MEASURING OUTPUT
//
// `renderListText` is `{s:<20}  {s:<12}  {s}`. The kind column's twelve is
// unobservable from any fixture built out of real kinds — the longest kind
// text is `personal` at eight, so eight, twelve and forty all render
// identically once the two-space gutter is added. The width is therefore
// transcribed from zig/src/engine/identity/association.zig rather than
// inferred, and what the fixture below CAN and does pin is the pair of
// properties a wrong width would break in an observable way: a slug
// shorter than twenty is padded out to twenty, and a slug LONGER than
// twenty is not truncated and pushes the rest of the line right.
//
// ## `assoc remove`'s SECOND REFUSAL IS NOT A MEMBERSHIP CHECK, DESPITE
// ## BEING CALLED `NotAMember`
//
// The two families share `no association named '<slug>'` and nothing else.
// `remove`'s other arm is `NotAMember`, worded `no project registered at
// '<repo-path>'` — a sentence about the PROJECT, naming neither the
// association nor the membership, and that wording is the accurate one.
// The error is produced by the project-by-path lookup alone; the `delete
// from project_associations` that follows is unconditional and its
// affected-row count is never read.
//
// So a project that IS registered but is NOT linked to this association
// takes the SUCCESS path: exit 0, a "removed project at … from …"
// sentence, zero rows deleted, and a durable `audit_log` row claiming an
// unlink that never happened. That was asserted the other way round in the
// first draft of this file, failed, and was re-probed against
// `zig/zig-out/bin/planar` in a pinned arena — where the oracle behaves
// identically. It is an ORACLE DEFECT, pinned in its own case below rather
// than fixed, and it needs a task row and a two-sided fix.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;

namespace {

using planar::cmd::context;

/// @brief One handler invocation's observable result.
struct invocation {
  int         code = 0; ///< The exit code.
  std::string out;      ///< Everything written to stdout.
  std::string err;      ///< Everything written to stderr.
};

/// @brief A scratch root plus the environment every case dispatches against.
struct fixture {
  std::filesystem::path                           root;    ///< The scratch root.
  std::map<std::string, std::string, std::less<>> vars;    ///< The environment map.
  std::filesystem::path                           db_path; ///< Inside `root`; never the operator's.
};

/// @brief Build a fixture under a unique scratch directory.
/// @param tag A short discriminator so a failure names its own case.
/// @return The fixture.
auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_assoc_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "proj", ec);
  std::filesystem::create_directories(root / "other", ec);
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_DB", (root / "planar.db").string()},
                  {"PLANAR_HOME", (root / "home").string()},
                  {"PLANAR_LOCAL_HOME", (root / "localhome").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()}},
      .db_path = root / "planar.db",
  };
}

/// @brief Dispatch `args` against the real tree and table, from the
/// fixture's project root.
/// @param fx The fixture.
/// @param args The argv tail.
/// @return The captured invocation.
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

/// @brief Initialize the database without registering the cwd as a project.
///
/// `--skip-project` deliberately: `assoc list` is NOT scope-resolved and
/// must answer identically whether or not the cwd belongs to anything, and
/// the remove cases register their projects explicitly so the membership
/// under test is the one the case created rather than one `init` happened
/// to make.
/// @param fx The fixture.
void seed_db(const fixture& fx) {
  auto const init = dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"});
  REQUIRE(init.code == 0);
}

/// @brief Create an association with an explicit kind.
/// @param fx The fixture.
/// @param slug The association slug.
/// @param kind The `--kind` value, in its WIRE spelling.
void create_assoc(const fixture& fx, std::string_view slug, std::string_view kind) {
  auto const got = dispatch(fx, {"assoc", "create", std::string{slug}, "--kind", std::string{kind}, "--json"});
  REQUIRE(got.code == 0);
}

} // namespace

// =========================================================================
// assoc list
// =========================================================================

TEST_CASE("assoc list on an empty table is a LISTING, not a refusal, in two shapes", "[cmd][assoc][list]") {
  auto const fx = make_fixture("list_empty");
  seed_db(fx);

  // A WORD, not zero bytes — and not the member renderer's `(no members)`.
  auto const text = dispatch(fx, {"assoc", "list"});
  CHECK(text.code == 0);
  CHECK(text.err.empty());
  CHECK(text.out == "(no associations)\n");

  // `[]`, NOT the text arm's sentence and NOT `null`. The two empty shapes
  // are spelled differently on purpose and neither is derivable from the
  // other.
  auto const json = dispatch(fx, {"assoc", "list", "--json"});
  CHECK(json.code == 0);
  CHECK(json.err.empty());
  CHECK(json.out == "[]\n");
}

TEST_CASE("assoc list pads short slugs to twenty and does not truncate long ones", "[cmd][assoc][list]") {
  auto const fx = make_fixture("list_widths");
  seed_db(fx);

  // `aa` sorts first, so the ordering below is also the slug ordering and
  // not insertion order — created in the reverse order to prove it.
  create_assoc(fx, "zz-this-slug-is-long-enough-to-overflow", "client");
  create_assoc(fx, "aa", "org");

  auto const text = dispatch(fx, {"assoc", "list"});
  CHECK(text.code == 0);
  CHECK(text.err.empty());
  // Written out literally rather than composed with the same `{:<20}` the
  // renderer uses: deriving the expectation from the format string under
  // test would make any width agree with itself.
  CHECK(text.out == "aa                    org           aa\n"
                    "zz-this-slug-is-long-enough-to-overflow  client        zz-this-slug-is-long-enough-to-overflow\n");
}

TEST_CASE("assoc list --kind discriminates, proven against the same seeded table", "[cmd][assoc][list][kind]") {
  auto const fx = make_fixture("list_kind");
  seed_db(fx);

  create_assoc(fx, "alpha", "org");
  create_assoc(fx, "beta", "project");
  create_assoc(fx, "gamma", "org");

  // 1. UNFILTERED lists all three. Without this, every assertion below is
  //    equally satisfied by a predicate that matches nothing.
  auto const all = dispatch(fx, {"assoc", "list"});
  CHECK(all.code == 0);
  CHECK(all.out == "alpha                 org           alpha\n"
                   "beta                  project       beta\n"
                   "gamma                 org           gamma\n");

  // 2. The MATCHING filter keeps two and drops one.
  auto const orgs = dispatch(fx, {"assoc", "list", "--kind", "org"});
  CHECK(orgs.code == 0);
  CHECK(orgs.err.empty());
  CHECK(orgs.out == "alpha                 org           alpha\n"
                    "gamma                 org           gamma\n");

  // 3. The OTHER matching filter keeps the one the first dropped — so the
  //    row is present in the table and it is the PREDICATE, not the seed,
  //    that removed it from (2).
  auto const projects = dispatch(fx, {"assoc", "list", "--kind", "project"});
  CHECK(projects.code == 0);
  CHECK(projects.out == "beta                  project       beta\n");

  // 4. A kind that is legal but seeded by nothing lists empty — an ANSWER,
  //    exit 0, not a refusal.
  auto const personal = dispatch(fx, {"assoc", "list", "--kind", "personal"});
  CHECK(personal.code == 0);
  CHECK(personal.err.empty());
  CHECK(personal.out == "(no associations)\n");

  // 5. The JSON arm filters too — the predicate lives in the engine, not
  //    in the renderer, and a port that filtered only on the text path
  //    would pass 1-4.
  auto const json = dispatch(fx, {"assoc", "list", "--kind", "project", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out.starts_with(R"([{"id":)"));
  CHECK(json.out.contains(R"("slug":"beta")"));
  CHECK(json.out.contains(R"("kind":"project")"));
  CHECK_FALSE(json.out.contains(R"("slug":"alpha")"));
  CHECK(json.out.ends_with("}]\n"));
}

TEST_CASE("assoc list --kind ad-hoc matches the DEFAULT-kind row", "[cmd][assoc][list][kind]") {
  auto const fx = make_fixture("list_adhoc");
  seed_db(fx);

  // NO `--kind`, so the engine's `ad_hoc` default lands in the column as
  // the hyphenated wire form `ad-hoc`.
  REQUIRE(dispatch(fx, {"assoc", "create", "defaulted", "--json"}).code == 0);
  create_assoc(fx, "explicit", "org");

  // The whole point of this case: the filter must bind `ad-hoc`, not the
  // C++ enumerator's `ad_hoc`. The wrong spelling answers
  // `(no associations)` here — plausible, silent, and wrong.
  auto const adhoc = dispatch(fx, {"assoc", "list", "--kind", "ad-hoc"});
  CHECK(adhoc.code == 0);
  CHECK(adhoc.out == "defaulted             ad-hoc        defaulted\n");

  // And the underscore spelling is not a kind at all — it REFUSES rather
  // than quietly listing nothing, which is what distinguishes "the bind
  // form is wrong" from "no row matched".
  auto const underscore = dispatch(fx, {"assoc", "list", "--kind", "ad_hoc"});
  CHECK(underscore.code == 2);
  CHECK(underscore.err == "error: unknown kind 'ad_hoc'\n");
}

TEST_CASE("assoc list refuses an unknown --kind at exit 2, and an EMPTY one too", "[cmd][assoc][list][kind]") {
  auto const fx = make_fixture("list_badkind");
  seed_db(fx);
  create_assoc(fx, "alpha", "org");

  // Exit 2, not 1 — `error.InvalidInput` has an arm in `codeFor`. Same
  // code and same wording as `assoc create`'s.
  auto const bad = dispatch(fx, {"assoc", "list", "--kind", "nope"});
  CHECK(bad.code == 2);
  CHECK(bad.out.empty());
  CHECK(bad.err == "error: unknown kind 'nope'\n");

  // PRESENT-but-empty is not ABSENT. `--kind ''` reaches the parser,
  // fails it, and refuses; it does NOT fall back to listing everything.
  // This is the assertion a `value_or("")`-then-test-for-empty
  // implementation fails.
  auto const empty = dispatch(fx, {"assoc", "list", "--kind", ""});
  CHECK(empty.code == 2);
  CHECK(empty.out.empty());
  CHECK(empty.err == "error: unknown kind ''\n");

  // And the absent flag really does list — otherwise the two refusals
  // above would also be satisfied by a verb that refused unconditionally.
  auto const absent = dispatch(fx, {"assoc", "list"});
  CHECK(absent.code == 0);
  CHECK(absent.out == "alpha                 org           alpha\n");
}

// =========================================================================
// assoc remove
// =========================================================================

TEST_CASE("assoc remove drops a membership that is asserted to exist first", "[cmd][assoc][remove]") {
  auto const fx = make_fixture("remove_ok");
  seed_db(fx);
  create_assoc(fx, "acme", "org");

  auto const path = (fx.root / "proj").string();
  REQUIRE(dispatch(fx, {"assoc", "add", "acme", path}).code == 0);

  // THE SEED IS ASSERTED. A `.`-shaped seed (task 6256) would leave this
  // empty and everything below would pass against a membership that never
  // existed.
  auto const before = dispatch(fx, {"assoc", "members", "acme", "--json"});
  REQUIRE(before.code == 0);
  REQUIRE(before.out != "[]\n");
  REQUIRE(before.out.contains(path));

  auto const removed = dispatch(fx, {"assoc", "remove", "acme", path});
  CHECK(removed.code == 0);
  CHECK(removed.err.empty());
  // PATH first, then slug — the argument order is the reverse of the one
  // the operator typed.
  CHECK(removed.out == std::format("removed project at {} from acme\n", path));

  auto const after = dispatch(fx, {"assoc", "members", "acme", "--json"});
  CHECK(after.code == 0);
  CHECK(after.out == "[]\n");
}

TEST_CASE("assoc remove --json emits the four-field literal built from the ARGUMENTS", "[cmd][assoc][remove][json]") {
  auto const fx = make_fixture("remove_json");
  seed_db(fx);
  create_assoc(fx, "acme", "org");

  auto const path = (fx.root / "proj").string();
  REQUIRE(dispatch(fx, {"assoc", "add", "acme", path}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "members", "acme", "--json"}).out.contains(path));

  auto const got = dispatch(fx, {"assoc", "remove", "acme", path, "--json"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  // `"status":"removed"` exists nowhere in the schema — the object is
  // assembled from the two argv values, not read back out of the database
  // (there is nothing left to read).
  CHECK(got.out == std::format(R"({{"status":"removed","association":"acme","repo_path":"{}"}})"
                               "\n",
                               path));
}

TEST_CASE("assoc remove refuses an unknown association, naming the SLUG", "[cmd][assoc][remove]") {
  auto const fx = make_fixture("remove_noassoc");
  seed_db(fx);

  auto const got = dispatch(fx, {"assoc", "remove", "nope", (fx.root / "proj").string()});
  CHECK(got.code == 1);
  CHECK(got.out.empty());
  // Byte-identical to `assoc add`'s unknown-association arm.
  CHECK(got.err == "error: no association named 'nope'\n");
}

TEST_CASE("assoc remove's NotAMember arm is about the PROJECT, not the membership", "[cmd][assoc][remove]") {
  // THE NAME `NotAMember` IS A LIE ABOUT WHEN THE ERROR FIRES, and this
  // case exists because the obvious reading of it is wrong.
  //
  // `removeMember` looks the project up by path and maps only that
  // lookup's `NotFound` onto `NotAMember`. The `delete from
  // project_associations` that follows is UNCONDITIONAL and its affected-row
  // count is never inspected. So the error means "no project is registered
  // at this path", full stop — it says nothing about membership, despite
  // its name and despite reading like a membership check.
  //
  // The first draft of this case asserted the other reading (a registered
  // project that is not linked to this association also refuses) and it
  // FAILED against this tree. Re-probed against
  // `zig/zig-out/bin/planar` in a pinned arena, the oracle agrees with
  // this tree: exit 0 and a success sentence. That is an ORACLE DEFECT and
  // it is pinned below rather than fixed, because fixing it here would be
  // a behaviour change this port is not entitled to make — see the case
  // after this one.
  auto const fx = make_fixture("remove_notmember");
  seed_db(fx);
  create_assoc(fx, "acme", "org");

  auto const bound = (fx.root / "proj").string();
  REQUIRE(dispatch(fx, {"assoc", "add", "acme", bound}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "members", "acme", "--json"}).out.contains(bound));

  // The ONLY cause: no `projects` row is registered at this path at all.
  auto const nowhere = dispatch(fx, {"assoc", "remove", "acme", "/no/such/path/xyz"});
  CHECK(nowhere.code == 1);
  CHECK(nowhere.out.empty());
  CHECK(nowhere.err == "error: no project registered at '/no/such/path/xyz'\n");

  // The membership this case did NOT name is still there — so the refusal
  // above really was a refusal and not a silent removal of something else.
  auto const still = dispatch(fx, {"assoc", "members", "acme", "--json"});
  CHECK(still.code == 0);
  CHECK(still.out.contains(bound));
}

TEST_CASE("assoc remove of a registered-but-UNLINKED project reports success (oracle defect)", "[cmd][assoc][remove]") {
  // ORACLE DEFECT, REPRODUCED DELIBERATELY. Captured from
  // `zig/zig-out/bin/planar` in a pinned arena: with `proj` linked to
  // `acme` and `other` linked to `other`, `assoc remove acme <other>`
  // exits 0, prints `removed project at <other> from acme`, and deletes
  // NOTHING — the `delete` matches zero rows and the count is not checked.
  // It also writes an `audit_log` row (`verb=unlink`, `summary=remove
  // project 'other' from association 'acme'`) asserting a removal that
  // never happened, which is worse than the exit code: the success
  // sentence is transient, the false audit row is durable.
  //
  // Pinned rather than fixed. A port is not entitled to tighten a
  // contract, and an operator script that treats exit 0 as "the membership
  // is gone now" is correct in outcome even here — the membership was
  // already gone. The durable-audit half is the part that needs a task row
  // and a fix on BOTH sides at once.
  auto const fx = make_fixture("remove_unlinked");
  seed_db(fx);
  create_assoc(fx, "acme", "org");
  create_assoc(fx, "other", "org");

  auto const bound   = (fx.root / "proj").string();
  auto const unbound = (fx.root / "other").string();
  REQUIRE(dispatch(fx, {"assoc", "add", "acme", bound}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "other", unbound}).code == 0);

  // A project IS registered at `unbound` — it is just linked to `other`.
  auto const unlinked = dispatch(fx, {"assoc", "remove", "acme", unbound});
  CHECK(unlinked.code == 0);
  CHECK(unlinked.err.empty());
  CHECK(unlinked.out == std::format("removed project at {} from acme\n", unbound));

  // NOTHING MOVED. Both memberships survive the "successful" removal —
  // which is what makes the exit 0 above a defect rather than a shape.
  auto const acme = dispatch(fx, {"assoc", "members", "acme", "--json"});
  CHECK(acme.code == 0);
  CHECK(acme.out.contains(bound));
  CHECK_FALSE(acme.out.contains(unbound));

  auto const other = dispatch(fx, {"assoc", "members", "other", "--json"});
  CHECK(other.code == 0);
  CHECK(other.out.contains(unbound));
}

TEST_CASE("assoc remove matches root_path VERBATIM and does not canonicalise", "[cmd][assoc][remove]") {
  auto const fx = make_fixture("remove_verbatim");
  seed_db(fx);
  create_assoc(fx, "acme", "org");

  auto const path = (fx.root / "proj").string();
  REQUIRE(dispatch(fx, {"assoc", "add", "acme", path}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "members", "acme", "--json"}).out.contains(path));

  // A trailing slash names the SAME directory on the filesystem and a
  // DIFFERENT string in the column. `remove` compares strings, so it must
  // miss — a `std::filesystem::canonical` call here would make this
  // succeed, look like a hardening, and desynchronise `remove` from the
  // `add` that stored the key.
  auto const slashed = dispatch(fx, {"assoc", "remove", "acme", path + "/"});
  CHECK(slashed.code == 1);
  CHECK(slashed.err == std::format("error: no project registered at '{}/'\n", path));

  // The verbatim spelling still works, which is what makes the miss above
  // evidence about normalisation rather than about the verb being broken.
  auto const exact = dispatch(fx, {"assoc", "remove", "acme", path});
  CHECK(exact.code == 0);
  CHECK(dispatch(fx, {"assoc", "members", "acme", "--json"}).out == "[]\n");
}
