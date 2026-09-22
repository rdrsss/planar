/// @file closure.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.closure`.

module planar.cmd.planar.handlers.closure;

import std;
import cli11;
import planar.cliapp.args;
import planar.engine.closure;
import planar.engine.planning;
import planar.engine.identity;
import planar.cmd.planar.context;
import planar.cmd.planar.scope;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.declare;
import planar.cmd.planar.handlers.closure.compute;
import planar.cmd.planar.handlers.closure.show;

namespace planar::cmd::handlers {

namespace store = engine::closure::store;

auto closure_compute(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const raw = cliapp::positional_string(args, "task-id").value_or(std::string{});
  auto const id  = cliapp::parse_int64_zig(raw);
  if (!id)
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, std::format("task id must be an integer, got '{}'", raw)));
  auto conn = ctx.ensure_db();
  if (!conn)
    return std::unexpected(conn.error());
  auto task = engine::planning::show_task(**conn, *id);
  if (!task)
    return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("no task with id {}", *id)));
  auto kind   = task->scope_kind == engine::planning::task_scope_kind::global        ? engine::identity::scope_kind::global
                : task->scope_kind == engine::planning::task_scope_kind::association ? engine::identity::scope_kind::association
                                                                                     : engine::identity::scope_kind::repo;
  auto entity = engine::identity::slug_from_ref(**conn, kind, task->scope_id);
  if (!entity)
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, "closure compute: looking up entity scope: QueryFailed"));
  auto resolved = resolve_write_scope(ctx, cliapp::flag_string(args, "--scope"), "closure compute: resolving write scope");
  if (!resolved)
    return std::unexpected(resolved.error());
  auto e = entity->has_value() ? std::optional<std::string_view>{**entity} : std::nullopt;
  auto w = resolved->scope.has_value() ? std::optional<std::string_view>{*resolved->scope} : std::nullopt;
  // Membership-aware, not strict equality (decision 1121, task 6735) --
  // see the same change at task.cpp's `task update` guard for why.
  if (!guard_with_membership(**conn, e, w)) {
    auto el = entity->value_or("global"), wl = resolved->scope.value_or("global");
    return std::unexpected(error_from_body(domain_error_kind::scope_mismatch,
                                           std::format("scope mismatch: task {} is in scope '{}' but operator write scope is "
                                                       "'{}'; pass --scope {} to write to that scope from here",
                                                       *id, el, wl, el)));
  }
  auto result = engine::closure::compute::extract(**conn, *id);
  if (!result) {
    if (result.error() == engine::closure::compute::error::no_seeds)
      return std::unexpected(error_from_body(
          domain_error_kind::invalid_input,
          std::format("closure compute: task {} declares no path-level touches (task_touch_paths); nothing to compute", *id)));
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "closure compute: QueryFailed"));
  }
  if (cliapp::flag_bool(args, "--json"))
    ctx.out() << std::format("{{\"task_id\":{},\"seeds\":{},\"modify\":{},\"reference\":{},\"transitive\":{},\"rows_written\":{},"
                             "\"extractor_version\":\"{}\"}}\n",
                             result->task_id, result->seeds, result->modify, result->reference, result->transitive,
                             result->rows_written, engine::closure::compute::extractor_version);
  else
    ctx.out() << std::format(
        "closure computed for task {}: {} seed(s) -> {} rows (modify={} reference={} transitive={}); extractor={}\n",
        result->task_id, result->seeds, result->rows_written, result->modify, result->reference, result->transitive,
        engine::closure::compute::extractor_version);
  return {};
}

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

/// @brief Declare the `closure` group and its two leaves.
auto declare_closure(CLI::App& root) -> void {
  CLI::App* closure = root.add_subcommand(
      "closure",
      "Compute the *derived* closure of a task — the symbols it must hold\nresident, computed by static analysis from the task's "
      "declared seed\npaths (task_touch_paths), partitioned by role:\n\n  modify     — the seed's own edited symbols.\n  "
      "reference  — the interfaces the seed depends on.\n  transitive — deeper hops (stored, but excluded from the effective\n   "
      "            closure by default).\n\n  Workflow: closure compute <task-id> → closure show <task-id> --json.");
  closure->require_subcommand(0);

  closure_cli::attach_compute(closure);

  closure_cli::attach_show(closure);
}

} // namespace planar::cmd::handlers
