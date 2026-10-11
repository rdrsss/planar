/// @file brief.cpp
/// @brief Implementation of `planar.cmd.planar_agent.handlers.brief`.

module planar.cmd.planar_agent.handlers.brief;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.internal.config_path;
import planar.engine.config;
import planar.cmd.planar_agent.brief;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.exit;
import planar.cmd.planar_agent.handler;
import planar.cmd.planar_agent.handlers.support;

namespace planar::cmd::agent::handlers {

namespace {

namespace cfg = engine::config;

/// @brief The longest a statement waits for a competing lock.
constexpr int k_busy_ms = 5000;

/// @brief The user's config file, flattened, plus the reason it could not be used.
struct config_load {
  cfg::toml_map              file;    ///< The flattened document; empty for no file or an unusable one.
  std::optional<std::string> problem; ///< Why the file was not used; unset when it was absent or loaded.
};

/// @brief Read and parse the config file the context's environment names.
///
/// An unresolvable path or a missing file gives the defaults without a problem. A file that is not
/// a regular file, cannot be read or does not parse gives the defaults and a problem.
/// @param ctx The invocation context.
/// @return The document and the problem, if any.
auto load_config(context& ctx) -> config_load {
  config_load out;
  auto const  path = internal::resolve_config_path(ctx.env());
  if (!path.has_value()) {
    return out;
  }
  std::error_code ec;
  if (!std::filesystem::exists(*path, ec)) {
    return out;
  }
  if (!std::filesystem::is_regular_file(*path, ec)) {
    out.problem = std::format("config path is not a regular file: {}", path->string());
    return out;
  }
  std::ifstream in{*path, std::ios::binary};
  if (!in) {
    out.problem = std::format("cannot read config file: {}", path->string());
    return out;
  }
  std::string const content{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
  auto              parsed = cfg::parse_toml(content);
  if (!parsed.has_value()) {
    out.problem =
        std::format("line {}: col {}: TOML parse error: {}", parsed.error().line, parsed.error().column, parsed.error().message);
    return out;
  }
  out.file = std::move(*parsed);
  return out;
}

} // namespace

auto brief(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const plan_id = cliapp::positional_int(args, "plan-id").value_or(0);
  if (plan_id <= 0) {
    return std::unexpected(invalid_input_error(std::format("brief: invalid plan id {}: expected a positive integer", plan_id)));
  }

  auto conn = database_policy::open_read_only(ctx.db().path(), ctx.err(), k_busy_ms);
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const config   = load_config(ctx);
  auto       gathered = gather_brief(*conn, plan_id, config.file);
  if (!gathered) {
    if (gathered.error() == brief_error::plan_not_found) {
      auto err = error_from_body(domain_error_kind::generic_failure, std::format("brief: plan {} not found", plan_id));
      err.tag  = "PlanNotFound";
      return std::unexpected(std::move(err));
    }
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("brief: could not read plan {}", plan_id)));
  }
  if (config.problem.has_value()) {
    gathered->policy.valid = false;
    gathered->policy.findings.insert(gathered->policy.findings.begin(),
                                     cfg::policy_finding{.key = "config", .message = *config.problem});
  }

  ctx.out() << (cliapp::flag_bool(args, "--json") ? render_brief_json(*gathered) : render_brief_text(*gathered));
  return {};
}

} // namespace planar::cmd::agent::handlers
