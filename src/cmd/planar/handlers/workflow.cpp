/// @file workflow.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.workflow`.

module planar.cmd.planar.handlers.workflow;

import std;
import cli11;
import planar.cliapp.args;
import planar.engine.workflows;
import planar.cliapp.args;
import planar.process;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers {

namespace catalog = engine::workflows::catalog;
namespace render  = engine::workflows::render;

namespace {

/// @brief Locate the `planar-execute` binary.
///
/// Probes in the oracle's order, first hit wins:
///   1. `$PLANAR_EXECUTE_BIN`, read through `env` (an EMPTY value does not
///      count as a hit — the oracle's `if (v.len > 0)`).
///   2. A sibling of `argv[0]`, i.e. the installed-case
///      `~/.planar/bin/planar-execute`. Only when it is actually there.
///   3. The bare name `planar-execute`, left for `PATH` resolution.
///
/// Step 3 is a bare name rather than a refusal on purpose: the oracle
/// returns one and lets the spawn fail, so "not installed" surfaces as a
/// spawn failure at the call site rather than a different error here.
/// @param ctx The invocation context, for `env()` and `argv()`.
/// @return The program to exec.
auto resolve_execute_bin(const context& ctx) -> std::string {
  if (auto const override_bin = ctx.env()("PLANAR_EXECUTE_BIN"); override_bin.has_value() && !override_bin->empty()) {
    return *override_bin;
  }
  auto const argv = ctx.argv();
  if (!argv.empty()) {
    std::filesystem::path const self{argv[0]};
    if (self.has_parent_path()) {
      auto            candidate = self.parent_path() / "planar-execute";
      std::error_code ec;
      if (std::filesystem::exists(candidate, ec)) {
        return candidate.string();
      }
    }
  }
  return "planar-execute";
}

} // namespace

auto workflow_list(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const local_only = flag_bool(args, "--local");
  auto const dirs       = catalog::resolve_dirs(ctx.env());
  auto const entries    = catalog::list(dirs, local_only);

  // Both renderers return the COMPLETE stdout payload. `list_json` returns
  // an EMPTY string for an empty catalog and that is the correct output —
  // appending anything here (even a newline) is a parity break the
  // renderer cannot compensate for.
  ctx.out() << (flag_bool(args, "--json") ? render::list_json(entries) : render::list_text(entries, local_only));
  return {};
}

auto workflow_show(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const name = positional_string(args, "name").value_or(std::string{});
  auto const dirs = catalog::resolve_dirs(ctx.env());
  auto const hit  = catalog::find(dirs, name);
  if (!hit.has_value()) {
    // `not_found_error` returns the whole line — `error: ` prefix and
    // trailing newline included — so it is a RENDERED payload, not a
    // message body. Wrapping it with `error_from_body` would emit
    // `error: error: workflow 'nope' not found`.
    return std::unexpected(error_from_rendered(domain_error_kind::generic_failure, render::not_found_error(name)));
  }
  ctx.out() << (flag_bool(args, "--json") ? render::entry_json(*hit) : render::show_text(*hit));
  return {};
}

auto workflow_run(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const name = positional_string(args, "name").value_or(std::string{});
  auto const dirs = catalog::resolve_dirs(ctx.env());

  // `--local` restricts resolution to the sandbox; otherwise shipped wins
  // on a name collision. Both are `catalog::find`/`list`'s behaviour, shared
  // with `workflow show` rather than reimplemented here.
  std::optional<catalog::entry> hit;
  if (flag_bool(args, "--local")) {
    for (auto const& candidate : catalog::list(dirs, true)) {
      if (catalog::effective_name(candidate) == name) {
        hit = candidate;
        break;
      }
    }
  } else {
    hit = catalog::find(dirs, name);
  }
  if (!hit.has_value()) {
    // The SAME complete line `workflow show nope` emits — one function,
    // byte-identical between the two leaves including the single quotes.
    return std::unexpected(error_from_rendered(domain_error_kind::generic_failure, render::not_found_error(name)));
  }

  // planar-execute run <path> --phase <phase> [--args <json>] [--worktree
  // <dir>] [--sandbox-root <dir>]. Each optional flag is forwarded only
  // when non-empty, matching the oracle's `if (args.X.len > 0)` — passing
  // an empty `--args` would be a different invocation.
  std::vector<std::string> argv{resolve_execute_bin(ctx), "run", hit->path, "--phase",
                                flag_string(args, "--phase").value_or(std::string{})};
  for (auto const* flag : {"--args", "--worktree", "--sandbox-root"}) {
    if (auto const value = flag_string(args, flag); value.has_value() && !value->empty()) {
      argv.emplace_back(flag);
      argv.emplace_back(*value);
    }
  }

  // Inherited stdio: the workflow's result JSON streams straight through
  // and planar-execute's diagnostics keep their own voice. `run_inherited`
  // flushes this process's buffers before forking.
  auto const code = process::run_inherited(ctx.env(), argv);
  if (!code.has_value()) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("spawning planar-execute: {}", argv.front())));
  }
  if (*code == 0) {
    return {};
  }
  // Exit status propagated EXACTLY, and nothing written to stderr: the
  // child already said whatever it had to say, in its own voice. An empty
  // `rendered` payload is written verbatim, i.e. not at all.
  auto failed             = error_from_rendered(domain_error_kind::generic_failure, std::string{});
  failed.passthrough_code = *code;
  return std::unexpected(std::move(failed));
}

/// @brief Declare the `workflow` group and its three leaves.
///
/// THE LAST PARTIAL FOLD, closed. `list` and `show` were
/// hand-written in `tree.cpp` since task 6105 while `run` came from a
/// `node_spec`; the two hand-written halves agreed with their generated
/// twins field for field. Folding all three together is what keeps the
/// sibling order correct as ONE contiguous list and is why no
/// child-level ordering anchor was ever needed here.
///
/// `run` shells `planar-execute` rather than executing a workflow
/// itself; see the handler for the resolution order.
auto declare_workflow(CLI::App& root) -> void {
  CLI::App* workflow = root.add_subcommand(
      "workflow", "Enumerate, inspect, and invoke shipped and sandbox Lua workflows.\n\n  Shipped workflows live at "
                  "$PLANAR_HOME/workflows/ (default\n  ~/.planar/workflows/).  Sandbox workflows live at\n  "
                  "~/.planar/local/workflows/ and are marked `local`.\n\n  These commands are READ-ONLY w.r.t. SQLite.  `run` "
                  "delegates\n  execution to `planar-execute` and forwards its output + exit code.");
  workflow->require_subcommand(0);

  CLI::App* list = workflow->add_subcommand("list", "List shipped and sandbox workflows.");
  add_bool(*list, "--local");
  add_json(*list);

  CLI::App* show = workflow->add_subcommand("show", "Show @meta and source path for a named workflow.");
  add_json(*show);
  add_positional(*show, "name");

  CLI::App* run = workflow->add_subcommand(
      "run", "Resolve <name> across shipped and sandbox workflows, then exec\n  `planar-execute run <path> --phase <phase> "
             "[--args <json>]\n  [--worktree <dir>] [--sandbox-root <dir>]`.  The workflow's\n  flow.result JSON streams to "
             "stdout; the exit code is forwarded\n  exactly (non-zero on flow.fail or engine error).\n\n  planar-execute "
             "resolution order: $PLANAR_EXECUTE_BIN →\n  sibling of argv[0] → PATH.");
  add_string_required(*run, "--phase", "Phase function to invoke inside the workflow.");
  add_string(*run, "--args", "JSON args blob forwarded to planar-execute --args.");
  add_string(*run, "--worktree", "Worktree directory forwarded to planar-execute --worktree.");
  add_string(*run, "--sandbox-root", "Sandbox root forwarded to planar-execute --sandbox-root.");
  add_bool(*run, "--local", "Restrict resolution to sandbox (local) workflows only.");
  add_positional(*run, "name");
}

} // namespace planar::cmd::handlers
