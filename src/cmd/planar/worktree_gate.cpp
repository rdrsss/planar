/// @file worktree_gate.cpp
/// @brief Implementation of `planar.cmd.planar.worktree_gate`. See the
/// module interface for the ordering, the exit code, and the two-layer
/// bypass.

module;

#include <CLI/CLI.hpp>

module planar.cmd.planar.worktree_gate;

import std;
import cli11;
import planar.cliapp.walk;
import planar.git;
import planar.cmd.planar.context;
import planar.cmd.planar.verb_classification;

namespace planar::cmd::worktree_gate {

namespace {

/// @brief Layer 1 of the bypass: compiled in only for a build configured
/// with `-DPLANAR_TEST_BINARY=ON`. In a production build every use of this
/// constant is `if constexpr`-dead, so the environment cannot defeat the
/// gate — defeating it requires deliberate intent by whoever builds.
#ifdef PLANAR_TEST_BINARY
constexpr bool k_bypass_compiled_in = true;
#else
constexpr bool k_bypass_compiled_in = false;
#endif

/// @brief Does `app` answer to `token`, by name or by alias?
/// @param app The candidate subcommand.
/// @param token The argv token.
/// @return True on a match.
auto command_matches(const CLI::App& app, std::string_view token) -> bool {
  if (app.get_name() == token) {
    return true;
  }
  auto const& aliases = app.get_aliases();
  return std::ranges::find(aliases, token) != aliases.end();
}

/// @brief Does the flag named `token` take a value at `app`'s scope (or an
/// ancestor's)?
///
/// Conservative by design: an unknown flag is assumed to take a value so a
/// value token is never mistaken for a subcommand. The parser rejects the
/// unknown flag properly a moment later.
/// @param app The current node.
/// @param token The flag-shaped argv token.
/// @return True when a following token should be consumed as its value.
auto flag_accepts_value(const CLI::App& app, std::string_view token) -> bool {
  // `--name=value` carries its value inline, so there is no second token.
  if (token.find('=') != std::string_view::npos) {
    return false;
  }
  for (const CLI::App* node = &app; node != nullptr; node = node->get_parent()) {
    for (const CLI::Option* opt : node->get_options()) {
      auto const& names  = opt->get_lnames();
      auto const& snames = opt->get_snames();
      bool        hit    = std::ranges::any_of(names, [&](const std::string& n) { return token == "--" + n; });
      if (!hit) {
        hit = std::ranges::any_of(snames, [&](const std::string& n) { return token == "-" + n; });
      }
      if (hit) {
        // `get_expected_max() == 0` is how this tree already spells "bool
        // flag" (see planar.cliapp.schema).
        return opt->get_expected_max() != 0;
      }
    }
  }
  return true;
}

} // namespace

auto bypass_compiled_in() -> bool {
  return k_bypass_compiled_in;
}

auto argv_requests_help(std::span<const std::string> argv) -> bool {
  for (auto const& token : argv) {
    if (token == "--") {
      return false;
    }
    if (token == "--help" || token == "-h") {
      return true;
    }
  }
  return false;
}

auto resolve_verb_path(const CLI::App& root, std::span<const std::string> argv) -> std::vector<std::string> {
  std::vector<std::string> path;
  if (argv.size() < 2) {
    return path;
  }
  const CLI::App* current = &root;
  for (std::size_t i = 1; i < argv.size(); ++i) {
    std::string_view const token = argv[i];
    if (token.empty()) {
      continue;
    }
    if (token == "--") {
      break;
    }
    if (token.front() == '-') {
      if (flag_accepts_value(*current, token)) {
        ++i;
      }
      continue;
    }
    const CLI::App* matched = nullptr;
    for (const CLI::App* child : cliapp::children(*current)) {
      if (command_matches(*child, token)) {
        matched = child;
        break;
      }
    }
    if (matched == nullptr) {
      break; // First non-subcommand token ends the path.
    }
    path.emplace_back(matched->get_name());
    current = matched;
  }
  return path;
}

auto refusal_message(std::string_view verb, std::string_view worktree_root, std::string_view parent_repo_root) -> std::string {
  // Labels and column alignment are verbatim from the tech spec and are
  // asserted by the scenario suite. Do not reflow.
  return std::format("error: planning verb '{}' may not run from inside a worktree\n"
                     "  cwd:        {}\n"
                     "  parent:     {}\n"
                     "  reason:     Worktrees are for code execution, not for planning the work itself.\n"
                     "  suggestion: cd {} and re-run.\n",
                     verb, worktree_root, parent_repo_root, parent_repo_root);
}

auto check(context& ctx, const CLI::App& root) -> std::optional<int> {
  // Layer 2 of the bypass, reachable only when layer 1 compiled it in.
  if constexpr (k_bypass_compiled_in) {
    auto const raw = ctx.env()("PLANAR_DISABLE_WORKTREE_GATE");
    if (raw.has_value() && !raw->empty() && raw->front() != '0') {
      return std::nullopt;
    }
  }

  if (argv_requests_help(ctx.argv())) {
    return std::nullopt;
  }

  auto const path = resolve_verb_path(root, ctx.argv());
  if (path.empty()) {
    // Nothing matched — let the parser surface its own error rather than
    // refusing an invocation that may not name a verb at all.
    return std::nullopt;
  }
  if (classify(path) == verb_class::execution_or_read) {
    return std::nullopt;
  }

  // A planning verb. `detect_worktree` is total: any failure reports "not
  // a worktree", so a machine without git, or a cwd outside any
  // repository, never has a planning verb refused out from under it.
  auto const det = git::detect_worktree(ctx.cwd());
  if (!det.is_worktree) {
    return std::nullopt;
  }

  std::string verb;
  for (auto const& token : path) {
    if (!verb.empty()) {
      verb += ' ';
    }
    verb += token;
  }
  ctx.err() << refusal_message(verb, det.worktree_root.value_or(ctx.cwd().string()), det.parent_repo_root.value_or("(unknown)"));
  return exit_code_worktree_refusal;
}

} // namespace planar::cmd::worktree_gate
