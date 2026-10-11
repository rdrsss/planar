/// @file agentatomic.cpp
/// @brief Implementation of `planar.engine.runtime.agentatomic`. See the
/// module interface for the atomicity contract and the injected-policy
/// argument.

module planar.engine.runtime.agentatomic;

import std;
import planar.db;
import planar.json_text;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.session;

namespace planar::engine::runtime::agentatomic {

namespace aa  = agentactivity;
namespace ses = ::planar::engine::runtime::session;

namespace {

constexpr std::string_view k_now = "strftime('%Y-%m-%dT%H:%M:%fZ','now')";

/// @brief Classify a failed `begin_transaction(lock_mode::immediate)` call
/// (task 6843): a competing writer that still holds the lock after this
/// connection's own `busy_timeout` window elapsed is `agent_error::busy`,
/// never `query_failed` -- a caller (an orchestrator, or an operator
/// reading the CLI's `Busy` tag) should retry, not escalate.
/// @param err The failure `begin_transaction` returned.
/// @return `agent_error::busy` for a post-timeout SQLITE_BUSY, else
/// `agent_error::query_failed`.
auto classify_begin_failure(const db::db_error& err) -> aa::agent_error {
  return db::is_busy(err) ? aa::agent_error::busy : aa::agent_error::query_failed;
}

/// @brief The shared eligibility selector behind BOTH `pull_next` and
/// `peek_next`.
///
/// One definition on purpose: `peek` documents itself as "the same query
/// as pull, no writes", and that claim is only true if there is literally
/// one query. `order by t.priority asc` — lower integers first.
///
/// **Dependency exclusion (task 6841 / decision D6, closes task 5535).** A
/// candidate is also excluded when it carries an outbound `depends-on`
/// edge (`entity_links`, `task -> task`) to a task whose blocker is not
/// yet terminal. This is the DEPENDENCY EXCLUSION ONLY — D6, verbatim:
/// "Rather than adding a `--respect-deps` flag, pull and peek exclude
/// tasks with an open depends-on blocker. They do NOT adopt `plan next`'s
/// packet-readiness gate." A `depends-on` edge can exist on a `todo` task
/// without the task itself being `blocked` (`task block --on` is not the
/// only writer of that edge — the generic `link` verb is another), so the
/// exclusion cannot ride on `t.status` alone.
///
/// "Terminal" here is deliberately the SAME set `clear_unblocked_dependents`
/// (`planar.engine.planning.task`) already uses to decide a dependent is
/// unblocked: `done` OR `cancelled` (decision 1122). Picking a narrower
/// "only `done` counts" reading would put `pull`/`peek` out of step with
/// the roll-up that already treats a cancelled blocker as satisfied —
/// pinned by `agentatomic.t.cpp`'s selector unit tests.
/// @param conn The connection.
/// @param plan_id The plan to select within.
/// @return Every eligible task id in selection order, or `query_failed`.
auto eligible_task_ids(db::connection& conn, std::int64_t plan_id) -> std::expected<std::vector<std::int64_t>, aa::agent_error> {
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
                                       "  and not exists (\n"
                                       "    select 1 from entity_links el\n"
                                       "    join tasks blocker on blocker.id = el.to_id\n"
                                       "    where el.from_kind = 'task' and el.from_id = t.id\n"
                                       "      and el.to_kind = 'task' and el.relationship = 'depends-on'\n"
                                       "      and blocker.status not in ('done', 'cancelled')\n"
                                       "  )\n"
                                       "order by t.priority asc, t.id asc",
                                       k_now));
  if (!stmt || !stmt->bind_int64(1, plan_id)) {
    return std::unexpected(aa::agent_error::query_failed);
  }
  std::vector<std::int64_t> ids;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(aa::agent_error::query_failed);
    }
    if (*stepped != db::step_result::row) {
      return ids;
    }
    ids.push_back(stmt->column_int64(0));
  }
}

/// @brief What the selector chose once readiness was applied.
struct selection {
  std::optional<std::int64_t> task_id; ///< The first ready eligible task, unset when none.
  std::vector<skipped_task>   skipped; ///< The eligible tasks passed over before it, with their reasons.
};

/// @brief The first eligible task that is also ready, plus the ones it passed over (plan 1142).
///
/// The one definition behind `pull_next` and `peek_next`. The readiness callable runs on `conn`,
/// so under `pull_next` it runs inside the claim transaction.
/// @param conn The connection.
/// @param plan_id The plan to select within.
/// @param policy The injected policy; `unready_reasons` must be bound.
/// @return The selection, or `query_failed` (also when no readiness callable is bound) or the
/// callable's own failure.
auto select_next_ready(db::connection& conn, std::int64_t plan_id, const task_policy& policy)
    -> std::expected<selection, aa::agent_error> {
  if (!policy.unready_reasons) {
    return std::unexpected(aa::agent_error::query_failed);
  }
  auto const ids = eligible_task_ids(conn, plan_id);
  if (!ids) {
    return std::unexpected(ids.error());
  }
  selection chosen;
  for (auto const id : *ids) {
    auto reasons = policy.unready_reasons(conn, id);
    if (!reasons) {
      return std::unexpected(reasons.error());
    }
    if (reasons->empty()) {
      chosen.task_id = id;
      return chosen;
    }
    chosen.skipped.push_back(skipped_task{.task_id = id, .reasons = std::move(*reasons)});
  }
  return chosen;
}

/// @brief `["a","b"]`, with `"` and `\` escaped.
/// @param reasons The reason names.
/// @return The JSON array text.
auto reasons_json(const std::vector<std::string>& reasons) -> std::string {
  std::string out   = "[";
  bool        first = true;
  for (auto const& reason : reasons) {
    if (!first) {
      out += ',';
    }
    first = false;
    out += '"';
    for (auto const c : reason) {
      if (c == '"' || c == '\\') {
        out += '\\';
      }
      out += c;
    }
    out += '"';
  }
  out += ']';
  return out;
}

/// @brief `a, b, c`.
/// @param reasons The reason names.
/// @return The reasons joined for a summary line.
auto reasons_text(const std::vector<std::string>& reasons) -> std::string {
  std::string out;
  for (auto const& reason : reasons) {
    if (!out.empty()) {
      out += ", ";
    }
    out += reason;
  }
  return out;
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
  std::string_view                    verb; ///< `complete` / `fail` / `release`, for the audit row.
  std::string_view                    claim_token;
  std::optional<std::string_view>     summary;  ///< Written onto the closed actions.
  std::string_view                    task_to;  ///< New `tasks.status`.
  aa::claim_status                    claim_to; ///< New claim status.
  aa::outcome                         result;   ///< Outcome written onto the closed actions.
  std::optional<std::string_view>     reason;   ///< Written onto the claim's `release_reason`.
  std::optional<aa::failure_category> category; ///< Written onto the claim.
};

/// @brief The task a claim holds: its entity id for a task claim, unset for
/// a plan / plan_step claim, whose entity id is a plan or step id and must
/// never be read as a task id (task 7118).
/// @param held The claim.
/// @return The task id, or unset.
auto held_task_id(const aa::claim& held) -> std::optional<std::int64_t> {
  if (held.kind != aa::entity_kind::task) {
    return std::nullopt;
  }
  return held.entity_id;
}

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
    return std::unexpected(classify_begin_failure(tx.error()));
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

  // Task 6890 / decision D-6890: `release` is the one terminal verb with
  // NO entity transition to perform on a plan/plan_step claim -- it is the
  // graceful give-up verb, and giving up a plan-level claim needs nothing
  // more than flipping the CLAIM to released. `complete`/`fail` still
  // refuse `ClaimNotOnTask` unconditionally: they exist specifically to
  // move a TASK's status, which a plan/plan_step claim has none of.
  // `block_work` never reaches this function at all (see its own
  // `ClaimNotOnTask` guard below). This is why a plan-level orchestrator
  // claim used to be endable ONLY by lease expiry -- there was no verb
  // that could terminate it deliberately.
  std::optional<std::int64_t> plan_id;
  if (held->kind != aa::entity_kind::task) {
    if (targs.verb != "release") {
      return std::unexpected(aa::agent_error::claim_not_on_task);
    }
    // No task/plan status write, no transition-matrix check, no
    // recompute -- there is no task row this claim covers.
  } else {
    auto const current = aa::current_task_status(conn, held->entity_id);
    if (!current) {
      return std::unexpected(current.error());
    }
    auto const allowed = policy.check_transition(*current, targs.task_to);
    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    // Captured BEFORE the flip, because the recompute below needs it and
    // the claim row is about to be replaced by its released form.
    plan_id = aa::task_plan_id(conn, held->entity_id);

    auto const flipped = set_task_status(conn, held->entity_id, targs.task_to);
    if (!flipped) {
      return std::unexpected(flipped.error());
    }

    // Mirrors `planar.engine.planning.task`'s own terminal paths
    // (`mark_done`, `mark_cancelled`, and `update_task` on a terminal
    // patch): the roll-up runs immediately after the flip, in the same
    // transaction, and only when the flip actually landed on a terminal
    // status. `complete` is the only terminal verb this module has that
    // does (`fail`/`release` return the task to `todo`; `block_work`
    // never reaches this function) — task 6875.
    if (targs.task_to == "done") {
      if (auto const unblocked = policy.clear_unblocked_dependents(conn, held->entity_id); !unblocked) {
        return std::unexpected(unblocked.error());
      }
    }
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
  return terminal_result{.released = std::move(*released), .task_id = held_task_id(*held)};
}

} // namespace

// =========================================================================
// Claim acquisition
// =========================================================================

auto claim_entity(db::connection& conn, const agentactivity::acquire_args& args, bool transition_task, const task_policy& policy,
                  claim_readiness* readiness) -> std::expected<claim, agent_error> {
  auto tx = conn.begin_transaction(db::lock_mode::immediate);
  if (!tx) {
    return std::unexpected(classify_begin_failure(tx.error()));
  }

  // Admission runs between the exclusivity check and the INSERT, on this connection and inside this
  // transaction (plan 1142): the status guard, then readiness. The status guard moved here from
  // after the insert so that `illegal_transition` still outranks readiness (an `invalid_task_status`
  // reason must not mask it); with the refusal returned before the insert, nothing is written
  // either way.
  std::vector<std::string> overridden;
  auto                     gated = args;
  gated.admit                    = [&](db::connection& c) -> std::expected<void, aa::agent_error> {
    if (args.admit) {
      if (auto const prior = args.admit(c); !prior) {
        return std::unexpected(prior.error());
      }
    }
    if (args.kind != aa::entity_kind::task) {
      return {};
    }
    if (transition_task) {
      auto const current = aa::current_task_status(c, args.entity_id);
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
    }
    if (!policy.unready_reasons) {
      return std::unexpected(aa::agent_error::query_failed);
    }
    auto reasons = policy.unready_reasons(c, args.entity_id);
    if (!reasons) {
      return std::unexpected(reasons.error());
    }
    if (reasons->empty()) {
      return {};
    }
    if (readiness != nullptr) {
      readiness->unready_reasons = *reasons;
    }
    if (readiness == nullptr || !readiness->allow_unready) {
      return std::unexpected(aa::agent_error::task_not_ready);
    }
    overridden = std::move(*reasons);
    return {};
  };

  auto acquired = aa::acquire_claim(conn, gated);
  if (!acquired) {
    return std::unexpected(acquired.error());
  }

  if (args.kind == aa::entity_kind::task && transition_task) {
    // The `and status = 'todo'` predicate is a SECOND read of the same
    // fact the admission guard checked, and the row-count check below is what
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
                                                   .session_id       = args.session_id,
                                                   .parent_action_id = args.parent_action_id,
                                                   .claim_id         = acquired->id,
                                                   .kind             = aa::action_kind::claim_check,
                                                   .entity           = aa::action_entity_kind::task,
                                                   .entity_id        = args.entity_id,
                                                   .vendor           = args.vendor,
                                                   .vendor_role      = args.role,
                                                   .model            = args.model,
                                                   .loc              = args.loc,
                                               });
    if (!marker) {
      return std::unexpected(marker.error());
    }
  } else if (args.parent_action_id.has_value()) {
    // No `claim_check` marker exists for a bare (`--no-transition`) or non-task claim, and it must not:
    // `abort` and `reconcile` read that kind as proof the claim moved the task. A parent was asked for,
    // so the claim gets its own open `other` action to hang under it; the terminal verb closes it.
    auto const entity = args.kind == aa::entity_kind::task        ? aa::action_entity_kind::task
                        : args.kind == aa::entity_kind::plan_step ? aa::action_entity_kind::plan_step
                                                                  : aa::action_entity_kind::plan;
    auto const nested = aa::start_action(conn, aa::start_action_args{
                                                   .session_id       = args.session_id,
                                                   .parent_action_id = args.parent_action_id,
                                                   .claim_id         = acquired->id,
                                                   .kind             = aa::action_kind::other,
                                                   .entity           = entity,
                                                   .entity_id        = args.entity_id,
                                                   .vendor           = args.vendor,
                                                   .vendor_role      = args.role,
                                                   .model            = args.model,
                                                   .loc              = args.loc,
                                                   .metadata         = aa::claim_nest_metadata,
                                               });
    if (!nested) {
      return std::unexpected(nested.error());
    }
  }

  // The audit row for an override: one closed `other` action on the claim. `agent_actions.kind` has
  // no kind of its own for it and a migration is out of scope, so the machine-readable marker is the
  // `unready_override` key in `metadata`, repeated in the summary for a reader of `planar-watch`.
  if (!overridden.empty()) {
    auto const metadata = std::format("{{\"unready_override\":true,\"reasons\":{}}}", reasons_json(overridden));
    auto const audit    = aa::start_action(conn, aa::start_action_args{
                                                     .session_id  = args.session_id,
                                                     .claim_id    = acquired->id,
                                                     .kind        = aa::action_kind::other,
                                                     .entity      = aa::action_entity_kind::task,
                                                     .entity_id   = args.entity_id,
                                                     .vendor      = args.vendor,
                                                     .vendor_role = args.role,
                                                     .model       = args.model,
                                                     .loc         = args.loc,
                                                     .metadata    = metadata,
                                                 });
    if (!audit) {
      return std::unexpected(audit.error());
    }
    auto const summary = std::format("unready_override: {}", reasons_text(overridden));
    if (auto const closed = aa::end_action(conn, *audit, aa::outcome::ok, summary); !closed) {
      return std::unexpected(closed.error());
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
    return std::unexpected(classify_begin_failure(tx.error()));
  }

  auto picked = select_next_ready(conn, args.plan_id, policy);
  if (!picked) {
    return std::unexpected(picked.error());
  }
  if (!picked->task_id.has_value()) {
    // Commits an EMPTY transaction and writes nothing. Not a shortcut —
    // committing keeps the (immediate) write lock held for the shortest
    // possible time when the answer is "no work".
    auto const committed = commit(*tx);
    if (!committed) {
      return std::unexpected(committed.error());
    }
    return pull_result{.no_work = true, .skipped = std::move(picked->skipped)};
  }
  auto const task_id = *picked->task_id;

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
                                              .model             = args.model,
                                              .worktree_id       = args.worktree_id,
                                              .worktree_path     = args.worktree_path,
                                              .purpose           = args.purpose,
                                              .base_ref          = args.base_ref,
                                              .ttl_secs          = args.ttl_secs,
                                              .loc               = args.loc,
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
                                                    .model            = args.model,
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
  return pull_result{.no_work   = false,
                     .acquired  = std::move(*acquired),
                     .task_id   = task_id,
                     .action_id = *action_id,
                     .skipped   = std::move(picked->skipped)};
}

auto peek_next(db::connection& conn, std::int64_t plan_id, const task_policy& policy) -> std::expected<peek_result, agent_error> {
  auto picked = select_next_ready(conn, plan_id, policy);
  if (!picked) {
    return std::unexpected(picked.error());
  }
  if (!picked->task_id.has_value()) {
    return peek_result{.no_work = true, .skipped = std::move(picked->skipped)};
  }
  return peek_result{.no_work = false, .task_id = *picked->task_id, .skipped = std::move(picked->skipped)};
}

// =========================================================================
// Terminal verbs
// =========================================================================

auto complete_work(db::connection& conn, std::string_view claim_token, std::optional<std::string_view> summary,
                   const task_policy& policy) -> std::expected<terminal_result, agent_error> {
  return terminal_transition(conn,
                             terminal_args{
                                 .verb        = "complete",
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
                                 .verb        = "fail",
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
                                 .verb        = "release",
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
    return std::unexpected(classify_begin_failure(tx.error()));
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
  return terminal_result{.released = std::move(*released), .task_id = held_task_id(*held)};
}

// =========================================================================
// Engine supervision
// =========================================================================

auto supervised_heartbeat(db::connection& conn, std::string_view claim_token, std::optional<std::int64_t> ttl_secs,
                          std::optional<std::string_view> status, const supervisor_gate& gate)
    -> std::expected<claim, agent_error> {
  auto tx = conn.begin_transaction(db::lock_mode::immediate);
  if (!tx) {
    return std::unexpected(classify_begin_failure(tx.error()));
  }
  auto const sup = aa::get_supervision(conn, claim_token);
  if (!sup) {
    return std::unexpected(sup.error());
  }

  std::expected<claim, agent_error> current = std::unexpected(aa::agent_error::query_failed);
  if (gate.as == actor::engine) {
    if (!sup->engine) {
      return std::unexpected(aa::agent_error::supervisor_mismatch);
    }
    if (!gate.attempt.has_value() || sup->attempt_id != std::string{*gate.attempt}) {
      return std::unexpected(aa::agent_error::attempt_mismatch);
    }
    current = aa::heartbeat_claim(conn, claim_token, ttl_secs);
  } else {
    // A legacy claim whose supervisor is `engine` is an ordinary claim to its
    // caller: the caller heartbeats it, extends its lease and reports status
    // like any other claim, matching the terminal verbs.
    current = aa::heartbeat_claim(conn, claim_token, ttl_secs);
  }
  if (!current) {
    return std::unexpected(current.error());
  }

  if (status.has_value()) {
    auto const action_id = aa::start_action(conn, aa::start_action_args{
                                                      .session_id = current->session_id,
                                                      .claim_id   = current->id,
                                                      .kind       = aa::action_kind::heartbeat,
                                                      .vendor     = current->vendor,
                                                  });
    if (!action_id) {
      return std::unexpected(action_id.error());
    }
    if (auto const closed = aa::end_action(conn, *action_id, aa::outcome::ok, status); !closed) {
      return std::unexpected(closed.error());
    }
  }

  if (auto const committed = commit(*tx); !committed) {
    return std::unexpected(committed.error());
  }
  return current;
}

auto record_review(db::connection& conn, const review_args& args) -> std::expected<review_result, agent_error> {
  auto tx = conn.begin_transaction(db::lock_mode::immediate);
  if (!tx) {
    return std::unexpected(classify_begin_failure(tx.error()));
  }

  auto reviewed = aa::get_claim_by_token(conn, args.reviewed_claim_token);
  if (!reviewed) {
    return std::unexpected(reviewed.error());
  }

  std::optional<aa::claim> reviewer;
  if (args.reviewer_claim_token.has_value()) {
    auto found = aa::get_claim_by_token(conn, *args.reviewer_claim_token);
    if (!found) {
      return std::unexpected(found.error() == aa::agent_error::claim_not_found ? aa::agent_error::reviewer_claim_not_found
                                                                               : found.error());
    }
    reviewer = std::move(*found);
  }

  // Who is calling: the reviewer claim's vendor session when it has one, else the one the caller
  // passed. A reviewer claim without a vendor session still identifies the caller, by its token.
  std::optional<std::string> caller_session;
  if (reviewer.has_value() && reviewer->vendor_session_id.has_value()) {
    caller_session = reviewer->vendor_session_id;
  } else if (args.reviewer_vendor_session.has_value()) {
    caller_session = std::string{*args.reviewer_vendor_session};
  }
  if (!reviewer.has_value() && !caller_session.has_value()) {
    return std::unexpected(aa::agent_error::reviewer_unidentified);
  }

  // The refusals. A claim cannot review itself, nor can its sessions row.
  if (reviewer.has_value() && (reviewer->id == reviewed->id || reviewer->session_id == reviewed->session_id)) {
    return std::unexpected(aa::agent_error::reviewer_not_independent);
  }
  if (reviewed->vendor_session_id.has_value()) {
    if (caller_session.has_value() && *caller_session == *reviewed->vendor_session_id) {
      return std::unexpected(aa::agent_error::reviewer_not_independent);
    }
  } else if (!reviewer.has_value() || !reviewer->vendor_session_id.has_value()) {
    // Nothing to compare the caller against: the reviewed claim has no vendor session, so only a
    // reviewer claim that carries one proves the caller is somebody else.
    return std::unexpected(aa::agent_error::reviewer_not_independent);
  }

  std::int64_t reviewer_session_row = 0;
  if (reviewer.has_value()) {
    reviewer_session_row = reviewer->session_id;
  } else {
    std::string_view const vendor  = args.vendor.empty() ? std::string_view{"planar-agent"} : args.vendor;
    auto const             ensured = ses::ensure_active(conn, vendor, std::string_view{*caller_session});
    if (!ensured) {
      return std::unexpected(aa::agent_error::query_failed);
    }
    reviewer_session_row = *ensured;
  }

  // The reviewed claim's earliest action that is not itself a review: the work the verdict is about.
  std::optional<std::int64_t> parent;
  {
    auto stmt = conn.prepare("select id from agent_actions where claim_id = ? and action_kind <> 'review'\n"
                             "order by id asc limit 1");
    if (!stmt || !stmt->bind_int64(1, reviewed->id)) {
      return std::unexpected(aa::agent_error::query_failed);
    }
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(aa::agent_error::query_failed);
    }
    if (*stepped == db::step_result::row) {
      parent = stmt->column_int64(0);
    }
  }

  std::string metadata = "{";
  if (caller_session.has_value()) {
    metadata += "\"reviewer_session\":";
    json_text::append_json_string(metadata, *caller_session);
  }
  if (reviewer.has_value()) {
    metadata += metadata.size() > 1 ? ",\"reviewer_claim\":" : "\"reviewer_claim\":";
    json_text::append_json_string(metadata, reviewer->claim_token);
  }
  metadata += '}';

  auto const                      entity = reviewed->kind == aa::entity_kind::task        ? aa::action_entity_kind::task
                                           : reviewed->kind == aa::entity_kind::plan_step ? aa::action_entity_kind::plan_step
                                                                                          : aa::action_entity_kind::plan;
  std::string_view const          vendor = reviewer.has_value()   ? std::string_view{reviewer->vendor}
                                           : !args.vendor.empty() ? args.vendor
                                                                  : std::string_view{"planar-agent"};
  std::optional<std::string_view> model;
  if (reviewer.has_value() && reviewer->model.has_value()) {
    model = std::string_view{*reviewer->model};
  }
  auto const action_id = aa::start_action(conn, aa::start_action_args{
                                                    .session_id       = reviewer_session_row,
                                                    .parent_action_id = parent,
                                                    .claim_id         = reviewed->id,
                                                    .kind             = aa::action_kind::review,
                                                    .entity           = entity,
                                                    .entity_id        = reviewed->entity_id,
                                                    .vendor           = vendor,
                                                    .vendor_role      = std::string_view{"reviewer"},
                                                    .model            = model,
                                                    .metadata         = metadata,
                                                });
  if (!action_id) {
    return std::unexpected(action_id.error());
  }
  if (auto const closed = aa::end_action(conn, *action_id, aa::outcome::ok, args.summary); !closed) {
    return std::unexpected(closed.error());
  }
  auto stamp = conn.prepare("update agent_actions set verdict = ? where id = ?");
  if (!stamp || !stamp->bind_text(1, aa::to_text(args.verdict)) || !stamp->bind_int64(2, *action_id) || !stamp->step()) {
    return std::unexpected(aa::agent_error::query_failed);
  }

  if (auto const committed = commit(*tx); !committed) {
    return std::unexpected(committed.error());
  }
  return review_result{
      .action_id = *action_id, .parent_action_id = parent, .verdict = args.verdict, .reviewed = std::move(*reviewed)};
}

} // namespace planar::engine::runtime::agentatomic
