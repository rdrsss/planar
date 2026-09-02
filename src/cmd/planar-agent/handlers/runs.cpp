/// @file runs.cpp
/// @brief Workflow-harness lifecycle handlers.
module planar.cmd.planar_agent.handlers.runs;
import std;
import planar.cliapp.args;
import planar.engine.runtime.workflowruns;
import planar.json_text;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.exit;
import planar.cmd.planar_agent.handler;
namespace planar::cmd::agent::handlers {
namespace wr = engine::runtime::workflowruns;
namespace {
auto text(const cliapp::parsed_args& a, std::string_view name) -> std::string {
  return cliapp::flag_string(a, name).value_or("");
}
auto fail(domain_error_kind kind, std::string body) -> handler_result {
  return std::unexpected(error_from_body(kind, std::move(body)));
}
auto from_start_error(wr::error e, std::int64_t plan) -> handler_result {
  if (e == wr::error::plan_not_found)
    return fail(domain_error_kind::not_found, std::format("plan {} not found", plan));
  return fail(domain_error_kind::generic_failure, "insert workflow_runs: QueryFailed");
}
auto from_end_error(wr::error e, std::string_view identifier, std::string_view status = {}) -> handler_result {
  if (e == wr::error::run_not_found)
    return fail(domain_error_kind::not_found, std::format("run '{}' not found", identifier));
  if (e == wr::error::not_running)
    return fail(domain_error_kind::invalid_input,
                std::format("run '{}' is already in terminal status '{}'; cannot end again", identifier, status));
  return fail(domain_error_kind::generic_failure, "update workflow_runs: QueryFailed");
}
} // namespace
auto run_start(context& ctx, const cliapp::parsed_args& a) -> handler_result {
  auto c = ctx.ensure_db();
  if (!c)
    return std::unexpected(c.error());
  auto plan = cliapp::flag_int(a, "--plan"), pid = cliapp::flag_int(a, "--pid");
  if (!plan)
    return fail(domain_error_kind::invalid_input,
                std::format("invalid --plan '{}': expected integer plan id", text(a, "--plan")));
  if (!pid)
    return fail(domain_error_kind::invalid_input,
                std::format("invalid --pid '{}': expected integer process id", text(a, "--pid")));
  auto r = wr::start(**c, {.plan_id        = *plan,
                           .pid            = *pid,
                           .workflow_name  = text(a, "--workflow"),
                           .run_identifier = text(a, "--run-id"),
                           .repo_root      = text(a, "--repo-root")});
  if (!r)
    return from_start_error(r.error(), *plan);
  if (cliapp::flag_bool(a, "--json"))
    ctx.out() << std::format("{{\"ok\":true,\"run_id\":{},\"run\":{{\"id\":{},\"plan_id\":{},\"workflow_name\":{},\"run_"
                             "identifier\":{},\"pid\":{},\"repo_root\":{},\"status\":\"running\"}}}}\n",
                             r->id, r->id, r->plan_id, json_text::json_string(r->workflow_name),
                             json_text::json_string(r->run_identifier), r->pid, json_text::json_string(r->repo_root));
  else
    ctx.out() << std::format("run:{} plan:{} workflow:{} pid:{} status:running\n", r->id, r->plan_id, r->workflow_name, r->pid);
  return {};
}
auto run_end(context& ctx, const cliapp::parsed_args& a) -> handler_result {
  auto c = ctx.ensure_db();
  if (!c)
    return std::unexpected(c.error());
  auto identifier = text(a, "--run-id"), status = text(a, "--status");
  if (!wr::terminal_status(status))
    return fail(domain_error_kind::invalid_input,
                std::format("invalid --status '{}': must be one of completed|failed|interrupted", status));
  auto r = wr::end(**c, identifier, status);
  if (!r) {
    if (r.error() == wr::error::not_running) {
      auto current = wr::find(**c, identifier);
      return from_end_error(r.error(), identifier, current ? current->status : std::string_view{"unknown"});
    }
    return from_end_error(r.error(), identifier);
  }
  if (cliapp::flag_bool(a, "--json"))
    ctx.out() << std::format("{{\"ok\":true,\"run_id\":{},\"status\":{}}}\n", r->id, json_text::json_string(r->status));
  else
    ctx.out() << std::format("run:{} status:{}\n", r->id, r->status);
  return {};
}
} // namespace planar::cmd::agent::handlers
