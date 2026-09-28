/// @file src/cmd/planar/handlers/test_spec/command.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.test_spec`. See
/// test_spec.cppm for why a real milestone plan reports "not found".

module planar.cmd.planar.handlers.test_spec;

import std;
import planar.cliapp.args;
import planar.engine.planning.test_spec_status;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import cli11;
import planar.cmd.planar.declare;
import planar.cmd.planar.handlers.test_spec.status;

namespace planar::cmd::handlers {

namespace tss_ns = engine::planning::test_spec_status;

auto test_spec_status(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const plan_arg = cliapp::positional_string(args, "plan").value_or(std::string{});

  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const anchor = tss_ns::fetch_anchor(**conn, plan_arg);
  if (!anchor) {
    // Both failure arms land on the same wording and the same exit 1. The
    // oracle distinguishes them (`plan '<a>' not found` vs `lookup plan
    // '<a>': <ErrorName>`), but only `not_found` is reachable: the query
    // failure arm needs a broken schema, not a bad argument.
    if (anchor.error() == tss_ns::test_spec_error::not_found) {
      return std::unexpected(error_from_body(domain_error_kind::not_found, tss_ns::render_not_found(plan_arg)));
    }
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("lookup plan '{}': QueryFailed", plan_arg)));
  }

  auto const report = tss_ns::compute(**conn, anchor->id);
  if (!report) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "computing test-spec status: QueryFailed"));
  }

  ctx.out() << (cliapp::flag_bool(args, "--json") ? tss_ns::render_json(*report) : tss_ns::render_text(anchor->slug, *report));
  return {};
}

/// @brief Declare the `test-spec` group and its `status` leaf.
auto declare_test_spec(CLI::App& root) -> void {
  CLI::App* test_spec = root.add_subcommand(
      "test-spec", "Commands for inspecting test-spec coverage of a plan's tasks.\n\n  'test-spec status' prints a per-milestone "
                   "breakdown of which tasks\n  have verifying scenarios. This is a read-only complement to the\n  ingest-time "
                   "coverage gate (see `planar spec ingest --strict`).");
  test_spec->require_subcommand(0);

  test_spec_cli::attach_status(test_spec);
}

} // namespace planar::cmd::handlers
