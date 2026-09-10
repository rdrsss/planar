// @file completion.t.cpp
// @brief Tests for `planar.cliapp.completion` — the shell-completion
// generator, rebuilt over `CLI::App` (plan 996, task 6123).
//
// ## THE COMPLETION VERDICT
//
// CLI11 2.7.2 ships NO completion facility of any kind — no bash/zsh/fish
// emitter, no `__complete` protocol hook. Its entire introspection surface
// is the `get_*` accessors this generator walks. So "adopt CLI11's
// completion" was never an option to weigh: the choice was KEEP the
// generator (retargeted from the deleted `cli::cmd` tree onto `CLI::App`)
// or DROP `planar-watch completion` as a verb. It was kept.
//
// NOTHING was lost in the swap that was not already deferred. The
// predecessor (`planar.cli.completion`) had already dropped flag-VALUE
// completion (etcli-zig's `values` / `files` / `directories` / `dynamic`
// metadata and its `__complete` sub-invocation) as out of scope, and its
// generated script was explicitly NOT oracle-byte-comparable — its own
// CMakeLists said so and no test claimed otherwise. This port emits the
// same script shape from the same information.
//
// These tests therefore assert the MECHANISM the generator exists to
// preserve — "strip flags from the current tokens, join the remainder into
// a path string, and switch on that path to decide what to suggest" — plus
// the per-shell syntax that mechanism is expressed in. They do not pin the
// whole script byte-for-byte: it is generated boilerplate whose exact
// spacing carries no contract, and a full-text pin would fail on every
// cosmetic edit while catching nothing a targeted assertion misses.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.completion;

namespace {

using planar::cliapp::generate_script;
using planar::cliapp::shell;

/// @brief A two-level tree with an inherited flag, a hidden subcommand, a
/// short option, and a described flag — every shape the generator branches
/// on.
/// @param app The root app to populate.
auto build_tree(CLI::App& app) -> void {
  app.require_subcommand(0);
  app.add_flag("--verbose")->description("Chatty output");

  CLI::App* workflow = app.add_subcommand("workflow", "Discover and run workflows.");
  workflow->require_subcommand(0);
  CLI::App* list = workflow->add_subcommand("list", "List them.");
  list->add_flag("--json");
  list->add_flag("-l,--local")->description("Sandbox only");

  CLI::App* secret = app.add_subcommand("secret", "Hidden group");
  secret->group("");
  secret->add_flag("--dangerous");
}

/// @brief Build the fixture and generate for `sh`.
/// @param sh The target shell.
/// @return The generated script.
auto script_for(shell sh) -> std::string {
  CLI::App app{"Planning CLI.", "planar"};
  build_tree(app);
  return generate_script(app, sh);
}

} // namespace

TEST_CASE("bash: the generated script switches on a flag-stripped path", "[cliapp][completion][bash]") {
  auto const script = script_for(shell::bash);
  // The durable mechanism: walk the tokens, skip anything dash-leading,
  // join the rest, `case` on it. If this shape goes, the generator has
  // stopped doing the one thing it exists to do.
  CHECK(script.contains("for (( i=1; i<COMP_CWORD; i++ )); do"));
  CHECK(script.contains("            -*) ;;"));
  CHECK(script.contains("    case \"$path\" in"));
  CHECK(script.contains("complete -F _planar planar\n"));
}

TEST_CASE("bash: every visible node gets a case arm, at every depth", "[cliapp][completion][bash]") {
  auto const script = script_for(shell::bash);
  CHECK(script.contains("        \"\")\n            cmds=\"workflow\"\n"));
  CHECK(script.contains("        \"workflow\")\n            cmds=\"list\"\n"));
  CHECK(script.contains("        \"workflow list\")\n"));
}

TEST_CASE("bash: a leaf offers its own flags PLUS its ancestors'", "[cliapp][completion][bash]") {
  auto const script = script_for(shell::bash);
  // `--verbose` is declared on the ROOT and must be offered under
  // `workflow list` too — the inherited-flag walk. `-h`/`--help` are
  // appended unconditionally because CLI11 adds them to every node.
  CHECK(script.contains("        \"workflow list\")\n"
                        "            cmds=\"\"\n"
                        "            flags=\"--verbose --json --local -l --help -h\"\n"));
}

TEST_CASE("bash: a hidden subtree is offered nowhere", "[cliapp][completion][bash]") {
  // Offering an unreachable verb would be worse than offering nothing:
  // the operator tab-completes it and gets an unknown-subcommand error.
  auto const script = script_for(shell::bash);
  CHECK_FALSE(script.contains("secret"));
  CHECK_FALSE(script.contains("--dangerous"));
}

TEST_CASE("zsh: descriptions ride along, and colons are escaped", "[cliapp][completion][zsh]") {
  auto const script = script_for(shell::zsh);
  CHECK(script.starts_with("#compdef planar\n"));
  CHECK(script.contains("cmds=(\"workflow:Discover and run workflows.\")"));
  CHECK(script.contains("_describe -t commands 'commands' cmds"));
  // zsh's `name:description` separator means an unescaped colon inside a
  // description silently truncates it.
  CLI::App colon_app{"", "tool"};
  colon_app.require_subcommand(0);
  colon_app.add_subcommand("run", "Usage: run it");
  auto const colon_script = generate_script(colon_app, shell::zsh);
  CHECK(colon_script.contains(R"("run:Usage\: run it")"));
}

TEST_CASE("fish: one completion line per node, gated on the resolved path", "[cliapp][completion][fish]") {
  auto const script = script_for(shell::fish);
  CHECK(script.contains("function __fish_planar_path\n"));
  CHECK(script.contains("complete -c planar -n '__fish_planar_path \"\"' -f -a 'workflow' -d 'Discover and run workflows.'\n"));
  CHECK(script.contains("complete -c planar -n '__fish_planar_path \"workflow\"' -f -a 'list' -d 'List them.'\n"));
  // A flag line strips the leading dashes (fish's `-l` wants the bare
  // name) and carries the short form separately.
  CHECK(script.contains("complete -c planar -n '__fish_planar_path \"workflow list\"' -f -l 'local' -s l -d 'Sandbox only'\n"));
}

TEST_CASE("break-probe: the three shells produce genuinely different scripts", "[cliapp][completion][break-probe]") {
  // A generator that ignored its `shell` argument would satisfy every
  // `contains` assertion above that happens to be shell-agnostic. These
  // three must be mutually distinct and each non-empty.
  auto const bash = script_for(shell::bash);
  auto const zsh  = script_for(shell::zsh);
  auto const fish = script_for(shell::fish);
  CHECK_FALSE(bash.empty());
  CHECK(bash != zsh);
  CHECK(zsh != fish);
  CHECK(bash != fish);
}

TEST_CASE("break-probe: dropping a flag changes every script", "[cliapp][completion][break-probe]") {
  // The assertions above are `contains` checks, which pass against a
  // superset. This is the complement: a REMOVED flag must be observable.
  CLI::App with{"", "tool"};
  with.require_subcommand(0);
  CLI::App* run_with = with.add_subcommand("run", "Run it");
  run_with->add_flag("--json");
  run_with->add_flag("--dry-run");

  CLI::App without{"", "tool"};
  without.require_subcommand(0);
  CLI::App* run_without = without.add_subcommand("run", "Run it");
  run_without->add_flag("--json");

  for (auto const sh : {shell::bash, shell::zsh, shell::fish}) {
    CHECK(generate_script(with, sh) != generate_script(without, sh));
  }
}
