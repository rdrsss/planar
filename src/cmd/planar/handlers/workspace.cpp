/// @file workspace.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.workspace`.

module planar.cmd.planar.handlers.workspace;

import std;
import planar.cli;
import planar.engine.workspace;
import planar.cmd.planar.args;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

namespace doctor = engine::workspace::doctor;

auto workspace_doctor(context& ctx, const cli::match_result& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // `ctx.env()` passes straight through: `planar.engine.workspace.identity`
  // declares the same `env_lookup` signature a `context` carries, which is
  // the whole point of modelling the environment as a callable rather than
  // a snapshot (see context.cppm). It matters more here than usual — see
  // this leaf's module header for where doctor actually writes.
  auto reports = doctor::run(**conn, ctx.env());
  if (!reports.has_value()) {
    // The Zig handler dies with `listing org associations failed: {s}`
    // interpolating `@errorName(e)`. The C++ `run` collapses every failure
    // to `nullopt`, and `listOrgs`'s only failure tag is `QueryFailed`, so
    // the tag is transcribed rather than captured — the path needs a
    // broken `associations` table to reach and no oracle probe produced it.
    return std::unexpected(
        error_from_body(cli::domain_error_kind::generic_failure, "listing org associations failed: QueryFailed"));
  }

  // Both renderers return COMPLETE payloads and this layer appends
  // nothing. They disagree on an empty database — `{"orgs":[]}\n` versus
  // zero bytes — and that disagreement is the oracle's, not a bug.
  ctx.out() << (flag_bool(args, "--json") ? doctor::doctor_json(*reports) : doctor::doctor_text(*reports));
  return {};
}

} // namespace planar::cmd::handlers
