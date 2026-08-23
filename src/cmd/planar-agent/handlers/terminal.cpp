/// @file terminal.cpp
/// @brief Implementation of `planar.cmd.planar_agent.handlers.terminal`.

module planar.cmd.planar_agent.handlers.terminal;

import std;
import planar.cli;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentatomic;
import planar.engine.runtime.agentrender;
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

/// @brief Re-read the task and write the shared envelope.
///
/// Re-READ rather than reasoning about what the transition must have
/// produced: the printed `status` is what the database now holds, so an
/// engine change that wrote something unexpected shows up in the output
/// instead of being masked by a hardcoded string.
/// @param ctx The invocation context.
/// @param result The terminal outcome.
/// @param json Whether `--json` was given.
/// @return Success, or the failure to report.
auto emit(context& ctx, const atomic::terminal_result& result, bool json) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const task = aa::get_task(**conn, result.task_id);
  if (!task) {
    return std::unexpected(verb_error("task show", task.error()));
  }
  ctx.out() << (json ? render::terminal_json(result, *task) : render::terminal_text(result, *task));
  return {};
}

} // namespace

auto complete(context& ctx, const cli::match_result& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const summary = flag_string(args, "--summary");
  auto const result =
      atomic::complete_work(**conn, flag_string(args, "--claim").value_or(std::string{}), view(summary), task_policy());
  if (!result) {
    return std::unexpected(verb_error("complete", result.error()));
  }
  return emit(ctx, *result, flag_bool(args, "--json"));
}

auto fail(context& ctx, const cli::match_result& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  // The parser constrains `--category` to the choice set and defaults it
  // to "unknown", so this lookup cannot miss — but it falls back rather
  // than asserting, because a tree-authoring mistake should not abort in
  // an operator's shell.
  auto const category_text = flag_string(args, "--category").value_or(std::string{"unknown"});
  auto const category      = aa::failure_category_from_text(category_text).value_or(aa::failure_category::unknown);
  auto const reason        = flag_string(args, "--reason").value_or(std::string{});
  auto const result =
      atomic::fail_work(**conn, flag_string(args, "--claim").value_or(std::string{}), reason, category, task_policy());
  if (!result) {
    return std::unexpected(verb_error("fail", result.error()));
  }
  return emit(ctx, *result, flag_bool(args, "--json"));
}

auto release(context& ctx, const cli::match_result& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const reason = flag_string(args, "--reason");
  auto const result =
      atomic::release_work(**conn, flag_string(args, "--claim").value_or(std::string{}), view(reason), task_policy());
  if (!result) {
    return std::unexpected(verb_error("release", result.error()));
  }
  return emit(ctx, *result, flag_bool(args, "--json"));
}

auto block(context& ctx, const cli::match_result& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const reason = flag_string(args, "--reason");
  auto const result = atomic::block_work(**conn, flag_string(args, "--claim").value_or(std::string{}),
                                         flag_int(args, "--blocker").value_or(0), view(reason), task_policy());
  if (!result) {
    return std::unexpected(verb_error("block", result.error()));
  }
  return emit(ctx, *result, flag_bool(args, "--json"));
}

} // namespace planar::cmd::agent::handlers
