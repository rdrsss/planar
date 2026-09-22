// @file golden.t.cpp
// @brief Golden `flow.result` fixtures for every shipped workflow and phase
// (plan 1033 M0, task 6484).
//
// Harness in `../parity_harness.hpp`.
//
// ## What this pins, and why now
//
// Decision 1007 moves workflow execution to Centurion; plan 1033 M3 re-runs
// every shipped workflow through a Lua prelude over Centurion's `command.exec`
// and must produce the SAME `flow.result` document the embedded runner does.
// "The same" is only checkable if the embedded runner's output was recorded
// before anything changed, so this file records it: one case per shipped
// `(workflow, phase)`, run by the BUILT `planar-execute` against a scratch
// database seeded by the named seed below, with the normalized stdout
// committed under `golden/` and diffed byte for byte.
//
// The bytes are the contract, including the ones that look accidental. An
// empty Lua table renders as `{}` even where the workflow means an empty
// array (`waves`' `serialized`, `capacity_reconcile`'s `retryable`); object
// keys are sorted; integers stay integers. A prelude that emits `[]` there
// is a regression this file catches, not a cleanup.
//
// ## The seed is named and versioned: `golden-v1`
//
// Built through the CLI, never raw SQL, in a fixed order so every id is
// known: plan 1 `golden` (active) with tasks 1-4 `alpha`..`delta`, each
// touching its own `src/<x>.cpp` in repo `proj`, and task 4 `depends-on`
// task 1 — which gives `plan` a three-lane fan-out with one serialized
// task and `waves` a real second wave. Plan 2 `closable` (active) holds one
// task driven to `done`, so `finalize_closeout` has a ready plan beside the
// not-ready plan 1. A git repository under the arena, committed with fixed
// identity and dates, is the `--worktree` for `bench_run_ritual`. CHANGING
// THE SEED CHANGES EVERY FIXTURE: bump the name and regenerate rather than
// editing it in place, so a diff against an old fixture is never read as a
// behaviour change.
//
// ## Cases run in order, in ONE arena
//
// `finalize_closeout`'s ready case closes plan 2 and `bench_run_ritual`'s
// `setup` opens a run that `measure` finishes, so the table is a sequence,
// not a set. Nothing earlier in it mutates what a later case reads except
// those two deliberate pairs.
//
// ## Normalization
//
// Exactly two substitutions, both for values the seed cannot fix: the arena
// root (a timestamped temp path) becomes `@ARENA@`, and the 32-hex `run_uid`
// `planar run start` mints becomes `@RUN_UID@`. Nothing else is masked.
//
// ## Regenerating
//
// `PLANAR_GOLDEN_UPDATE=1` rewrites every fixture from the current binary and
// then FAILS the case, so an update run can never be mistaken for a pass.
// Review the resulting `git diff` of `golden/` as a behaviour change.

#include <catch2/catch_test_macros.hpp>

import std;

#include "parity_harness.hpp"

namespace {

using planar::cmd::parity::capture;
using planar::cmd::parity::make_arena;
using planar::cmd::parity::read_all;
using planar::cmd::parity::run_pinned;
using planar::cmd::parity::shell_quote;

/// @brief The seed every fixture was captured against. See the file header.
constexpr std::string_view k_seed = "golden-v1";

/// @brief Path to the built `planar-execute` (set by this target's CMakeLists).
/// @return The path.
auto execute_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

/// @brief Path to the built `planar` used to seed (set by src/cmd/CMakeLists).
/// @return The path.
auto operator_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_OPERATOR_CPP_BIN};
}

/// @brief One shipped `(workflow, phase)` run and the fixture it is diffed against.
struct golden_case {
  std::string_view fixture;  ///< File name under `golden/`, without `.json`.
  std::string_view workflow; ///< File name under `workflows/`.
  std::string_view phase;    ///< The `--phase` value.
  std::string      args;     ///< The `--args` JSON; empty means no `--args`.
  bool             worktree; ///< Pass the seeded git repository as `--worktree`.
};

/// @brief Run one seeding step and fail the case if it did not succeed.
/// @param root The arena root.
/// @param args The `planar` arguments.
/// @param tag The capture discriminator.
void seed_step(std::filesystem::path const& root, std::vector<std::string> const& args, std::string const& tag) {
  auto const cap = run_pinned(operator_bin(), args, root, tag);
  INFO("seed step " << tag << " stderr: " << cap.err);
  REQUIRE(cap.code == 0);
}

/// @brief Run a shell line and fail the case unless it exits 0.
/// @param line The `/bin/sh` command line.
void sh(std::string const& line) {
  INFO("shell: " << line);
  REQUIRE(std::system(line.c_str()) == 0);
}

/// @brief Build the `golden-v1` seed. See the file header for its shape.
/// @param root The arena root.
/// @return The seeded git repository used as `--worktree`, and its base commit.
auto seed_golden_v1(std::filesystem::path const& root) -> std::pair<std::filesystem::path, std::string> {
  auto const proj = (root / "proj").string();
  seed_step(root, {"init"}, "s00");
  seed_step(root, {"assoc", "create", "project:proj", "--kind", "project"}, "s01");
  seed_step(root, {"assoc", "add", "project:proj", proj}, "s02");
  seed_step(root, {"plan", "create", "Golden plan", "--slug", "golden"}, "s03");
  constexpr std::array<std::string_view, 4> k_tasks{"alpha", "bravo", "charlie", "delta"};
  for (std::size_t i = 0; i < k_tasks.size(); ++i) {
    auto const slug = std::string{k_tasks[i]};
    seed_step(root, {"task", "add", slug, "--plan", "1", "--slug", slug, "--editor=false"}, std::format("s1{}", i));
    seed_step(root, {"task", "touches", "add", std::to_string(i + 1), "proj", "--path", std::format("src/{}.cpp", slug[0])},
              std::format("s2{}", i));
  }
  seed_step(root, {"links", "add", "task:4", "task:1", "--relationship", "depends-on"}, "s30");
  seed_step(root, {"plan", "update", "1", "--status", "active"}, "s31");
  seed_step(root, {"plan", "create", "Closable plan", "--slug", "closable"}, "s40");
  seed_step(root, {"task", "add", "only", "--plan", "2", "--slug", "only", "--editor=false"}, "s41");
  seed_step(root, {"plan", "update", "2", "--status", "active"}, "s42");
  seed_step(root, {"task", "update", "5", "--status", "doing"}, "s43");
  seed_step(root, {"task", "done", "5"}, "s44");

  // Fixed identity, dates and config so the base commit is the same bytes on
  // every machine; the operator's global git config is shut out entirely.
  auto const repo = root / "repo";
  std::filesystem::create_directories(repo / "src");
  for (char const c : std::string_view{"abcd"}) {
    std::ofstream(repo / "src" / std::format("{}.cpp", c), std::ios::binary) << c << "\n";
  }
  std::string const git_env = "env GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1 GIT_AUTHOR_NAME=golden "
                              "GIT_AUTHOR_EMAIL=golden@example.invalid GIT_COMMITTER_NAME=golden "
                              "GIT_COMMITTER_EMAIL=golden@example.invalid GIT_AUTHOR_DATE=2000-01-01T00:00:00Z "
                              "GIT_COMMITTER_DATE=2000-01-01T00:00:00Z";
  auto const        q       = shell_quote(repo.string());
  sh(std::format("cd {} && {} git init -q -b main . && {} git add . && {} git commit -q -m base", q, git_env, git_env, git_env));
  sh(std::format("cd {} && git rev-parse HEAD > ../base_sha", q));
  auto sha = read_all(root / "base_sha");
  sha      = sha.substr(0, sha.find('\n'));
  REQUIRE(sha.size() == 40);
  return {repo, sha};
}

/// @brief Replace every occurrence of `from` in `text` with `to`.
/// @param text The text to rewrite in place.
/// @param from The needle; must be non-empty.
/// @param to The replacement.
void replace_all(std::string& text, std::string_view from, std::string_view to) {
  for (auto pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos + to.size())) {
    text.replace(pos, from.size(), to);
  }
}

/// @brief Apply the two substitutions the file header names, and no others.
/// @param out The raw stdout.
/// @param root The arena root.
/// @return The normalized stdout.
auto normalize(std::string out, std::filesystem::path const& root) -> std::string {
  replace_all(out, root.string(), "@ARENA@");
  static std::regex const k_run_uid{R"("run_uid":"[0-9a-f]{32}")"};
  return std::regex_replace(out, k_run_uid, R"("run_uid":"@RUN_UID@")");
}

/// @brief The `phases:` list a workflow's `@meta` block declares.
/// @param source The workflow source.
/// @return The declared phase names, in declaration order.
auto declared_phases(std::string_view source) -> std::vector<std::string> {
  std::vector<std::string> phases;
  auto const               start = source.find("\nphases:");
  if (start == std::string_view::npos) {
    return phases;
  }
  auto line       = source.substr(start + 8);
  line            = line.substr(0, line.find('\n'));
  std::size_t pos = 0;
  while (pos <= line.size()) {
    auto const comma = line.find(',', pos);
    auto       item  = line.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
    while (!item.empty() && item.front() == ' ') {
      item.remove_prefix(1);
    }
    while (!item.empty() && item.back() == ' ') {
      item.remove_suffix(1);
    }
    if (!item.empty()) {
      phases.emplace_back(item);
    }
    if (comma == std::string_view::npos) {
      break;
    }
    pos = comma + 1;
  }
  return phases;
}

/// @brief Every shipped `(workflow, phase)` case, in the order they must run.
/// @param repo The seeded `--worktree` repository.
/// @param base_sha Its base commit.
/// @return The case table.
auto golden_cases(std::filesystem::path const& repo, std::string const& base_sha) -> std::vector<golden_case> {
  auto const pd = std::string_view{"parallel-dispatch.lua"};
  return {
      {"example.setup", "example.lua", "setup", "", false},
      {"parallel-dispatch.plan", pd, "plan", R"({"plan_id":1})", false},
      {"parallel-dispatch.cycle_plan", pd, "cycle_plan", R"({"plan_id":1,"task_id":4})", false},
      {"parallel-dispatch.waves", pd, "waves", R"({"plan_id":1})", false},
      {"parallel-dispatch.barrier_check", pd, "barrier_check",
       R"({"plan_id":1,"lanes":[{"task_id":1,"landed":true,"fanned_in":true},{"task_id":2,"landed":true,"fanned_in":false},{"task_id":3,"landed":false,"fanned_in":false}]})",
       false},
      {"parallel-dispatch.fan_in", pd, "fan_in",
       R"({"plan_id":1,"lanes":[{"task_id":3,"branch":"cycle/p1/charlie","worktree":".worktrees/cycle/p1/charlie","wave":1},{"task_id":4,"branch":"cycle/p1/delta","worktree":".worktrees/cycle/p1/delta","wave":2},{"task_id":1,"branch":"cycle/p1/alpha","worktree":".worktrees/cycle/p1/alpha","wave":1},{"task_id":2,"branch":"cycle/p1/bravo","worktree":".worktrees/cycle/p1/bravo","wave":1,"outcome":"failed"}]})",
       false},
      {"parallel-dispatch.conflict_escalation", pd, "conflict_escalation",
       R"({"plan_id":1,"ours":"cycle/p1/alpha","theirs":"cycle/p1/bravo","paths":["src/z.cpp","src/a.cpp"]})", false},
      {"parallel-dispatch.reconcile_plan", pd, "reconcile_plan",
       R"({"plan_id":1,"lanes":[{"task_id":1,"outcome":"landed"},{"task_id":2,"outcome":"failed_clean","branch":"cycle/p1/bravo"},{"task_id":3,"outcome":"abandoned","branch":"cycle/p1/charlie","worktree":".worktrees/cycle/p1/charlie"}]})",
       false},
      {"parallel-dispatch.capacity_reconcile", pd, "capacity_reconcile",
       R"({"plan_id":1,"lanes":[{"task_id":1,"provider":"claude","outcome":"landed"},{"task_id":2,"provider":"claude","outcome":"failed_clean","category":"usage_limit"},{"task_id":3,"provider":"codex","outcome":"abandoned"},{"task_id":4,"provider":"claude","outcome":"pending"}]})",
       false},
      // Task 2 carries neither branch nor worktree: the phase skips absent
      // fields rather than defaulting them, and that is pinned here.
      {"parallel-dispatch.teardown", pd, "teardown",
       R"({"plan_id":1,"lanes":[{"task_id":1,"branch":"cycle/p1/alpha","worktree":".worktrees/cycle/p1/alpha"},{"task_id":2}],"epic_branch":"epic/p1-golden"})",
       false},
      {"finalize_closeout.closeout.not_ready", "finalize_closeout.lua", "closeout", R"({"plan_id":1})", false},
      {"finalize_closeout.closeout.ready", "finalize_closeout.lua", "closeout", R"({"plan_id":2})", false},
      {"bench_run_ritual.setup", "bench_run_ritual.lua", "setup",
       std::format(R"({{"run_uid":"golden-run-1","plan_id":1,"base_sha":"{}","config_hash":"cfg-1","tasks":[1,2]}})", base_sha),
       true},
      {"bench_run_ritual.measure", "bench_run_ritual.lua", "measure",
       std::format(R"({{"run_uid":"golden-run-1","worktree":"{}","tasks":[1,2]}})", repo.string()), true},
  };
}

} // namespace

TEST_CASE("planar-execute golden: every shipped workflow phase matches its committed flow.result fixture",
          "[cmd][execute][golden][6484]") {
  auto const space            = make_arena("golden");
  auto const root             = space.cpp_root;
  auto const [repo, base_sha] = seed_golden_v1(root);

  std::filesystem::path const golden_dir{PLANAR_GOLDEN_DIR};
  std::filesystem::path const workflows_dir{PLANAR_WORKFLOWS_DIR};
  char const*                 update_env = std::getenv("PLANAR_GOLDEN_UPDATE");
  bool const                  update     = update_env != nullptr && *update_env != '\0' && *update_env != '0';

  std::size_t n = 0;
  for (auto const& c : golden_cases(repo, base_sha)) {
    INFO("fixture: " << c.fixture << " (seed " << k_seed << ")");
    // bench_run_ritual's setup must visibly RESET the worktree, and measure
    // must have edits to harvest: the caller's LLM step between the two
    // phases is simulated by writing to the tracked files.
    if (c.fixture == "bench_run_ritual.setup") {
      std::ofstream(repo / "src" / "a.cpp", std::ios::binary) << "dirty before setup\n";
    }
    if (c.fixture == "bench_run_ritual.measure") {
      std::ofstream(repo / "src" / "a.cpp", std::ios::binary) << "edited\n";
      std::ofstream(repo / "src" / "b.cpp", std::ios::binary) << "edited\n";
    }

    std::vector<std::string> args{"run", (workflows_dir / c.workflow).string(), "--phase", std::string{c.phase}};
    if (!c.args.empty()) {
      args.insert(args.end(), {"--args", c.args});
    }
    if (c.worktree) {
      args.insert(args.end(), {"--worktree", repo.string()});
    }
    auto const cap = run_pinned(execute_bin(), args, root, std::format("g{:02}", n++));
    INFO("stderr: " << cap.err);
    REQUIRE(cap.code == 0);
    auto const actual = normalize(cap.out, root);

    auto const path = golden_dir / std::format("{}.json", c.fixture);
    if (update) {
      std::ofstream(path, std::ios::binary) << actual;
      continue;
    }
    REQUIRE(std::filesystem::exists(path));
    CHECK(actual == read_all(path));

    if (c.fixture == "bench_run_ritual.setup") {
      // Not vacuous: git.reset_hard really ran in the seeded worktree.
      CHECK(read_all(repo / "src" / "a.cpp") == "a\n");
    }
  }
  if (update) {
    FAIL("PLANAR_GOLDEN_UPDATE is set: fixtures rewritten from the current binary. Review `git diff` and re-run without it.");
  }
}

TEST_CASE("planar-execute golden: the fixture table covers exactly the phases each workflow declares",
          "[cmd][execute][golden][6484]") {
  // Completeness by construction rather than by review: a phase added to a
  // workflow's `@meta` without a golden case fails here, and so does a case
  // (or a `@meta` entry) naming a phase that does not exist — which is what
  // `planar workflow show` prints to an operator.
  std::filesystem::path const                  workflows_dir{PLANAR_WORKFLOWS_DIR};
  std::map<std::string, std::set<std::string>> covered;
  for (auto const& c : golden_cases("/unused", std::string(40, '0'))) {
    covered[std::string{c.workflow}].insert(std::string{c.phase});
  }

  std::size_t shipped = 0;
  for (auto const& entry : std::filesystem::directory_iterator{workflows_dir}) {
    if (entry.path().extension() != ".lua") {
      continue;
    }
    ++shipped;
    auto const name   = entry.path().filename().string();
    auto const source = read_all(entry.path());
    auto const phases = declared_phases(source);
    INFO("workflow: " << name);
    REQUIRE_FALSE(phases.empty());
    std::set<std::string> const declared{phases.begin(), phases.end()};
    CHECK(declared == covered[name]);
    for (auto const& phase : declared) {
      INFO("phase: " << phase);
      // The phase is a real top-level function, not just a header entry.
      CHECK(source.contains(std::format("\nfunction {}()", phase)));
    }
  }
  CHECK(shipped == covered.size());
}
