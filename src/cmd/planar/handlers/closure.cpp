/// @file closure.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.closure`.

module planar.cmd.planar.handlers.closure;

import std;
import cli11;
import planar.cliapp.args;
import planar.engine.closure;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

namespace store = engine::closure::store;

auto closure_show(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // The positional is declared a STRING and parsed here, not by CLI11, so the
  // refusal carries the oracle's own wording and its exit-2 bucket rather than
  // a generic parser type error at exit 1. `entity_id_arg` is not reused: its
  // message is `task id must be an integer, got 'x'` too, but it prefixes an
  // absent positional with `task id is required` where this leaf's renderer
  // owns the wording. Reading the renderer is the rule.
  auto const raw = cliapp::positional_string(args, "task-id").value_or(std::string{});
  auto const id  = cliapp::parse_int64_zig(raw);
  if (!id.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, store::render_invalid_task_id(raw)));
  }

  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const rows = store::show(**conn, *id);
  if (!rows) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "closure show: QueryFailed"));
  }

  // An unknown task id is an EMPTY result, never a not-found: the envelope
  // echoes the REQUESTED id straight from the argument. Both renderers return
  // complete payloads; this layer appends nothing.
  ctx.out() << (cliapp::flag_bool(args, "--json") ? store::render_show_json(*id, *rows) : store::render_show_text(*id, *rows));
  return {};
}

} // namespace planar::cmd::handlers
