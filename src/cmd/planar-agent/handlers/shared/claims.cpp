/// @file claims.cpp
/// @brief Implementation of `planar.cmd.planar_agent.handlers.claims`.

module;

#include <glaze/json/read.hpp>

module planar.cmd.planar_agent.handlers.claims;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentatomic;
import planar.engine.runtime.agentrender;
import planar.engine.runtime.session;
import planar.cmd.planar_agent.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.exit;
import planar.cmd.planar_agent.handler;
import planar.cmd.planar_agent.locality;
import planar.cmd.planar_agent.policy;
import planar.cmd.planar_agent.handlers.support;

namespace planar::cmd::agent::handlers {

namespace aa     = engine::runtime::agentactivity;
namespace atomic = engine::runtime::agentatomic;
namespace render = engine::runtime::agentrender;
namespace ses    = engine::runtime::session;

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

/// @brief Split `--worktree` into an id or a path.
///
/// A value that parses as an integer is a `worktrees` row id (and is
/// validated against that table); anything else is stored verbatim as a
/// path. There is no flag to disambiguate, so a worktree literally named
/// `"7"` would be read as an id — the Zig original has the same ambiguity.
/// @param raw The `--worktree` value, or unset.
/// @return The id and path, at most one of which is set.
struct worktree_split {
  std::optional<std::int64_t> id;   ///< The row id, when the value parsed as one.
  std::optional<std::string>  path; ///< The path, otherwise.
};

auto split_worktree(const std::optional<std::string>& raw) -> worktree_split {
  if (!raw.has_value()) {
    return {};
  }
  if (auto const id = cliapp::parse_int64_zig(*raw); id.has_value()) {
    return worktree_split{.id = id};
  }
  return worktree_split{.path = raw};
}

/// @brief Refuse a `--metadata` value that is not well-formed JSON.
///
/// The engine stores the column opaquely and never parses it, so this is
/// the ONLY thing standing between a malformed blob and the database.
/// Validated with the same purpose-built probe `engine/config`
/// uses.
/// @param raw The `--metadata` value, or unset.
/// @return The refusal, or unset when absent or valid.
auto check_metadata(const std::optional<std::string>& raw) -> std::optional<domain_error> {
  if (!raw.has_value()) {
    return std::nullopt;
  }
  if (glz::validate_json(*raw)) {
    return invalid_input_error(std::format("--metadata is not valid JSON: {}", *raw));
  }
  return std::nullopt;
}

/// @brief The one-line fix for a readiness reason, shown under a refused claim.
/// @param reason The packet's reason name.
/// @param task_id The refused task.
/// @return The hint, or the empty string for a reason without one.
auto reason_hint(std::string_view reason, std::int64_t task_id) -> std::string {
  if (reason == "blocked_by_open_task") {
    return std::format("finish the dependency (planar task show {} lists it)", task_id);
  }
  if (reason == "open_question_linked") {
    return "answer the linked open question";
  }
  if (reason == "proposed_decision_linked") {
    return "accept or withdraw the linked proposed decision";
  }
  if (reason == "invalid_task_status") {
    return "the task is not todo, or another claim holds it; wait for its lease to lapse";
  }
  if (reason == "unresolved_citation") {
    return "repair or re-cite the artifact the task cites";
  }
  if (reason == "missing_owning_plan") {
    return "attach the task to a plan";
  }
  if (reason == "missing_title") {
    return "give the task a title";
  }
  return {};
}

/// @brief The refusal for `claim` on a task that is not ready.
///
/// Exit 1, like `ClaimContention` and `IllegalTransition`. Names every reason with its fix and the
/// command that would succeed.
/// @param task_id The refused task.
/// @param reasons Its readiness reasons.
/// @return The domain error to return from the handler.
auto not_ready_error(std::int64_t task_id, const std::vector<std::string>& reasons) -> domain_error {
  std::string list;
  for (auto const& reason : reasons) {
    list += list.empty() ? "" : ", ";
    list += reason;
  }
  std::string body = std::format("claim: task {} is not ready: {}", task_id, list);
  for (auto const& reason : reasons) {
    if (auto const hint = reason_hint(reason, task_id); !hint.empty()) {
      body += std::format("\n  {}: {}", reason, hint);
    }
  }
  body += std::format("\n  to claim it anyway, run: planar-agent claim --entity task:{} --allow-unready", task_id);
  auto err = error_from_body(domain_error_kind::generic_failure, std::move(body));
  err.tag  = "TaskNotReady";
  return err;
}

} // namespace

auto pull(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // An UNRECOGNISED --role degrades to `coder` rather than failing. That
  // is deliberately laxer than `action start --kind`, which refuses an
  // unknown kind outright: `--role` is a free-text label the tree's own
  // help describes as `planner|coder|reviewer|test_coder|...`, and the
  // trailing ellipsis is doing real work.
  // A supervision kind (plan 1033) is treated like any other unrecognised
  // role: the dispatch row is a `coder` row, never a forged audit row.
  auto const role   = cliapp::flag_string(args, "--role");
  auto const parsed = role.has_value() ? aa::action_kind_from_text(*role) : std::nullopt;
  auto const kind   = parsed.has_value() && !aa::is_supervision_kind(*parsed) ? *parsed : aa::action_kind::coder;

  // Two independent reasons to skip: the operator asked, or this action
  // kind does not probe by default. Pull picks a role kind, so its
  // default is probe-on.
  auto const skip = cliapp::flag_bool(args, "--no-locality-probe") || !aa::probe_default(kind);
  auto const loc  = resolve_locality(cliapp::flag_string(args, "--repo-root"), ctx.cwd(), skip);

  auto const worktree = split_worktree(cliapp::flag_string(args, "--worktree"));

  auto const vendor  = cliapp::flag_string(args, "--vendor").value_or(std::string{"planar-agent"});
  auto const vsid    = cliapp::flag_string(args, "--vendor-session");
  auto const session = ses::ensure_active(**conn, vendor, view(vsid));
  if (!session) {
    return std::unexpected(session_error_message(session.error()));
  }

  auto const ttl_raw = cliapp::flag_string(args, "--ttl").value_or(std::string{"600"});
  auto const ttl     = parse_ttl_seconds(ttl_raw);
  if (!ttl.has_value()) {
    return std::unexpected(duration_error("--ttl", ttl_raw, "600"));
  }

  auto const metadata = cliapp::flag_string(args, "--metadata");
  if (auto const refusal = check_metadata(metadata); refusal.has_value()) {
    return std::unexpected(*refusal);
  }

  // --parent-action is checked for EXISTENCE, not just range: a wrong
  // action id in an orchestrator script would otherwise produce a claim
  // whose tree edge points nowhere, and the mistake would only surface
  // later in `planar-watch tree`.
  auto const parent = cliapp::flag_int(args, "--parent-action");
  if (parent.has_value()) {
    if (*parent <= 0) {
      return std::unexpected(invalid_input_error(std::format("--parent-action must be a positive integer (got {})", *parent)));
    }
    auto stmt   = (*conn)->prepare("select 1 from agent_actions where id = ? limit 1");
    bool exists = false;
    if (stmt && stmt->bind_int64(1, *parent)) {
      auto stepped = stmt->step();
      exists       = stepped.has_value() && *stepped == db::step_result::row;
    }
    if (!exists) {
      // NotFound, which `planar-agent`'s codeFor does not map — exit 1,
      // not the 2 the neighbouring range check produces.
      return std::unexpected(
          error_from_body(domain_error_kind::not_found, std::format("--parent-action {}: action not found", *parent)));
    }
  }

  auto const purpose  = cliapp::flag_string(args, "--purpose");
  auto const base_ref = cliapp::flag_string(args, "--base-ref");

  auto const result = atomic::pull_next(**conn,
                                        atomic::pull_args{
                                            .plan_id           = cliapp::positional_int(args, "plan-id").value_or(0),
                                            .session_id        = *session,
                                            .vendor            = vendor,
                                            .vendor_session_id = view(vsid),
                                            .role              = view(role),
                                            .worktree_id       = worktree.id,
                                            .worktree_path     = view(worktree.path),
                                            .purpose           = view(purpose),
                                            .base_ref          = view(base_ref),
                                            .ttl_secs          = *ttl,
                                            .loc               = loc,
                                            .kind              = kind,
                                            .metadata          = view(metadata),
                                            .parent_action_id  = parent,
                                        },
                                        task_policy());
  if (!result) {
    return std::unexpected(verb_error("pull", result.error()));
  }

  auto const json = cliapp::flag_bool(args, "--json");
  // Text mode names the tasks passed over on stderr, so stdout stays the stable `pulled ...` or
  // `no_work` line; `--json` carries them in the payload.
  if (!json) {
    ctx.err() << render::skipped_text(result->skipped);
  }
  if (result->no_work) {
    ctx.out() << (json ? render::no_work_json(result->skipped) : render::no_work_text());
    return {};
  }

  auto const task = aa::get_task(**conn, result->task_id);
  if (!task) {
    return std::unexpected(verb_error("task show", task.error()));
  }
  ctx.out() << (json ? render::pull_json(*result, *task) : render::pull_text(*result));
  return {};
}

auto peek(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const result = atomic::peek_next(**conn, cliapp::positional_int(args, "plan-id").value_or(0), task_policy());
  if (!result) {
    return std::unexpected(verb_error("peek", result.error()));
  }

  auto const json = cliapp::flag_bool(args, "--json");
  if (!json) {
    ctx.err() << render::skipped_text(result->skipped);
  }
  if (result->no_work) {
    ctx.out() << (json ? render::no_work_json(result->skipped) : render::no_work_text());
    return {};
  }
  auto const task = aa::get_task(**conn, result->task_id);
  if (!task) {
    return std::unexpected(verb_error("task show", task.error()));
  }
  ctx.out() << (json ? render::peek_json(*task, result->skipped) : render::peek_text(*task));
  return {};
}

auto claim(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const entity_raw = cliapp::flag_string(args, "--entity").value_or(std::string{});
  auto const entity     = parse_entity_ref(entity_raw);
  if (!entity) {
    // The two refusal tags exit DIFFERENTLY — see `entity_ref_error`.
    auto const kind = entity.error() == entity_ref_error::invalid_entity_ref ? domain_error_kind::invalid_entity_ref
                                                                             : domain_error_kind::generic_failure;
    return std::unexpected(
        error_from_body(kind, std::format("invalid --entity '{}' ({}); expected task:<id>|plan:<id>|plan_step:<id>", entity_raw,
                                          entity_ref_error_name(entity.error()))));
  }

  // NOTE: no `probe_default` gating here, unlike pull. `claim` records no
  // action kind of its own to consult, so `--no-locality-probe` is the
  // only thing that suppresses the probe.
  auto const loc =
      resolve_locality(cliapp::flag_string(args, "--repo-root"), ctx.cwd(), cliapp::flag_bool(args, "--no-locality-probe"));
  auto const worktree = split_worktree(cliapp::flag_string(args, "--worktree"));

  auto const vendor  = cliapp::flag_string(args, "--vendor").value_or(std::string{"planar-agent"});
  auto const vsid    = cliapp::flag_string(args, "--vendor-session");
  auto const session = ses::ensure_active(**conn, vendor, view(vsid));
  if (!session) {
    return std::unexpected(session_error_message(session.error()));
  }

  auto const ttl_raw = cliapp::flag_string(args, "--ttl").value_or(std::string{"600"});
  auto const ttl     = parse_ttl_seconds(ttl_raw);
  // A zero-second result mints an already-lapsed lease (task 6906); unlike
  // `--stale-after` (whose own zero is a legitimate "no grace" default),
  // `--ttl` has no meaningful zero, so it takes the same error shape as a
  // malformed value.
  if (!ttl.has_value() || *ttl == 0) {
    return std::unexpected(duration_error("--ttl", ttl_raw, "600"));
  }

  auto const role    = cliapp::flag_string(args, "--role");
  auto const model   = cliapp::flag_string(args, "--model");
  auto const purpose = cliapp::flag_string(args, "--purpose");

  atomic::claim_readiness readiness{.allow_unready = cliapp::flag_bool(args, "--allow-unready")};
  auto const              acquired = atomic::claim_entity(**conn,
                                                          aa::acquire_args{
                                                              .session_id        = *session,
                                                              .kind              = entity->kind,
                                                              .entity_id         = entity->id,
                                                              .vendor            = vendor,
                                                              .vendor_session_id = view(vsid),
                                                              .role              = view(role),
                                                              .model             = view(model),
                                                              .worktree_id       = worktree.id,
                                                              .worktree_path     = view(worktree.path),
                                                              .purpose           = view(purpose),
                                                              .ttl_secs          = *ttl,
                                                              .loc               = loc,
                                                              .force             = cliapp::flag_bool(args, "--force"),
                                                          },
                                                          !cliapp::flag_bool(args, "--no-transition"), task_policy(), &readiness);
  if (!acquired) {
    if (acquired.error() == aa::agent_error::task_not_ready) {
      return std::unexpected(not_ready_error(entity->id, readiness.unready_reasons));
    }
    return std::unexpected(verb_error("claim", acquired.error()));
  }

  ctx.out() << (cliapp::flag_bool(args, "--json") ? render::claim_json(*acquired) : render::claim_text(*acquired));
  return {};
}

auto heartbeat(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // An omitted `--ttl` is NOT defaulted to 600 here: it means "renew the
  // lease length this claim already holds". Defaulting would make omission
  // indistinguishable from an explicit `--ttl 600` and silently truncate
  // every long lease (Planar task 6093).
  auto const                  ttl_flag = cliapp::flag_string(args, "--ttl");
  std::optional<std::int64_t> ttl;
  if (ttl_flag.has_value()) {
    auto const parsed = parse_ttl_seconds(*ttl_flag);
    if (!parsed.has_value()) {
      // BEFORE the transaction: a malformed duration should not open one.
      return std::unexpected(duration_error("--ttl", *ttl_flag, "600"));
    }
    ttl = *parsed;
  }
  auto const token = cliapp::flag_string(args, "--claim").value_or(std::string{});

  // `--status` present-and-empty is NOT the same as absent: an omitted
  // flag writes no action row at all, while `--status ""` writes one with
  // an empty summary. `flag_string` distinguishes them for us. Capped
  // BEFORE the transaction.
  auto const status = cliapp::flag_string(args, "--status");
  if (status.has_value()) {
    constexpr std::size_t k_status_cap = 256;
    if (status->size() > k_status_cap) {
      return std::unexpected(invalid_input_error(
          std::format("--status payload is {} bytes; the cap is {}. Shorten the status string.", status->size(), k_status_cap)));
    }
  }

  // One immediate transaction in the engine: the lease and the status action
  // land together or not at all. The heartbeat acts only as the caller, and a
  // legacy engine-supervised claim is an ordinary claim to it; the engine actor
  // is no longer reachable from this binary (task 7458).
  auto const refreshed = atomic::supervised_heartbeat(**conn, token, ttl, view(status), atomic::supervisor_gate{});
  if (!refreshed) {
    return std::unexpected(verb_error("heartbeat", refreshed.error()));
  }

  ctx.out() << (cliapp::flag_bool(args, "--json") ? render::claim_json(*refreshed) : render::heartbeat_text(*refreshed));
  return {};
}

} // namespace planar::cmd::agent::handlers
