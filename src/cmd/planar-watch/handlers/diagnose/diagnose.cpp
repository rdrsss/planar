/// @file diagnose.cpp
/// @brief Implementation of `planar.cmd.planar_watch.handlers.diagnose`.

module planar.cmd.planar_watch.handlers.diagnose;

import std;
import planar.cliapp.args;
import planar.cmd.internal.config_path;
import planar.db;
import planar.engine.config.effective;
import planar.engine.diagnose;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.exit;
import planar.cmd.planar_watch.handler;
import planar.cmd.planar_watch.handlers.format;

namespace planar::cmd::watch::handlers {

namespace {

/// @brief `[introspection].cli_log` as the config file resolves it. A missing file is the embedded
/// default (off); no resolvable path or an unparseable file is unknown, which the run reports as an
/// unavailable capture-log input rather than a guess.
/// @param ctx The invocation context.
/// @return The setting, or empty when it cannot be determined.
auto cli_log_setting(context& ctx) -> std::optional<bool> {
  auto const path = internal::resolve_config_path(ctx.env());
  if (!path.has_value()) {
    return std::nullopt;
  }
  std::optional<std::string> content;
  if (std::ifstream file(*path, std::ios::binary); file) {
    content = std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  }
  // `cli_log` is resolved without the environment (the capture path reads it the same way).
  auto const resolved = engine::config::resolve(content.has_value() ? std::optional<std::string_view>{*content} : std::nullopt,
                                                engine::config::env_view::empty(), std::nullopt);
  if (!resolved.has_value()) {
    return std::nullopt;
  }
  return resolved->cfg.introspection.cli_log;
}

} // namespace

auto diagnose(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return diagnose_with_catalog(ctx, args, engine::diagnose::builtin_catalog());
}

auto diagnose_with_catalog(context& ctx, const cliapp::parsed_args& args, const engine::diagnose::catalog& cat)
    -> handler_result {
  namespace dg = planar::engine::diagnose;

  auto const days = cliapp::flag_int(args, "--days");
  if (days.has_value() && (*days < 1 || *days > std::numeric_limits<int>::max())) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, std::format("diagnose: --days must be at least 1, got {}", *days)));
  }

  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  // The viewer's whole contract: this handle cannot write. SQLite's own answer is asked, not
  // the flag the connection was opened with.
  if (!(*conn)->is_write_protected()) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "diagnose: refusing a writable database handle"));
  }

  dg::run_request request{.plan_id         = cliapp::flag_int(args, "--plan"),
                          .days            = days ? std::optional<int>{static_cast<int>(*days)} : std::nullopt,
                          .checks          = cliapp::flag_strings(args, "--check"),
                          .evaluated_at    = format::now_iso(),
                          .cli_log_enabled = cli_log_setting(ctx)};

  auto diagnosis = dg::run(**conn, request, cat);
  if (!diagnosis) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, std::format("diagnose: {}", diagnosis.error().message)));
  }

  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << dg::render_json(*diagnosis) << '\n';
  } else {
    ctx.out() << dg::render_text(*diagnosis);
  }
  if (diagnosis->result == dg::run_outcome::unavailable) {
    // The run did not complete. The result is already on stdout; exit 1 with nothing more to say.
    return std::unexpected(error_from_rendered(domain_error_kind::generic_failure, ""));
  }
  return {};
}

} // namespace planar::cmd::watch::handlers
