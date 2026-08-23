/// @file agentatomic.cpp
/// @brief Implementation of `planar.engine.runtime.agentatomic`. See the
/// module interface for the atomicity contract and the injected-policy
/// argument.

module planar.engine.runtime.agentatomic;

import std;
import planar.db;
import planar.engine.runtime.agentactivity;

namespace planar::engine::runtime::agentatomic {

namespace aa = agentactivity;

namespace {

constexpr std::string_view k_now = "strftime('%Y-%m-%dT%H:%M:%fZ','now')";

/// @brief The shared eligibility selector behind BOTH `pull_next` and
/// `peek_next`.
///
/// One definition on purpose: `peek` documents itself as "the same query
/// as pull, no writes", and that claim is only true if there is literally
/// one query. `order by t.priority asc` — lower integers first.
/// @param conn The connection.
/// @param plan_id The plan to select within.
/// @return The next eligible task id, unset when none, or `query_failed`.
auto pick_next_eligible(db::connection& conn, std::int64_t plan_id)
    -> std::expected<std::optional<std::int64_t>, aa::agent_error> {
  auto stmt = conn.prepare(std::format("select t.id from tasks t\n"
                                       "where t.plan_id = ?\n"
                                       "  and t.status = 'todo'\n"
                                       "  and not exists (\n"
                                       "    select 1 from agent_work_claims c\n"
                                       "    where c.entity_kind = 'task'\n"
                                       "      and c.entity_id = t.id\n"
                                       "      and c.status = 'active'\n"
                                       "      and c.lease_expires_at >= {}\n"
                                       "  )\n"
                                       "order by t.priority asc, t.id asc\n"
                                       "limit 1",
                                       k_now));
  if (!stmt || !stmt->bind_int64(1, plan_id)) {
    return std::unexpected(aa::agent_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(aa::agent_error::query_failed);
  }
  if (*stepped != db::step_result::row) {
    return std::optional<std::int64_t>{};
  }
  return std::optional<std::int64_t>{stmt->column_int64(0)};
}

/// @brief The number of rows the most recent statement on `conn` changed.
///
/// `select changes()` because `planar.db` exposes no `sqlite3_changes`
/// accessor. SQLite excludes statements that modify no rows from the
/// counter, so reading it with a SELECT does not disturb it.
/// @param conn The connection.
/// @return The row count; `0` when the probe itself failed.
auto changed_rows(db::connection& conn) -> std::int64_t {
  auto stmt = conn.prepare("select changes()");
  if (!stmt) {
    return 0;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != db::step_result::row) {
    return 0;
  }
  return stmt->column_int64(0);
}

/// @brief `update tasks set status = ? ...`, unguarded on the current
/// status. The callers run the transition guard themselves first.
/// @param conn The connection, inside the transaction.
/// @param task_id The task to flip.
/// @param status The new status.
/// @return Success, or `query_failed`.
auto set_task_status(db::connection& conn, std::int64_t task_id, std::string_view status)
    -> std::expected<void, aa::agent_error> {
  auto stmt = conn.prepare(std::format("update tasks set status = ?, updated_at = {} where id = ?", k_now));
  if (!stmt || !stmt->bind_text(1, status) || !stmt->bind_int64(2, task_id) || !stmt->step()) {
    return std::unexpected(aa::agent_error::query_failed);
  }
  return {};
}

/// @brief Commit a transaction, mapping the driver failure onto this
/// module's error surface.
/// @param tx The transaction.
/// @return Success, or `query_failed`.
auto commit(db::transaction& tx) -> std::expected<void, aa::agent_error> {
  if (!tx.commit()) {
    return std::unexpected(aa::agent_error::query_failed);
  }
  return {};
}

/// @brief The parameterisation the three non-`block` terminal verbs share.
struct terminal_args {
  std::string_view                    claim_token;
  std::optional<std::string_view>     summary;  ///< Written onto the closed actions.
  std::string_view                    task_to;  ///< New `tasks.status`.
  aa::claim_status                    claim_to; ///< New claim status.
  aa::outcome                         result;   ///< Outcome written onto the closed actions.
  std::optional<std::string_view>     reason;   ///< Written onto the claim's `release_reason`.
  std::optional<aa::failure_category> category; ///< Written onto the claim.
};

/// @brief The one transaction `complete` / `fail` / `release` all run.
///
/// Statement order is load-bearing and matches the Zig original exactly:
/// fetch, guard the claim, guard the task transition, flip the task, close
/// the actions, release the claim, recompute the plan — then commit. Every
/// refusal returns before `commit()`, so `db::transaction`'s destructor
/// rolls the whole thing back.
/// @param conn The connection (this function owns the transaction).
/// @param targs The verb's parameterisation.
/// @param policy The injected planning policy.
/// @return The outcome, or the first refusal.
auto terminal_transition(db::connection& conn, const terminal_args& targs, const task_policy& policy)
    -> std::expected<terminal_result, aa::agent_error> {
  auto tx = conn.begin_transaction(db::lock_mode::immediate);
  if (!tx) {
    return std::unexpected(aa::agent_error::query_failed);
  }

  auto held = aa::get_claim_by_token(conn, targs.claim_token);
  if (!held) {
    return std::unexpected(held.error());
  }

  // BOTH halves of "is this claim honored?" — the status column alone is
  // not enough, because an expired lease keeps `status='active'` until a
  // sweep flips it. See agentactivity.cppm's header.
  auto const live = aa::is_claim_active_unexpired(conn, targs.claim_token);
  if (!live) {
    return std::unexpected(live.error());
  }
  if (held->status != aa::claim_status::active || !*live) {
    return std::unexpected(aa::agent_error::claim_not_active);
  }
  if (held->kind != aa::entity_kind::task) {
    return std::unexpected(aa::agent_error::claim_not_on_task);
  }

  auto const current = aa::current_task_status(conn, held->entity_id);
  if (!current) {
    return std::unexpected(current.error());
  }
  auto const allowed = policy.check_transition(*current, targs.task_to);
  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  // Captured BEFORE the flip, because the recompute below needs it and the
  // claim row is about to be replaced by its released form.
  auto const plan_id = aa::task_plan_id(conn, held->entity_id);

  auto const flipped = set_task_status(conn, held->entity_id, targs.task_to);
  if (!flipped) {
    return std::unexpected(flipped.error());
  }

  auto const closed = aa::close_open_actions_for_claim(conn, held->id, targs.result, targs.summary);
  if (!closed) {
    return std::unexpected(closed.error());
  }

  auto released = aa::release_claim(conn, targs.claim_token, targs.claim_to, targs.reason, targs.category);
  if (!released) {
    return std::unexpected(released.error());
  }

  // INSIDE the transaction, so the roll-up sees the task flip above.
  if (plan_id.has_value()) {
    auto const recomputed = policy.recompute_plan(conn, *plan_id);
    if (!recomputed) {
      return std::unexpected(recomputed.error());
    }
  }

  auto const committed = commit(*tx);
  if (!committed) {
    return std::unexpected(committed.error());
  }
  return terminal_result{.released = std::move(*released), .task_id = held->entity_id};
}

} // namespace

// =========================================================================
// Claim acquisition
// =========================================================================

auto claim_entity(db::connection& conn, const agentactivity::acquire_args& args, bool transition_task, const task_policy& policy)
    -> std::expected<claim, agent_error> {
  auto tx = conn.begin_transaction(db::lock_mode::immediate);
  if (!tx) {
    return std::unexpected(aa::agent_error::query_failed);
  }

  auto acquired = aa::acquire_claim(conn, args);
  if (!acquired) {
    return std::unexpected(acquired.error());
  }

  if (args.kind == aa::entity_kind::task && transition_task) {
    auto const current = aa::current_task_status(conn, args.entity_id);
    if (!current) {
      return std::unexpected(current.error());
    }
    // Direct dispatch is an ENTRY into work: only `todo` is eligible, and
    // this is stricter than the transition matrix (which also allows
    // `blocked -> doing`). `--no-transition` is the escape hatch for a
    // caller that wants the bare claim primitive.
    if (*current != "todo") {
      return std::unexpected(aa::agent_error::illegal_transition);
    }
    auto const allowed = policy.check_transition(*current, "doing");
    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    // The `and status = 'todo'` predicate is a SECOND read of the same
    // fact the guard above checked, and the row-count check below is what
    // makes it useful: if anything changed the task between the two reads,
    // the UPDATE matches nothing and this refuses instead of reporting a
    // successful claim over a transition that never happened. The Zig
    // original carries the same `changes() != 1` check; it was missing
    // from the first draft of this port and a break-probe on the
    // concurrency case is what surfaced it.
    auto stmt =
        conn.prepare(std::format("update tasks set status = 'doing', updated_at = {} where id = ? and status = 'todo'", k_now));
    if (!stmt || !stmt->bind_int64(1, args.entity_id) || !stmt->step()) {
      return std::unexpected(aa::agent_error::query_failed);
    }
    if (changed_rows(conn) != 1) {
      return std::unexpected(aa::agent_error::query_failed);
    }

    // The `claim_check` marker: transactional evidence that THIS claim
    // moved the task, which is what `abort` and `reconcile` test before
    // returning the task to `todo`. Left OPEN deliberately — the terminal
    // transaction closes it.
    auto const marker = aa::start_action(conn, aa::start_action_args{
                                                   .session_id  = args.session_id,
                                                   .claim_id    = acquired->id,
                                                   .kind        = aa::action_kind::claim_check,
                                                   .entity      = aa::action_entity_kind::task,
                                                   .entity_id   = args.entity_id,
                                                   .vendor      = args.vendor,
                                                   .vendor_role = args.role,
                                                   .model       = args.model,
                                                   .loc         = args.loc,
                                               });
    if (!marker) {
      return std::unexpected(marker.error());
    }
  }

  auto const committed = commit(*tx);
  if (!committed) {
    return std::unexpected(committed.error());
  }
  return std::move(*acquired);
}

auto pull_next(db::connection& conn, const pull_args& args, const task_policy& policy)
    -> std::expected<pull_result, agent_error> {
  auto tx = conn.begin_transaction(db::lock_mode::immediate);
  if (!tx) {
    return std::unexpected(aa::agent_error::query_failed);
  }

  auto const picked = pick_next_eligible(conn, args.plan_id);
  if (!picked) {
    return std::unexpected(picked.error());
  }
  if (!picked->has_value()) {
    // Commits an EMPTY transaction and writes nothing. Not a shortcut —
    // committing keeps the (immediate) write lock held for the shortest
    // possible time when the answer is "no work".
    auto const committed = commit(*tx);
    if (!committed) {
      return std::unexpected(committed.error());
    }
    return pull_result{.no_work = true};
  }
  auto const task_id = **picked;

  // The selector already constrained the task to `todo`, so this guard is
  // constant-folded in practice — it is here because the Zig original runs
  // it, and because a future selector change must not silently drop it.
  auto const allowed = policy.check_transition("todo", "doing");
  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  auto acquired = aa::acquire_claim(conn, aa::acquire_args{
                                              .session_id        = args.session_id,
                                              .kind              = aa::entity_kind::task,
                                              .entity_id         = task_id,
                                              .vendor            = args.vendor,
                                              .vendor_session_id = args.vendor_session_id,
                                              .role              = args.role,
                                              .worktree_id       = args.worktree_id,
                                              .worktree_path     = args.worktree_path,
                                              .purpose           = args.purpose,
                                              .base_ref          = args.base_ref,
                                              .ttl_secs          = args.ttl_secs,
                                              .loc               = args.loc,
                                              .run_id            = args.run_id,
                                              .stage             = args.stage,
                                          });
  if (!acquired) {
    return std::unexpected(acquired.error());
  }

  auto const flipped = set_task_status(conn, task_id, "doing");
  if (!flipped) {
    return std::unexpected(flipped.error());
  }

  auto const action_id = aa::start_action(conn, aa::start_action_args{
                                                    .session_id       = args.session_id,
                                                    .parent_action_id = args.parent_action_id,
                                                    .claim_id         = acquired->id,
                                                    .kind             = args.kind,
                                                    .entity           = aa::action_entity_kind::task,
                                                    .entity_id        = task_id,
                                                    .vendor           = args.vendor,
                                                    .vendor_role      = args.role,
                                                    .loc              = args.loc,
                                                    .metadata         = args.metadata,
                                                });
  if (!action_id) {
    return std::unexpected(action_id.error());
  }

  auto const committed = commit(*tx);
  if (!committed) {
    return std::unexpected(committed.error());
  }
  return pull_result{.no_work = false, .acquired = std::move(*acquired), .task_id = task_id, .action_id = *action_id};
}

auto peek_next(db::connection& conn, std::int64_t plan_id) -> std::expected<peek_result, agent_error> {
  auto const picked = pick_next_eligible(conn, plan_id);
  if (!picked) {
    return std::unexpected(picked.error());
  }
  if (!picked->has_value()) {
    return peek_result{.no_work = true};
  }
  return peek_result{.no_work = false, .task_id = **picked};
}

// =========================================================================
// Terminal verbs
// =========================================================================

auto complete_work(db::connection& conn, std::string_view claim_token, std::optional<std::string_view> summary,
                   const task_policy& policy) -> std::expected<terminal_result, agent_error> {
  return terminal_transition(conn,
                             terminal_args{
                                 .claim_token = claim_token,
                                 .summary     = summary,
                                 .task_to     = "done",
                                 .claim_to    = aa::claim_status::completed,
                                 .result      = aa::outcome::ok,
                                 .reason      = std::nullopt,
                                 .category    = std::nullopt,
                             },
                             policy);
}

auto fail_work(db::connection& conn, std::string_view claim_token, std::string_view reason, failure_category category,
               const task_policy& policy) -> std::expected<terminal_result, agent_error> {
  // `summary` stays unset while `reason` carries the text: the action's
  // summary is `coalesce(?, summary)`, so passing null PRESERVES whatever
  // the worker already recorded, and the reason lands on the claim
  // instead. `complete` is the only verb that overwrites the summary.
  return terminal_transition(conn,
                             terminal_args{
                                 .claim_token = claim_token,
                                 .summary     = std::nullopt,
                                 .task_to     = "todo",
                                 .claim_to    = aa::claim_status::aborted,
                                 .result      = aa::outcome::error_,
                                 .reason      = reason,
                                 .category    = category,
                             },
                             policy);
}

auto release_work(db::connection& conn, std::string_view claim_token, std::optional<std::string_view> reason,
                  const task_policy& policy) -> std::expected<terminal_result, agent_error> {
  return terminal_transition(conn,
                             terminal_args{
                                 .claim_token = claim_token,
                                 .summary     = std::nullopt,
                                 .task_to     = "todo",
                                 .claim_to    = aa::claim_status::released,
                                 .result      = aa::outcome::aborted,
                                 .reason      = reason,
                                 .category    = std::nullopt,
                             },
                             policy);
}

auto block_work(db::connection& conn, std::string_view claim_token, std::int64_t blocker_task_id,
                std::optional<std::string_view> reason, const task_policy& policy)
    -> std::expected<terminal_result, agent_error> {
  auto tx = conn.begin_transaction(db::lock_mode::immediate);
  if (!tx) {
    return std::unexpected(aa::agent_error::query_failed);
  }

  auto held = aa::get_claim_by_token(conn, claim_token);
  if (!held) {
    return std::unexpected(held.error());
  }

  // NOTE the asymmetry with `terminal_transition`: that one tests
  // `status == active` AND the lease; this one tests only the lease. The
  // SQL guard makes them equivalent (the predicate includes
  // `status='active'`), and the Zig original differs the same way.
  auto const live = aa::is_claim_active_unexpired(conn, claim_token);
  if (!live) {
    return std::unexpected(live.error());
  }
  if (!*live) {
    return std::unexpected(aa::agent_error::claim_not_active);
  }
  if (held->kind != aa::entity_kind::task) {
    return std::unexpected(aa::agent_error::claim_not_on_task);
  }

  // Refuse a dangling blocker BEFORE changing either task or claim. The
  // status text itself is discarded; only the row's existence matters.
  auto const blocker = aa::current_task_status(conn, blocker_task_id);
  if (!blocker) {
    return std::unexpected(blocker.error());
  }

  auto const current = aa::current_task_status(conn, held->entity_id);
  if (!current) {
    return std::unexpected(current.error());
  }
  auto const allowed = policy.check_transition(*current, "blocked");
  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  auto const plan_id = aa::task_plan_id(conn, held->entity_id);

  auto edge = conn.prepare("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                           "values ('task', ?, 'task', ?, 'depends-on')");
  if (!edge || !edge->bind_int64(1, held->entity_id) || !edge->bind_int64(2, blocker_task_id) || !edge->step()) {
    return std::unexpected(aa::agent_error::query_failed);
  }

  auto const flipped = set_task_status(conn, held->entity_id, "blocked");
  if (!flipped) {
    return std::unexpected(flipped.error());
  }

  // `reason` doubles as the action summary here — unlike `fail` and
  // `release`, which leave the summary alone. Zig's behavior.
  auto const closed = aa::close_open_actions_for_claim(conn, held->id, aa::outcome::aborted, reason);
  if (!closed) {
    return std::unexpected(closed.error());
  }

  auto released = aa::release_claim(conn, claim_token, aa::claim_status::released, reason, std::nullopt);
  if (!released) {
    return std::unexpected(released.error());
  }

  if (plan_id.has_value()) {
    auto const recomputed = policy.recompute_plan(conn, *plan_id);
    if (!recomputed) {
      return std::unexpected(recomputed.error());
    }
  }

  auto const committed = commit(*tx);
  if (!committed) {
    return std::unexpected(committed.error());
  }
  return terminal_result{.released = std::move(*released), .task_id = held->entity_id};
}

} // namespace planar::engine::runtime::agentatomic
