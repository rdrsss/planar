/// @file resume.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.resume`.

module planar.cmd.planar.handlers.resume;

import std;
import cli11;
import planar.cliapp.args;
import planar.engine.runtime.resumecheck;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

namespace rck = engine::runtime::resumecheck;

namespace {

using kind_t = domain_error_kind;

} // namespace

auto resume_packet(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  static_cast<void>(ctx);
  static_cast<void>(args);
  // Registered on purpose so a dual group-and-leaf node fails LOUDLY
  // instead of falling through to dispatch's help page and exiting 0.
  // See this module's header for the captured oracle behavior.
  return std::unexpected(error_from_body(kind_t::not_implemented, "not implemented yet"));
}

auto resume_validate(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const raw     = positional_string(args, "task-id").value_or("");
  auto const task_id = cliapp::parse_int64_zig(raw);
  if (!task_id) {
    return std::unexpected(error_from_body(kind_t::invalid_input, std::format("task id must be an integer, got '{}'", raw)));
  }

  auto const checked = rck::validate(**conn, *task_id);
  if (!checked) {
    if (checked.error() == rck::resume_error::not_found) {
      // No payload at all on this path — stdout stays EMPTY, which is what
      // distinguishes "absent task" from "present but not resumable".
      return std::unexpected(error_from_body(kind_t::not_found, std::format("task {} not found", *task_id)));
    }
    return std::unexpected(error_from_body(kind_t::generic_failure, "resume validate: QueryFailed"));
  }

  // The payload is written FIRST and unconditionally. A non-resumable task
  // still emits its failure list and only then exits 1 — a caller that
  // reads stdout only on exit 0 would lose exactly the diagnosis it needs.
  if (flag_bool(args, "--json")) {
    ctx.out() << rck::render_validate_json(*checked);
  } else {
    ctx.out() << rck::render_validate_text(*checked);
  }

  if (!checked->resumable) {
    return std::unexpected(error_from_body(kind_t::not_found, std::format("task {} is not resumable", *task_id)));
  }
  return {};
}

} // namespace planar::cmd::handlers
