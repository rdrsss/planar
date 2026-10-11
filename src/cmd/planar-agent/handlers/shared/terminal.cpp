/// @file terminal.cpp
/// @brief Implementation of `planar.cmd.planar_agent.handlers.terminal`.

module planar.cmd.planar_agent.handlers.terminal;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.internal.config_path;
import planar.db;
import planar.engine.config.effective;
import planar.engine.diagnose;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentatomic;
import planar.engine.runtime.agentrender;
import planar.engine.runtime.sessioncommits;
import planar.cmd.planar_agent.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.exit;
import planar.cmd.planar_agent.handler;
import planar.cmd.planar_agent.policy;
import planar.cmd.planar_agent.handlers.support;

namespace planar::cmd::agent::handlers {

namespace aa     = engine::runtime::agentactivity;
namespace atomic = engine::runtime::agentatomic;
namespace render = engine::runtime::agentrender;

namespace {

/// @brief View an optional string as an optional string_view.
/// @param value The owning optional.
/// @return A view over it, or unset.
auto view(const std::optional<std::string>& value) -> std::optional<std::string_view> {
  if (!value.has_value()) {
    return std::nullopt;
  }
  return std::string_view{*value};
}

/// @brief The current instant as `YYYY-MM-DDTHH:MM:SS.mmmZ`, the form the diagnose engine takes.
/// @return The UTC instant.
auto now_iso() -> std::string {
  auto const now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
  return std::format("{:%Y-%m-%dT%H:%M:%S}Z", now);
}

/// @brief Diagnose the milestone a `complete` just promoted to `done`, after the transaction committed.
///
/// Reads only and never fails the verb: the engine returns an unavailable section for a busy database
/// (its own 250 ms timeout) or a failed query, and a connection that cannot be had reads the same way.
/// @param ctx The invocation context.
/// @param observed What the in-transaction roll-up decided.
/// @param result The terminal outcome.
/// @return The section to append, or empty when no milestone was promoted by this call.
auto milestone_section(context& ctx, const plan_roll_up& observed, const atomic::terminal_result& result)
    -> std::optional<engine::diagnose::section> {
  if (!milestone_promoted(observed, result)) {
    return std::nullopt;
  }
  auto const cli_log = engine::config::introspection_cli_log(internal::resolve_config_path(ctx.env()));
  auto       conn    = ctx.db().ensure_db();
  if (!conn) {
    return engine::diagnose::unavailable_section(observed.result->plan_id, now_iso(),
                                                 engine::diagnose::unavailable_reason::query_failed);
  }
  return engine::diagnose::run_section(**conn, observed.result->plan_id, now_iso(), cli_log);
}

/// @brief Re-read the task and write the shared envelope.
///
/// Re-READ rather than reasoning about what the transition must have
/// produced: the printed `status` is what the database now holds, so an
/// engine change that wrote something unexpected shows up in the output
/// instead of being masked by a hardcoded string.
/// @param ctx The invocation context.
/// @param result The terminal outcome.
/// @param json Whether `--json` was given.
/// @param section The diagnose section to append, or empty. Text gets it after one newline; JSON
/// gets `diagnose` as the last key of the one object.
/// @return Success, or the failure to report.
auto emit(context& ctx, const atomic::terminal_result& result, bool json,
          const std::optional<engine::diagnose::section>& section = std::nullopt) -> handler_result {
  auto const trailing = section.has_value() ? std::format(",\"diagnose\":{}", section->json) : std::string{};
  auto const suffix   = section.has_value() ? std::format("\n{}", section->text) : std::string{};
  auto       conn     = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  if (!result.task_id.has_value()) {
    // A plan / plan_step claim (only `release` ends one) holds no task.
    // Looking one up by the claim's entity id would print whatever unrelated
    // task shares that number (task 7118).
    ctx.out() << (json ? render::terminal_json(result, trailing) : render::terminal_text(result) + suffix);
    return {};
  }
  auto const task = aa::get_task(**conn, *result.task_id);
  if (!task) {
    return std::unexpected(verb_error("task show", task.error()));
  }
  ctx.out() << (json ? render::terminal_json(result, *task, trailing) : render::terminal_text(result, *task) + suffix);
  return {};
}

/// @brief Port of `terminal_common.collectCommits` (task 6360). Best-effort
/// claim-window commit harvest run AFTER the atomic op commits and BEFORE
/// the shared envelope is emitted, matching the oracle's call order in
/// complete.zig/fail.zig/release.zig/block.zig. Swallows every failure --
/// nothing atomic and nothing these verbs print depends on it.
/// @param ctx The invocation context.
/// @param result The terminal outcome; `result.released` carries the
/// claim's locality snapshot.
/// @param no_locality_probe The verb's `--no-locality-probe` flag.
auto collect_commits(context& ctx, const atomic::terminal_result& result, bool no_locality_probe) -> void {
  // A replay (an engine terminal verb repeated under the attempt that
  // already landed it) wrote nothing; harvesting again would double-record
  // the claim window.
  if (result.replayed) {
    return;
  }
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return;
  }
  engine::runtime::sessioncommits::claim_window const window{
      .claim_id          = result.released.id,
      .session_id        = result.released.session_id,
      .worktree_path     = result.released.worktree_path,
      .repo_root         = result.released.repo_root,
      .head_sha_at_claim = result.released.head_sha_at_claim,
  };
  engine::runtime::sessioncommits::record_claim_window_best_effort(**conn, window, no_locality_probe);
}

} // namespace

auto complete(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const flags = parse_gate_flags(args);
  if (!flags) {
    return std::unexpected(flags.error());
  }
  auto const   summary = cliapp::flag_string(args, "--summary");
  plan_roll_up observed;
  auto const   result = atomic::complete_work(**conn, cliapp::flag_string(args, "--claim").value_or(std::string{}), view(summary),
                                              task_policy(&observed), flags->gate());
  if (!result) {
    return std::unexpected(verb_error("complete", result.error()));
  }
  collect_commits(ctx, *result, cliapp::flag_bool(args, "--no-locality-probe"));
  return emit(ctx, *result, cliapp::flag_bool(args, "--json"), milestone_section(ctx, observed, *result));
}

auto fail(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  // The parser constrains `--category` to the choice set and defaults it
  // to "unknown", so this lookup cannot miss — but it falls back rather
  // than asserting, because a tree-authoring mistake should not abort in
  // an operator's shell.
  auto const flags = parse_gate_flags(args);
  if (!flags) {
    return std::unexpected(flags.error());
  }
  auto const category_text = cliapp::flag_string(args, "--category").value_or(std::string{"unknown"});
  auto const category      = aa::failure_category_from_text(category_text).value_or(aa::failure_category::unknown);
  auto const reason        = cliapp::flag_string(args, "--reason").value_or(std::string{});
  auto const result = atomic::fail_work(**conn, cliapp::flag_string(args, "--claim").value_or(std::string{}), reason, category,
                                        task_policy(), flags->gate());
  if (!result) {
    return std::unexpected(verb_error("fail", result.error()));
  }
  collect_commits(ctx, *result, cliapp::flag_bool(args, "--no-locality-probe"));
  return emit(ctx, *result, cliapp::flag_bool(args, "--json"));
}

auto release(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const flags = parse_gate_flags(args);
  if (!flags) {
    return std::unexpected(flags.error());
  }
  auto const reason = cliapp::flag_string(args, "--reason");
  auto const result = atomic::release_work(**conn, cliapp::flag_string(args, "--claim").value_or(std::string{}), view(reason),
                                           task_policy(), flags->gate());
  if (!result) {
    return std::unexpected(verb_error("release", result.error()));
  }
  collect_commits(ctx, *result, cliapp::flag_bool(args, "--no-locality-probe"));
  return emit(ctx, *result, cliapp::flag_bool(args, "--json"));
}

auto block(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const flags = parse_gate_flags(args);
  if (!flags) {
    return std::unexpected(flags.error());
  }
  auto const reason = cliapp::flag_string(args, "--reason");
  auto const result =
      atomic::block_work(**conn, cliapp::flag_string(args, "--claim").value_or(std::string{}),
                         cliapp::flag_int(args, "--blocker").value_or(0), view(reason), task_policy(), flags->gate());
  if (!result) {
    return std::unexpected(verb_error("block", result.error()));
  }
  collect_commits(ctx, *result, cliapp::flag_bool(args, "--no-locality-probe"));
  return emit(ctx, *result, cliapp::flag_bool(args, "--json"));
}

} // namespace planar::cmd::agent::handlers
