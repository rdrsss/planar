// @file completion.t.cpp
// @brief Unit tests for `planar.cli.completion::generate_script` (plan
// 996, task cpp-cli-tree-parity). Uses the same modeled `task`
// (add/done) subtree as parser.t.cpp/help.t.cpp; see parser.t.cpp's
// header comment for why these leaves were chosen.
#include <catch2/catch_test_macros.hpp>

import std;
import planar.cli;

namespace {

using planar::cli::cmd;
using planar::cli::flag;
using planar::cli::positional;

auto make_planar_root() -> cmd {
  cmd add{
      .name        = "add",
      .desc        = "Create a new task.",
      .flags       = {flag{.long_name = "--title", .short_name = 't'}},
      .positionals = {positional{.name = "title", .required = true}},
  };
  cmd done{
      .name        = "done",
      .desc        = "Mark a task as done (single-arg form; Go supports variadic).",
      .flags       = {flag{.long_name = "--force", .value_kind = planar::cli::kind::boolean}},
      .positionals = {positional{.name = "task-id", .required = true}},
  };
  cmd task{.name = "task", .desc = "Manage tasks.", .cmds = {std::move(add), std::move(done)}};
  return cmd{.name = "planar", .cmds = {std::move(task)}};
}

/// @brief Simulate a shell driving `<partial>` against the generated bash
/// script's "path" resolution: split `partial` on spaces and walk it the
/// same way the generated `for (( i=1; i<COMP_CWORD; i++ ))` loop does —
/// every token except a trailing partial-word fragment becomes part of
/// the resolved `path`. Exercises "completion output for a partial
/// input" against the SAME algorithm the generated script encodes,
/// without shelling an actual bash process.
auto resolve_bash_path(std::vector<std::string> const& words_before_cursor) -> std::string {
  std::string path;
  for (auto const& w : words_before_cursor) {
    if (!w.empty() && w[0] == '-') {
      continue;
    }
    if (!path.empty()) {
      path += " ";
    }
    path += w;
  }
  return path;
}

} // namespace

TEST_CASE("generate_script(bash): root case lists top-level commands and flags, keyed by empty path", "[completion][bash]") {
  auto       root   = make_planar_root();
  auto const script = planar::cli::generate_script(root, planar::cli::shell::bash);
  CHECK(script.find("_planar() {") != std::string::npos);
  CHECK(script.find("complete -F _planar planar") != std::string::npos);
  CHECK(script.find("cmds=\"task\"") != std::string::npos);
}

TEST_CASE("generate_script(bash): completion output for a partial input under a nested path", "[completion][bash][partial]") {
  auto       root   = make_planar_root();
  auto const script = planar::cli::generate_script(root, planar::cli::shell::bash);

  // Simulate the operator having typed `planar task ad<TAB>` — cursor word
  // "ad" is not yet part of `words_before_cursor` (COMP_WORDS[COMP_CWORD]
  // is the in-progress word; the loop only walks indices BEFORE it).
  auto const path = resolve_bash_path({"task"});
  REQUIRE(path == "task");

  // The generated script's `"task")` case must list "add" and "done" as
  // completions for that resolved path — `compgen -W "$cmds" -- "ad"`
  // would then filter that space-joined list down to "add" by prefix, the
  // same way a real bash completion driver does; the C++ tree-walk
  // producing "add done" as the case body's `cmds=` value is what we
  // assert against here (bash's own `compgen` prefix-matching is bash's
  // job, not this module's).
  auto const case_marker = std::string("\"") + path + "\")\n";
  auto const case_pos    = script.find(case_marker);
  REQUIRE(case_pos != std::string::npos);
  auto const cmds_pos = script.find("cmds=\"", case_pos);
  REQUIRE(cmds_pos != std::string::npos);
  auto const cmds_end   = script.find('"', cmds_pos + 6);
  auto const cmds_value = script.substr(cmds_pos + 6, cmds_end - (cmds_pos + 6));
  CHECK(cmds_value == "add done");

  auto const flags_pos = script.find("flags=\"", case_pos);
  REQUIRE(flags_pos != std::string::npos);
  auto const flags_end   = script.find('"', flags_pos + 7);
  auto const flags_value = script.substr(flags_pos + 7, flags_end - (flags_pos + 7));
  CHECK(flags_value.find("--help") != std::string::npos);
}

TEST_CASE("generate_script(bash): a leaf path's case lists its own flags including short forms", "[completion][bash]") {
  auto       root     = make_planar_root();
  auto const script   = planar::cli::generate_script(root, planar::cli::shell::bash);
  auto const case_pos = script.find("\"task add\")\n");
  REQUIRE(case_pos != std::string::npos);
  auto const flags_pos = script.find("flags=\"", case_pos);
  REQUIRE(flags_pos != std::string::npos);
  auto const flags_end   = script.find('"', flags_pos + 7);
  auto const flags_value = script.substr(flags_pos + 7, flags_end - (flags_pos + 7));
  CHECK(flags_value.find("--title -t") != std::string::npos);
}

TEST_CASE("generate_script(zsh): uses #compdef and _describe, with command descriptions", "[completion][zsh]") {
  auto       root   = make_planar_root();
  auto const script = planar::cli::generate_script(root, planar::cli::shell::zsh);
  CHECK(script.starts_with("#compdef planar\n"));
  CHECK(script.find("_describe -t commands") != std::string::npos);
  CHECK(script.find("\"task:Manage tasks.\"") != std::string::npos);
  CHECK(script.find("\"add:Create a new task.\"") != std::string::npos);
}

TEST_CASE("generate_script(fish): emits per-path complete lines gated by the resolved path helper", "[completion][fish]") {
  auto       root   = make_planar_root();
  auto const script = planar::cli::generate_script(root, planar::cli::shell::fish);
  CHECK(script.find("function __fish_planar_path") != std::string::npos);
  CHECK(script.find("__fish_planar_path \"\"") != std::string::npos);
  CHECK(script.find("-a 'task' -d 'Manage tasks.'") != std::string::npos);
  CHECK(script.find("__fish_planar_path \"task\"") != std::string::npos);
  CHECK(script.find("-a 'add' -d 'Create a new task.'") != std::string::npos);
  CHECK(script.find("-l 'title' -s t") != std::string::npos);
}
