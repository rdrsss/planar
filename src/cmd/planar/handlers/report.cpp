/// @file report.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.report`.
/// See report.cppm for the flag-validation contract and the privacy note.

module planar.cmd.planar.handlers.report;

import std;
import planar.cliapp.args;
import planar.db;
import planar.engine.config.effective;
import planar.engine.introspect;
import planar.engine.introspection_adapters;
import planar.cmd.planar.cli_log;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

namespace cfg   = engine::config;
namespace intro = engine::introspect;
namespace ia    = engine::introspection_adapters;

namespace {

/// @brief Resolve `~/.planar/config.toml` (or `$PLANAR_CONFIG_PATH`) into
/// the typed config, fail-open on every step. Mirrors `report.zig`'s
/// `resolveConfig`: a missing or unparseable file, or an unresolvable
/// path, all collapse to "no config" rather than refusing the verb — the
/// oracle's `catch return null`.
/// @param ctx The invocation context.
/// @return The resolved config, or unset on any failure.
auto resolved_config(context& ctx) -> std::optional<cfg::config> {
  auto const path = resolve_config_path(ctx.env());
  if (!path.has_value()) {
    return std::nullopt;
  }
  std::optional<std::string> content;
  if (std::ifstream file(*path, std::ios::binary); file) {
    content = std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  }
  auto const env = cfg::env_view::from_lookup(ctx.env());
  auto resolved = cfg::resolve(content.has_value() ? std::optional<std::string_view>{*content} : std::nullopt, env, std::nullopt);
  if (!resolved.has_value()) {
    return std::nullopt;
  }
  return resolved->cfg;
}

} // namespace

auto report(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const days = cliapp::flag_int(args, "--days").value_or(30);
  if (days <= 0) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, std::format("--days must be a positive integer (got {})", days)));
  }
  auto const tail = cliapp::flag_int(args, "--tail").value_or(20);
  if (tail <= 0) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, std::format("--tail must be a positive integer (got {})", tail)));
  }

  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  db::connection& db_conn = **conn;

  // Fail-open: an unreadable/missing/unparseable config resolves to "no
  // config", which reads as `cli_log = false` — the embedded default —
  // exactly like the oracle's `resolveConfig` catching every failure arm.
  auto const cfg_result      = resolved_config(ctx);
  bool const logging_enabled = cfg_result.has_value() && cfg_result->introspection.cli_log;

  auto bundle = intro::build(db_conn, days, tail, logging_enabled, ctx.db_path().string());
  if (!bundle.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "building report: QueryFailed"));
  }

  auto const home = ctx.env()("HOME").value_or("");

  // Per-vendor transcript locations. Assembled from the resolved config
  // directly (see report.cppm/introspection_adapters.cppm's "WHY NO
  // `collect_configured_preview`" for why this handler does the anytype
  // -equivalent glue by hand rather than the module importing
  // `engine_config` itself).
  ia::transcript_config transcripts{.home_dir = home};
  if (cfg_result.has_value()) {
    auto const& t               = cfg_result->introspection.transcripts;
    transcripts.claude_enabled  = t.claude_enabled;
    transcripts.claude_path     = t.claude_path;
    transcripts.codex_enabled   = t.codex_enabled;
    transcripts.codex_path      = t.codex_path;
    transcripts.copilot_enabled = t.copilot_enabled;
    transcripts.copilot_path    = t.copilot_path;
  }

  // Bridges `engine::introspect::cli_preview_jsonl` (the authoritative,
  // structurally-redacted `cli_invocations` reader) into the adapter's
  // read-only boundary. Mirrors report.zig's `readCliPreview` closure over
  // its `CliContext`.
  ia::cli_log_adapter const cli_adapter{
      .enabled = logging_enabled,
      .read    = [&db_conn, days](std::size_t max_bytes) -> ia::cli_read_result {
        auto jsonl = intro::cli_preview_jsonl(db_conn, days, max_bytes);
        if (!jsonl.has_value()) {
          return ia::cli_read_result{.status = ia::cli_read_status::failed};
        }
        return ia::cli_read_result{.status = ia::cli_read_status::ok, .bytes = std::move(*jsonl)};
      },
  };

  bundle->preview = ia::collect_preview_from_paths(transcripts, cli_adapter);

  ctx.out() << (cliapp::flag_bool(args, "--json") ? intro::render_json(*bundle) : intro::render_text(*bundle));
  return {};
}

} // namespace planar::cmd::handlers
