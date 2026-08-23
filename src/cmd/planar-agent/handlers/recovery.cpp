/// @file recovery.cpp
/// @brief Implementation of `planar.cmd.planar_agent.handlers.recovery`.

module planar.cmd.planar_agent.handlers.recovery;

import std;
import planar.cli;
import planar.db;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentrender;
import planar.engine.runtime.session;
import planar.cmd.planar_agent.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.exit;
import planar.cmd.planar_agent.handler;
import planar.cmd.planar_agent.handlers.support;

namespace planar::cmd::agent::handlers {

namespace aa     = engine::runtime::agentactivity;
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

} // namespace

auto reconcile(context& ctx, const cli::match_result& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const grace_raw = flag_string(args, "--stale-after").value_or(std::string{"0"});
  auto const grace     = parse_ttl_seconds(grace_raw);
  if (!grace.has_value()) {
    return std::unexpected(duration_error("--stale-after", grace_raw, "0"));
  }

  auto const dry_run = flag_bool(args, "--dry-run");

  // `--session` carries a SENTINEL-ZERO default while `--plan` is a plain
  // optional. The two spellings mean the same thing — 0 or absent is a
  // global sweep — and the asymmetry is the Zig tree's, reproduced because
  // it is visible in `--help` (`--session` renders `default=0`).
  auto const session_flag = flag_int(args, "--session").value_or(0);
  auto const plan_flag    = flag_int(args, "--plan").value_or(0);

  aa::reconcile_policy policy{
      .stale_after_secs = *grace,
      .dry_run          = dry_run,
      .session_id       = session_flag > 0 ? std::optional<std::int64_t>{session_flag} : std::nullopt,
      .plan_id          = plan_flag > 0 ? std::optional<std::int64_t>{plan_flag} : std::nullopt,
  };
  if (auto const category = flag_string(args, "--category"); category.has_value()) {
    policy.category = aa::failure_category_from_text(*category);
  }

  // ONE transaction around BOTH sweeps when applying; none when
  // dry-running, because a dry run performs no writes to protect.
  std::optional<db::transaction> tx;
  if (!dry_run) {
    auto opened = (*conn)->begin_transaction(db::lock_mode::immediate);
    if (!opened) {
      return std::unexpected(error_from_body(cli::domain_error_kind::generic_failure, "BEGIN IMMEDIATE: QueryFailed"));
    }
    tx.emplace(std::move(*opened));
  }

  auto const claims = aa::reconcile_stale(**conn, policy);
  if (!claims) {
    return std::unexpected(verb_error("reconcile", claims.error()));
  }
  auto const runs = aa::reconcile_runs(**conn, dry_run, policy.plan_id);
  if (!runs) {
    return std::unexpected(verb_error("reconcile runs", runs.error()));
  }

  if (tx.has_value() && !tx->commit()) {
    return std::unexpected(error_from_body(cli::domain_error_kind::generic_failure, "COMMIT: QueryFailed"));
  }

  ctx.out() << (flag_bool(args, "--json") ? render::reconcile_json(*claims, *runs, dry_run)
                                          : render::reconcile_text(*claims, *runs, dry_run));
  return {};
}

auto abort(context& ctx, const cli::match_result& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // Outside the transaction, deliberately: opening the ABORTING session is
  // a read-or-create on `sessions` unrelated to the claim being recovered,
  // and holding the write lock across it would serialise operators for no
  // benefit.
  auto const vendor  = flag_string(args, "--vendor").value_or(std::string{"planar-agent"});
  auto const vsid    = flag_string(args, "--vendor-session");
  auto const session = ses::ensure_active(**conn, vendor, view(vsid));
  if (!session) {
    return std::unexpected(session_error_message(session.error(), true));
  }

  auto tx = (*conn)->begin_transaction(db::lock_mode::immediate);
  if (!tx) {
    return std::unexpected(error_from_body(cli::domain_error_kind::generic_failure, "BEGIN IMMEDIATE: QueryFailed"));
  }

  std::optional<aa::failure_category> category;
  if (auto const raw = flag_string(args, "--category"); raw.has_value()) {
    category = aa::failure_category_from_text(*raw);
  }
  auto const reason  = flag_string(args, "--reason");
  auto const token   = flag_string(args, "--claim").value_or(std::string{});
  auto const aborted = aa::abort_claim(**conn, token, view(reason), category);
  if (!aborted) {
    return std::unexpected(verb_error("abort", aborted.error()));
  }

  if (aborted->kind == aa::entity_kind::task) {
    auto const recovered = aa::reset_direct_claim_task_after_abort(**conn, aborted->id, aborted->entity_id);
    if (!recovered) {
      return std::unexpected(verb_error("abort task recovery", recovered.error()));
    }
  }

  auto const summary = reason.has_value() ? std::format("aborted by operator: {}", *reason) : std::string{"aborted by operator"};
  // The audit row's vendor is the LITERAL "planar-agent", not `--vendor`.
  // That looks like a bug and is the oracle's behavior; preserved under D2
  // so a `planar-watch` reader sees the same rows from both binaries.
  auto const audit_id = aa::start_action(**conn, aa::start_action_args{
                                                     .session_id = *session,
                                                     .claim_id   = aborted->id,
                                                     .kind       = aa::action_kind::other,
                                                     .vendor     = "planar-agent",
                                                 });
  if (!audit_id) {
    return std::unexpected(verb_error("audit startAction", audit_id.error()));
  }
  auto const closed = aa::end_action(**conn, *audit_id, aa::outcome::aborted, std::string_view{summary});
  if (!closed) {
    return std::unexpected(verb_error("audit endAction", closed.error()));
  }

  if (!tx->commit()) {
    return std::unexpected(error_from_body(cli::domain_error_kind::generic_failure, "COMMIT: QueryFailed"));
  }

  ctx.out() << (flag_bool(args, "--json") ? render::abort_json(*aborted, *session) : render::abort_text(*aborted, *session));
  return {};
}

} // namespace planar::cmd::agent::handlers
