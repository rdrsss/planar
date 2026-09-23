// @file worktree_gate.t.cpp
// @brief Tests for `planar.cmd.planar.verb_classification` and
// `planar.cmd.planar.worktree_gate` (plan 996, task 6137).
//
// DB SAFETY. Every context here is built over an explicit env map and a
// scratch database path under a per-test temp directory. The refusal cases
// return BEFORE any handler runs and therefore before any database is
// opened at all — that is the gate's whole contract.
//
// WHY THE END-TO-END CASES BUILD A REAL WORKTREE. The defect this module
// closes is "exit 0 with a real row written, in the exact situation the
// gate exists to refuse". A test that stubs detection proves the wiring and
// nothing about the classification of an actual `git worktree add` — which
// is where the two rules that were previously WRONG live (see
// planar.git's header on `--path-format=absolute` and on submodules).
//
// Include-before-import is deliberate (see db/db.t.cpp).
#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.surface;
import planar.cmd.planar.main;
import planar.cmd.planar.verb_classification;
import planar.cmd.planar.worktree_gate;

namespace {

namespace gate = planar::cmd::worktree_gate;

using planar::cmd::classify;
using planar::cmd::context;
using planar::cmd::verb_class;

/// @brief `classify` over a brace-initialised token list.
/// @param tokens The verb path.
/// @return The bucket.
auto cls(std::vector<std::string> tokens) -> verb_class {
  return classify(tokens);
}

/// @brief A unique scratch directory tree, removed on scope exit.
struct scratch_dir {
  std::filesystem::path path_;

  scratch_dir()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_wtgate_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
    std::filesystem::create_directories(path_);
  }

  scratch_dir(const scratch_dir&)                        = delete;
  auto operator=(const scratch_dir&) -> scratch_dir&     = delete;
  scratch_dir(scratch_dir&&) noexcept                    = delete;
  auto operator=(scratch_dir&&) noexcept -> scratch_dir& = delete;

  ~scratch_dir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  /// @brief The directory's path.
  /// @return The path.
  [[nodiscard]] auto get() const -> const std::filesystem::path& {
    return path_;
  }
};

/// @brief Run a fixture shell line inside `dir`.
/// @param dir The working directory.
/// @param line The shell line.
/// @return True when it exited 0.
auto fixture_sh(const std::filesystem::path& dir, std::string_view line) -> bool {
  std::string const composed = std::format("cd '{}' && {} >/dev/null 2>&1", dir.string(), line);
  return std::system(composed.c_str()) == 0;
}

/// @brief Is `git` runnable on this machine?
/// @return True when `git --version` exits 0.
auto have_git() -> bool {
  static bool const answer = std::system("git --version >/dev/null 2>&1") == 0;
  return answer;
}

/// @brief What one gated `dispatch::run` produced.
struct invocation {
  int         code = 0; ///< The exit code.
  std::string err;      ///< Everything written to stderr.
};

/// @brief Run the real tree, the real handler table and the real gate over
/// `args`, from `cwd`.
/// @param cwd The directory the invocation claims to run from.
/// @param args The argv tail.
/// @param vars The environment.
/// @return The captured invocation.
auto dispatch_from(const std::filesystem::path& cwd, std::vector<std::string> args,
                   std::map<std::string, std::string, std::less<>> vars = {}) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv), planar::cmd::map_env(std::move(vars)), cwd, std::make_shared<planar::cmd::database>(cwd / "planar.db", err), out, err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::make_handler_table(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .err = err.str()};
}

} // namespace

// ---------------------------------------------------------------------------
// verb_classification
// ---------------------------------------------------------------------------

TEST_CASE("classify: an empty path defaults to planning", "[cmd][worktree-gate]") {
  CHECK(classify({}) == verb_class::planning);
}

TEST_CASE("classify: an unknown top-level verb defaults to planning", "[cmd][worktree-gate]") {
  // The safe default. Allowing unknowns would silently let a newly-ported
  // planning verb through the gate, which is the failure mode task 6137
  // exists to close.
  CHECK(cls({"never-heard-of-this"}) == verb_class::planning);
}

TEST_CASE("classify: top-level read verbs are execution_or_read", "[cmd][worktree-gate]") {
  for (auto const& v :
       {"resume", "dashboard", "health", "tree", "search", "version", "completion", "import", "synthesize", "explore"}) {
    INFO(v);
    CHECK(cls({v}) == verb_class::execution_or_read);
  }
}

TEST_CASE("classify: report and schema are execution_or_read", "[cmd][worktree-gate]") {
  // Both are regression guards carried over from the oracle. `report` is
  // run from worktrees by the introspector agent; `schema` emits a static
  // catalog and opens no database, and the cli-usage and coverage gates
  // shell out to it — refusing either false-fails tooling.
  CHECK(cls({"report"}) == verb_class::execution_or_read);
  CHECK(cls({"schema"}) == verb_class::execution_or_read);
}

TEST_CASE("classify: bench and workflow are execution_or_read", "[cmd][worktree-gate]") {
  // `bench` is the measurement rig the harness drives from inside
  // worktrees by design; `workflow` is a read-only filesystem scan.
  CHECK(cls({"bench"}) == verb_class::execution_or_read);
  CHECK(cls({"bench", "harvest"}) == verb_class::execution_or_read);
  CHECK(cls({"workflow"}) == verb_class::execution_or_read);
  CHECK(cls({"workflow", "validate"}) == verb_class::execution_or_read);
}

TEST_CASE("classify: entity-group reads pass and writes are planning", "[cmd][worktree-gate]") {
  CHECK(cls({"plan", "show"}) == verb_class::execution_or_read);
  CHECK(cls({"plan", "list"}) == verb_class::execution_or_read);
  CHECK(cls({"plan", "next"}) == verb_class::execution_or_read);
  CHECK(cls({"plan", "recommend-strategy"}) == verb_class::execution_or_read);
  CHECK(cls({"plan", "divergence"}) == verb_class::execution_or_read);
  CHECK(cls({"plan", "create"}) == verb_class::planning);
  CHECK(cls({"plan", "update"}) == verb_class::planning);
  CHECK(cls({"task", "add"}) == verb_class::planning);
  CHECK(cls({"question", "add"}) == verb_class::planning);
  CHECK(cls({"decision", "add"}) == verb_class::planning);
  CHECK(cls({"artifact", "update"}) == verb_class::planning);
  CHECK(cls({"assoc", "add"}) == verb_class::planning);
}

TEST_CASE("classify: task done is planning", "[cmd][worktree-gate]") {
  // Deliberate even though it reads terminal: coders must go through
  // `planar-agent complete`, which is what keeps the claim and the status
  // flip in one transaction.
  CHECK(cls({"task", "done"}) == verb_class::planning);
}

TEST_CASE("classify: task touches splits by leaf", "[cmd][worktree-gate]") {
  CHECK(cls({"task", "touches", "list"}) == verb_class::execution_or_read);
  CHECK(cls({"task", "touches", "add"}) == verb_class::planning);
  CHECK(cls({"task", "touches", "remove"}) == verb_class::planning);
}

TEST_CASE("classify: feedback triage splits by leaf", "[cmd][worktree-gate]") {
  CHECK(cls({"feedback", "triage", "list"}) == verb_class::execution_or_read);
  CHECK(cls({"feedback", "triage", "show"}) == verb_class::execution_or_read);
  CHECK(cls({"feedback", "triage", "set"}) == verb_class::planning);
}

TEST_CASE("classify: workbench reads and sync pass, other subverbs are planning", "[cmd][worktree-gate]") {
  for (auto const& v : {"pull", "push", "status", "sync", "resolve"}) {
    INFO(v);
    CHECK(cls({"workbench", v}) == verb_class::execution_or_read);
  }
  CHECK(cls({"workbench", "edit"}) == verb_class::planning);
  CHECK(cls({"workbench"}) == verb_class::planning);
}

TEST_CASE("classify: ext propagate is planning and ext status is a read", "[cmd][worktree-gate]") {
  CHECK(cls({"ext", "propagate"}) == verb_class::planning);
  CHECK(cls({"ext", "status"}) == verb_class::execution_or_read);
}

TEST_CASE("classify: annotate, init, promote, demote, link, unlink, links are planning", "[cmd][worktree-gate]") {
  CHECK(cls({"annotate", "add"}) == verb_class::planning);
  for (auto const& v : {"init", "promote", "demote", "link", "unlink"}) {
    INFO(v);
    CHECK(cls({v}) == verb_class::planning);
  }
  CHECK(cls({"links", "add"}) == verb_class::planning);
}

TEST_CASE("classify: read groups pass wholesale", "[cmd][worktree-gate]") {
  for (auto const& v : {"handoff", "capture", "audit", "workspace", "config", "models", "templates", "scope", "doc", "local",
                        "skills", "test-spec", "sync"}) {
    INFO(v);
    CHECK(cls({v}) == verb_class::execution_or_read);
    CHECK(cls({v, "show"}) == verb_class::execution_or_read);
  }
}

// ---------------------------------------------------------------------------
// resolve_verb_path
// ---------------------------------------------------------------------------

TEST_CASE("resolve_verb_path walks the real tree to a leaf", "[cmd][worktree-gate]") {
  auto const                     tree = planar::cmd::root_app();
  std::vector<std::string> const argv{"planar", "plan", "create", "some title"};
  auto const                     path = gate::resolve_verb_path(*tree, argv);
  REQUIRE(path.size() == 2);
  CHECK(path[0] == "plan");
  CHECK(path[1] == "create");
}

TEST_CASE("resolve_verb_path does not mistake a flag VALUE for a subcommand", "[cmd][worktree-gate]") {
  // The reason the walk consults the tree for flag arity instead of just
  // skipping `-`-prefixed tokens: `--title plan` would otherwise resolve a
  // second path token of `plan` and change the classification.
  auto const                     tree = planar::cmd::root_app();
  std::vector<std::string> const argv{"planar", "task", "--plan", "list", "add"};
  auto const                     path = gate::resolve_verb_path(*tree, argv);
  REQUIRE(!path.empty());
  CHECK(path[0] == "task");
  // `list` was consumed as `--plan`'s value, so it must NOT appear as the
  // resolved leaf — which would have downgraded this to a read.
  CHECK(std::ranges::find(path, "list") == path.end());
}

TEST_CASE("resolve_verb_path stops at a `--` terminator and at a positional", "[cmd][worktree-gate]") {
  auto const tree = planar::cmd::root_app();
  {
    std::vector<std::string> const argv{"planar", "plan", "--", "create"};
    auto const                     path = gate::resolve_verb_path(*tree, argv);
    REQUIRE(path.size() == 1);
    CHECK(path[0] == "plan");
  }
  {
    std::vector<std::string> const argv{"planar", "nosuchverb", "plan"};
    CHECK(gate::resolve_verb_path(*tree, argv).empty());
  }
  {
    std::vector<std::string> const argv{"planar"};
    CHECK(gate::resolve_verb_path(*tree, argv).empty());
  }
}

TEST_CASE("resolve_verb_path treats an inline --flag=value as carrying no second token", "[cmd][worktree-gate]") {
  auto const                     tree = planar::cmd::root_app();
  std::vector<std::string> const argv{"planar", "task", "--plan=7", "touches", "add"};
  auto const                     path = gate::resolve_verb_path(*tree, argv);
  REQUIRE(path.size() == 3);
  CHECK(path[2] == "add");
}

// ---------------------------------------------------------------------------
// argv_requests_help / refusal_message
// ---------------------------------------------------------------------------

TEST_CASE("argv_requests_help sees help before a terminator and not after", "[cmd][worktree-gate]") {
  CHECK(gate::argv_requests_help(std::vector<std::string>{"planar", "plan", "--help"}));
  CHECK(gate::argv_requests_help(std::vector<std::string>{"planar", "plan", "-h"}));
  CHECK_FALSE(gate::argv_requests_help(std::vector<std::string>{"planar", "plan", "--", "--help"}));
  CHECK_FALSE(gate::argv_requests_help(std::vector<std::string>{"planar", "plan", "create"}));
}

TEST_CASE("refusal_message renders the four labelled lines verbatim", "[cmd][worktree-gate]") {
  // Pinned exactly, not as a `contains` check: the labels AND the column
  // alignment are the tech spec's contract, and this block was captured
  // byte-for-byte from the oracle running against a real linked worktree.
  auto const got = gate::refusal_message("plan create", "/w/linked", "/w/main");
  CHECK(got == "error: planning verb 'plan create' may not run from inside a worktree\n"
               "  cwd:        /w/linked\n"
               "  parent:     /w/main\n"
               "  reason:     Worktrees are for code execution, not for planning the work itself.\n"
               "  suggestion: cd /w/main and re-run.\n");
}

// ---------------------------------------------------------------------------
// check, end to end through dispatch::run against a REAL worktree
// ---------------------------------------------------------------------------

TEST_CASE("the gate refuses a planning verb from a real linked worktree", "[cmd][worktree-gate]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir const root;
  auto const        main_repo = root.get() / "main";
  auto const        linked    = root.get() / "linked";
  std::filesystem::create_directories(main_repo);
  REQUIRE(fixture_sh(main_repo, "git init -q -b main"));
  REQUIRE(fixture_sh(main_repo, "git config user.email planar@example.invalid"));
  REQUIRE(fixture_sh(main_repo, "git config user.name Planar"));
  REQUIRE(fixture_sh(main_repo, "git commit -q --allow-empty -m seed"));
  REQUIRE(fixture_sh(main_repo, std::format("git worktree add -q -b side '{}'", linked.string())));

  auto const refused = dispatch_from(linked, {"plan", "create", "should-be-refused"});
  CHECK(refused.code == gate::exit_code_worktree_refusal);
  CHECK(refused.code == 8);
  CHECK(refused.err.contains("may not run from inside a worktree"));
  CHECK(refused.err.contains("  cwd:        "));
  CHECK(refused.err.contains("  parent:     "));
  CHECK(refused.err.contains("  reason:     "));
  CHECK(refused.err.contains("  suggestion: "));
  // The parent named in the block is the repository, not the worktree.
  CHECK(refused.err.contains(std::filesystem::canonical(main_repo).string()));

  // `--scope` does NOT override the refusal: the rule is about WHERE the
  // verb runs, not which scope it targets.
  auto const scoped = dispatch_from(linked, {"plan", "create", "x", "--scope", "anything"});
  CHECK(scoped.code == 8);

  // A read from the same worktree is allowed through the gate. It may fail
  // downstream for its own reasons, but it must not be REFUSED — anything
  // other than 8 proves the gate let it past.
  CHECK(dispatch_from(linked, {"plan", "list", "--json"}).code != 8);

  // Help is exempt for every verb, planning included.
  CHECK(dispatch_from(linked, {"plan", "--help"}).code == 0);

  // And the same planning verb from the MAIN checkout is not refused.
  CHECK(dispatch_from(main_repo, {"plan", "create", "allowed"}).code != 8);
}

TEST_CASE("the gate refuses BEFORE the parser, so a bad flag still exits 8", "[cmd][worktree-gate]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir const root;
  auto const        main_repo = root.get() / "main";
  auto const        linked    = root.get() / "linked";
  std::filesystem::create_directories(main_repo);
  REQUIRE(fixture_sh(main_repo, "git init -q -b main"));
  REQUIRE(fixture_sh(main_repo, "git config user.email planar@example.invalid"));
  REQUIRE(fixture_sh(main_repo, "git config user.name Planar"));
  REQUIRE(fixture_sh(main_repo, "git commit -q --allow-empty -m seed"));
  REQUIRE(fixture_sh(main_repo, std::format("git worktree add -q -b side '{}'", linked.string())));

  // Ordering contract: gating after the parse would report the parse error
  // and leave the operator to fix their flags, cd, and only THEN discover
  // the refusal. The oracle exits 8 here and so must this.
  auto const got = dispatch_from(linked, {"plan", "create", "x", "--no-such-flag"});
  CHECK(got.code == 8);
}

TEST_CASE("the gate refuses on the .worktrees convention path with no git involved", "[cmd][worktree-gate]") {
  scratch_dir const root;
  auto const        conventional = root.get() / ".worktrees" / "plan-996" / "task-6137";
  std::filesystem::create_directories(conventional);

  // No repository anywhere. The orchestrator's convention classifies on
  // the path alone, so the gate fires without a subprocess.
  auto const got = dispatch_from(conventional, {"task", "add", "x", "--plan", "1"});
  CHECK(got.code == 8);
  CHECK(got.err.contains("planning verb 'task add' may not run from inside a worktree"));
  CHECK(got.err.contains((root.get() / ".worktrees" / "plan-996").string()));
}

TEST_CASE("the gate allows an ordinary directory that is not a repository", "[cmd][worktree-gate]") {
  scratch_dir const root;
  // Detection is total: git answering nothing must never refuse a planning
  // verb out from under an operator.
  CHECK(dispatch_from(root.get(), {"plan", "create", "x"}).code != 8);
}

TEST_CASE("the env bypass is honoured only where it was compiled in", "[cmd][worktree-gate]") {
  scratch_dir const root;
  auto const        conventional = root.get() / ".worktrees" / "seg";
  std::filesystem::create_directories(conventional);

  // Baseline: refused with no bypass in the environment.
  CHECK(dispatch_from(conventional, {"plan", "create", "x"}).code == 8);

  auto const refused_values = std::vector<std::string>{"", "0"};
  for (auto const& value : refused_values) {
    INFO("PLANAR_DISABLE_WORKTREE_GATE=" << value);
    // Empty, and a value STARTING with '0', both read as "not set" — the
    // oracle's rule, and what lets a scenario un-set the harness default by
    // passing an empty string.
    CHECK(dispatch_from(conventional, {"plan", "create", "x"}, {{"PLANAR_DISABLE_WORKTREE_GATE", value}}).code == 8);
  }

  auto const bypassed = dispatch_from(conventional, {"plan", "create", "x"}, {{"PLANAR_DISABLE_WORKTREE_GATE", "1"}});
  if (gate::bypass_compiled_in()) {
    // This test binary is built from the `debug` preset, which sets
    // -DPLANAR_TEST_BINARY=ON precisely so the parity harness can suppress
    // the gate for fixture paths that are not about it.
    CHECK(bypassed.code != 8);
  } else {
    // A production build: the branch is `if constexpr`-dead and the
    // environment cannot defeat the gate at all.
    CHECK(bypassed.code == 8);
  }
}
