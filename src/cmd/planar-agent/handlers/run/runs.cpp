/// @file runs.cpp
/// @brief Workflow-harness lifecycle handlers.
module planar.cmd.planar_agent.handlers.runs;
import std;
import planar.cliapp.args;
import planar.engine.runtime.workflowruns;
import planar.json_text;
import planar.cmd.planar_agent.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.exit;
import planar.cmd.planar_agent.handlers.support;
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
  if (e == wr::error::unsupervised)
    return fail(domain_error_kind::invalid_input,
                "a workflow run must be supervised: supply --pid (pid-supervised) or --ttl (lease-supervised)");
  return fail(domain_error_kind::generic_failure, "insert workflow_runs: StepFailed");
}
auto from_end_error(wr::error e, std::string_view identifier, std::string_view status = {}) -> handler_result {
  if (e == wr::error::run_not_found)
    return fail(domain_error_kind::not_found, std::format("run '{}' not found", identifier));
  if (e == wr::error::not_running)
    return fail(domain_error_kind::invalid_input,
                std::format("run '{}' is already in terminal status '{}'; cannot end again", identifier, status));
  return fail(domain_error_kind::generic_failure, "update workflow_runs: QueryFailed");
}
auto from_heartbeat_error(wr::error e, std::string_view identifier, std::string_view status = {}) -> handler_result {
  if (e == wr::error::run_not_found)
    return fail(domain_error_kind::not_found, std::format("run '{}' not found", identifier));
  if (e == wr::error::not_running)
    return fail(domain_error_kind::invalid_input,
                std::format("run '{}' is in terminal status '{}'; cannot heartbeat", identifier, status));
  if (e == wr::error::pid_bound)
    return fail(domain_error_kind::invalid_input,
                std::format("run '{}' is pid-supervised; heartbeat only extends a pid-less run's lease", identifier));
  return fail(domain_error_kind::generic_failure, "update workflow_runs: QueryFailed");
}
auto pid_json(std::optional<std::int64_t> pid) -> std::string {
  return pid.has_value() ? std::to_string(*pid) : std::string{"null"};
}
auto expires_at_json(const std::optional<std::string>& expires_at) -> std::string {
  return expires_at.has_value() ? json_text::json_string(*expires_at) : std::string{"null"};
}
auto pid_text(std::optional<std::int64_t> pid) -> std::string {
  return pid.has_value() ? std::to_string(*pid) : std::string{"-"};
}
auto expires_at_text(const std::optional<std::string>& expires_at) -> std::string {
  return expires_at.has_value() ? *expires_at : std::string{"-"};
}
} // namespace

/// @brief Handle `planar-agent run start`.
///
/// `--pid` and `--ttl` are mutually exclusive supervision modes (decision
/// D11, task 6847): a pid-bound run is probed for liveness by `reconcile`;
/// a pid-less run carries a lease (`expires_at`, from `--ttl`) that
/// `reconcile` abandons on expiry and `run heartbeat` extends. Exactly one
/// must be supplied, checked BEFORE the plan lookup so a malformed
/// supervision choice never opens a transaction or touches the database.
/// @param ctx The invocation context.
/// @param a The parsed arguments.
/// @return Success, or the failure to report.
auto run_start(context& ctx, const cliapp::parsed_args& a) -> handler_result {
  auto c = ctx.db().ensure_db();
  if (!c)
    return std::unexpected(c.error());
  auto plan = cliapp::flag_int(a, "--plan");
  if (!plan)
    return fail(domain_error_kind::invalid_input,
                std::format("invalid --plan '{}': expected integer plan id", text(a, "--plan")));

  auto const pid_raw = cliapp::flag_string(a, "--pid");
  auto const ttl_raw = cliapp::flag_string(a, "--ttl");

  std::optional<std::int64_t> pid;
  std::optional<std::int64_t> ttl_secs;
  if (pid_raw.has_value() && ttl_raw.has_value()) {
    // Both given is a contradiction, not a precedence question: silently
    // preferring --pid would hand back a pid-supervised run to a caller
    // that asked for a lease, and every later `run heartbeat` on it would
    // refuse with `pid-supervised` (task 6847, reviewer finding 1).
    return fail(domain_error_kind::invalid_input,
                "planar-agent run start takes either --pid (pid-supervised) or --ttl (lease-supervised), "
                "not both; they are mutually exclusive supervision modes");
  }
  if (pid_raw.has_value()) {
    auto const parsed = cliapp::flag_int(a, "--pid");
    if (!parsed)
      return fail(domain_error_kind::invalid_input, std::format("invalid --pid '{}': expected integer process id", *pid_raw));
    pid = *parsed;
  } else if (ttl_raw.has_value()) {
    auto const parsed = parse_ttl_seconds(*ttl_raw);
    // A zero-second result sets an already-lapsed `expires_at` (task
    // 6906): the run would be reconcile-eligible for abandonment the
    // instant it is started. Same error shape as a malformed value.
    if (!parsed || *parsed == 0)
      return std::unexpected(duration_error("--ttl", *ttl_raw, "600"));
    ttl_secs = *parsed;
  } else {
    return fail(domain_error_kind::invalid_input,
                "planar-agent run start requires either --pid (pid-supervised) or --ttl (lease-supervised); "
                "a pid-less run must carry a lease");
  }

  auto r = wr::start(**c, {.plan_id        = *plan,
                           .pid            = pid,
                           .ttl_secs       = ttl_secs,
                           .workflow_name  = text(a, "--workflow"),
                           .run_identifier = text(a, "--run-id"),
                           .repo_root      = text(a, "--repo-root")});
  if (!r)
    return from_start_error(r.error(), *plan);
  if (cliapp::flag_bool(a, "--json"))
    ctx.out() << std::format("{{\"ok\":true,\"run_id\":{},\"run\":{{\"id\":{},\"plan_id\":{},\"workflow_name\":{},\"run_"
                             "identifier\":{},\"pid\":{},\"expires_at\":{},\"repo_root\":{},\"status\":\"running\"}}}}\n",
                             r->id, r->id, r->plan_id, json_text::json_string(r->workflow_name),
                             json_text::json_string(r->run_identifier), pid_json(r->pid), expires_at_json(r->expires_at),
                             json_text::json_string(r->repo_root));
  else
    ctx.out() << std::format("run:{} plan:{} workflow:{} pid:{} expires_at:{} status:running\n", r->id, r->plan_id,
                             r->workflow_name, pid_text(r->pid), expires_at_text(r->expires_at));
  return {};
}

/// @brief Handle `planar-agent run end`.
/// @param ctx The invocation context.
/// @param a The parsed arguments.
/// @return Success, or the failure to report.
auto run_end(context& ctx, const cliapp::parsed_args& a) -> handler_result {
  auto c = ctx.db().ensure_db();
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

/// @brief Handle `planar-agent run heartbeat`. Extends a pid-less run's
/// lease; refuses (by name) on a pid-bound run or a non-running run
/// (decision D11, task 6847).
/// @param ctx The invocation context.
/// @param a The parsed arguments.
/// @return Success, or the failure to report.
auto run_heartbeat(context& ctx, const cliapp::parsed_args& a) -> handler_result {
  auto c = ctx.db().ensure_db();
  if (!c)
    return std::unexpected(c.error());
  auto const identifier = text(a, "--run-id");
  auto const ttl_raw    = cliapp::flag_string(a, "--ttl");
  if (!ttl_raw.has_value())
    return fail(domain_error_kind::invalid_input, "planar-agent run heartbeat requires --ttl");
  auto const ttl_secs = parse_ttl_seconds(*ttl_raw);
  if (!ttl_secs)
    return std::unexpected(duration_error("--ttl", *ttl_raw, "600"));

  auto r = wr::heartbeat(**c, identifier, *ttl_secs);
  if (!r) {
    if (r.error() == wr::error::not_running || r.error() == wr::error::pid_bound) {
      auto current = wr::find(**c, identifier);
      return from_heartbeat_error(r.error(), identifier, current ? current->status : std::string_view{"unknown"});
    }
    return from_heartbeat_error(r.error(), identifier);
  }
  if (cliapp::flag_bool(a, "--json"))
    ctx.out() << std::format("{{\"ok\":true,\"run_id\":{},\"expires_at\":{}}}\n", r->id, expires_at_json(r->expires_at));
  else
    ctx.out() << std::format("run:{} expires_at:{}\n", r->id, expires_at_text(r->expires_at));
  return {};
}
} // namespace planar::cmd::agent::handlers
