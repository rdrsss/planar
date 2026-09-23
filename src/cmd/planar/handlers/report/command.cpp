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
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers {

namespace cfg   = engine::config;
namespace intro = engine::introspect;
namespace ia    = engine::introspection_adapters;

namespace {

/// @brief Resolve `~/.planar/config.toml` (or `$PLANAR_CONFIG_PATH`) into
/// the typed config. Mirrors `report.zig`'s `resolveConfig` exactly,
/// including its caller: a missing config FILE is fail-open (no file means
/// `content == nullopt`, which `cfg::resolve` treats as "fall through to
/// embedded defaults" and succeeds), but an unresolvable path (no `HOME`)
/// or an unparseable file that DOES exist both make `resolveConfig` itself
/// return `null` — and `report.zig:98` reads that as `orelse
/// exit.die(ctx, error.InvalidConfig, "resolving report config", .{})`,
/// i.e. the verb refuses rather than silently treating a malformed config
/// as "no config". This helper reproduces that refusal instead of
/// swallowing it: on either failure it returns the domain error the
/// caller propagates, exit 1, body "resolving report config" — verified
/// byte-for-byte against the oracle under a scratch `$HOME` with a
/// malformed `config.toml`.
/// @param ctx The invocation context.
/// @return The resolved config, or the refusal to propagate.
auto resolved_config(context& ctx) -> std::expected<cfg::config, domain_error> {
  auto const path = resolve_config_path(ctx.env());
  if (!path.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "resolving report config"));
  }
  std::optional<std::string> content;
  if (std::ifstream file(*path, std::ios::binary); file) {
    content = std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  }
  auto const env = cfg::env_view::from_lookup(ctx.env());
  auto resolved = cfg::resolve(content.has_value() ? std::optional<std::string_view>{*content} : std::nullopt, env, std::nullopt);
  if (!resolved.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "resolving report config"));
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

  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  db::connection& db_conn = **conn;

  // Only a MISSING config file is fail-open (falls through to the embedded
  // default, `cli_log = false`). An unresolvable path or an unparseable
  // file that DOES exist refuses the verb — see `resolved_config`'s header
  // for the oracle citation (`report.zig:98`'s `orelse exit.die(...)`).
  auto cfg_result = resolved_config(ctx);
  if (!cfg_result.has_value()) {
    return std::unexpected(cfg_result.error());
  }
  bool const logging_enabled = cfg_result->introspection.cli_log;

  auto bundle = intro::build(db_conn, days, tail, logging_enabled, ctx.db_path().string());
  if (!bundle.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "building report: QueryFailed"));
  }

  auto const home = ctx.env()("HOME").value_or("");

  // Per-vendor transcript locations. Assembled from the resolved config
  // directly (see report.cppm/introspection_adapters.cppm's "WHY NO
  // `collect_configured_preview`" for why this handler does the anytype
  // -equivalent glue by hand rather than the module importing
  // `engine_config` itself). Guarded below (task 6356) so a future field on
  // either struct that this hand-copy forgets fails LOUDLY at build time
  // instead of being silently dropped.
  ia::transcript_config transcripts{.home_dir = home};
  {
    auto const& t               = cfg_result->introspection.transcripts;
    transcripts.claude_enabled  = t.claude_enabled;
    transcripts.claude_path     = t.claude_path;
    transcripts.codex_enabled   = t.codex_enabled;
    transcripts.codex_path      = t.codex_path;
    transcripts.copilot_enabled = t.copilot_enabled;
    transcripts.copilot_path    = t.copilot_path;

    // `t` and `transcripts` are two SEPARATE structs (`engine_config` and
    // `engine_introspection_adapters` are both layer 2, and this handler
    // exists specifically to bridge them by hand rather than adding the
    // engine<->engine edge D15/D18/D20 forbid — see the header above and
    // task 6351/decisions 981-982). Nothing but this block keeps their
    // field lists in sync, and the compiler cannot check a hand-written
    // assignment list against "every field got copied": a sixth field
    // added to either struct compiles clean and is silently dropped here.
    //
    // TASK 6356'S GUARD: aggregate structured-binding decomposition arity
    // IS checked by the compiler, so it stands in for that missing check —
    // a structured binding is well-formed only when the aggregate has
    // EXACTLY as many public, non-static data members as names given. If
    // either struct's field count ever changes without a matching edit
    // here, the line below fails to compile with the count it actually
    // found, naming this exact spot rather than surfacing only in some
    // later differential run. (Not written as `static_assert(requires{...})`:
    // a structured-binding mismatch inside a CONCRETE lambda body — not a
    // template — is not a substitution failure, so `requires` cannot catch
    // it and turn it into a custom message; it is a hard compile error
    // either way, which satisfies "fails loudly at build time" on its own.)
    [[maybe_unused]] auto const& [cfg_a, cfg_b, cfg_c, cfg_d, cfg_e, cfg_f] = cfg_result->introspection.transcripts;
    [[maybe_unused]] auto const& [ia_a, ia_b, ia_c, ia_d, ia_e, ia_f, ia_g] = transcripts;
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

/// @brief Declare the `report` leaf.
///
/// `--json` carries a description here, so it is `add_bool` rather
/// than `add_json`; `init` is the only other such site.
auto declare_report(CLI::App& root) -> void {
  CLI::App* report = root.add_subcommand(
      "report",
      "Reads the cli_invocations capture log and the always-on observability\ntables (agent_actions, sync_events, "
      "agent_work_claims, handoffs) and\nrenders a diagnostic bundle.\n\nInvocation and failure sections render \"logging "
      "disabled\" when\n[introspection].cli_log is off; the always-on sections (actions, sync,\nclaims, claim failure "
      "categories, handoffs, health) render normally in\neither case.\n\nThe bundle is structurally redacted: queries select "
      "only counts,\ncategories, verb paths, statuses, and timestamps — never entity text.\n\nExit codes:\n  0   bundle rendered "
      "successfully.\n  2   invalid flag value (--days or --tail must be a positive integer).\n  1   database error.");
  add_int_default(*report, "--days", "30", "Window in days (must be > 0, default 30).");
  add_int_default(*report, "--tail", "20", "Number of failure-tail rows (must be > 0, default 20).");
  add_bool(*report, "--json", "Emit stable machine-readable JSON.");
}

} // namespace planar::cmd::handlers
