/// @file workflow.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.workflow`.

module planar.cmd.planar.handlers.workflow;

import std;
import cli11;
import planar.cliapp.args;
import planar.engine.workflows;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

namespace catalog = engine::workflows::catalog;
namespace render  = engine::workflows::render;

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

} // namespace planar::cmd::handlers
