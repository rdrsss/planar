/// @file agentatomic.cppm
/// @brief `planar.engine.runtime.agentatomic` — the multi-table atomic
/// wrappers that make the claim ritual a ritual: pull / claim, and the four
/// terminal verbs (plan 996, task 6038).
///
/// Behavior-preserving port (D2) of
/// zig/src/engine/runtime/agentactivity/atomic.zig.
///
/// ## The invariant this module exists for
///
/// > The terminal verbs are ATOMIC: they flip the claim's status and the
/// > task's status in ONE transaction.
///
/// This is not an optimisation. `planar task done` followed by
/// `planar-agent release` is the same two writes in two transactions, and
/// a process death between them leaves a `done` task owned by a claim that
/// is still `active` — the task cannot be re-pulled (its status is
/// terminal) and the claim cannot be re-terminalised by anyone but an
/// operator running `abort`. CLAUDE.md § four-binary boundary forbids
/// splitting them for exactly this reason. Every function here opens ONE
/// `BEGIN IMMEDIATE` and commits once; the `db::transaction` destructor
/// rolls back on every early return, so a refusal partway through leaves
/// the claim exactly as it found it.
///
/// `BEGIN IMMEDIATE` rather than plain `BEGIN`, and this is the load-
/// bearing half: `acquire_claim` CHECKS for a live claim and then INSERTS,
/// and a deferred transaction takes no write lock until its first write —
/// leaving a window in which a second process can run its own check
/// between our check and our insert, and both succeed. Immediate takes the
/// lock at `BEGIN`, so the second process fails cleanly at `BEGIN` instead
/// of quietly double-claiming. `agentatomic.t.cpp`'s two-connection cases
/// are what hold that down.
///
/// ## Why the task policy is injected rather than imported
///
/// The Zig original calls `policy.status.check` before every
/// `UPDATE tasks SET status`, and `plan.recomputeStatus` after every
/// terminal flip. Both live in the C++ tree already — in
/// `planar.engine.planning.transitions` and `planar.engine.planning.plan`
/// — and both are in a DIFFERENT layer-2 bucket, which D15/D18 forbid this
/// one from importing (`cmake/architecture.cmake` FATALs at configure
/// time on an `engine_* -> engine_*` edge).
///
/// The alternatives were: duplicate the task transition matrix and the
/// plan roll-up aggregate here (two policies in two places, free to
/// drift), or extract them down to layer 1 under D19 (they are planning
/// POLICY, not a shared primitive like `scope_ref` or `json_text` — the
/// wrong shape for that move). So `task_policy` takes them as callables
/// and layer 3 supplies the real ones. That is not a workaround: it makes
/// the thing this module must PROVE — "the guard is consulted before the
/// UPDATE, and the roll-up runs inside the same transaction" — directly
/// observable, because a test can pass a recording probe and assert on the
/// exact `(from, to)` pairs and on whether the recompute saw the
/// uncommitted row.
///
/// ## What is NOT here
///
/// `sessioncommits.recordClaimWindowBestEffort`, which the four terminal
/// handlers call AFTER their transaction to harvest the commits made
/// during the claim window. It shells `git` over a revision walk; the same
/// call `engine/runtime` already deferred for `capture commits` and
/// `engine/runs` deferred for `bench harvest`. Deferred with its
/// dependency, and it is outside the transaction in the original too, so
/// nothing atomic depends on it.
module;

export module planar.engine.runtime.agentatomic;

import std;
import planar.db;
import planar.engine.runtime.agentactivity;

namespace planar::engine::runtime::agentatomic {

using agentactivity::action_kind;
using agentactivity::agent_error;
using agentactivity::claim;
using agentactivity::claim_scope;
using agentactivity::entity_kind;
using agentactivity::failure_category;
using agentactivity::locality;

/// @brief The two planning-policy decisions this module must make but may
/// not own. See the file header for why these are callables.
///
/// Both are REQUIRED — neither has a permissive default, deliberately. A
/// defaulted `check_transition` that returned success would turn every
/// status guard in this module into a no-op, and nothing in the type
/// system would say so.
export struct task_policy {
  /// @brief Validate a `tasks.status` transition.
  ///
  /// Layer 3 binds this to `planar.engine.planning.transitions`'
  /// `check_transition(transition_kind::task, from, to, false)`. Returns
  /// `agent_error::illegal_transition` or `agent_error::unknown_status`
  /// on refusal.
  std::function<std::expected<void, agent_error>(std::string_view from, std::string_view to)> check_transition;

  /// @brief Recompute a plan's roll-up status.
  ///
  /// Layer 3 binds this to `planar.engine.planning.plan`'s
  /// `recompute_status`. Called INSIDE the terminal transaction, so it
  /// sees the task flip this transaction just made — which is the whole
  /// point: a plan whose last task just completed must not report its old
  /// status to the next reader.
  std::function<std::expected<void, agent_error>(db::connection& conn, std::int64_t plan_id)> recompute_plan;
};

// =========================================================================
// Claim acquisition
// =========================================================================

/// @brief Acquire a direct entity claim, optionally transitioning the task.
///
/// **`transition_task` defaults to ON at the CLI, and this matters.** A
/// `planar-agent claim --entity task:<id>` moves the task `todo -> doing`
/// as part of the same transaction; `--no-transition` is the explicit
/// escape hatch that yields the bare claim primitive. Verified directly
/// against the oracle rather than assumed: a `todo` task claimed with no
/// flags came back `doing`.
///
/// Two consequences follow, and both are observable:
///
/// - The eligible-status set is EXACTLY `{todo}`, not "anything the
///   transition matrix allows". Direct dispatch is an entry into work, not
///   a reopen. So `claim --entity` on a task already `doing` — including
///   `--force`, which takes the CLAIM over but still has to make the task
///   transition — fails `illegal_transition`, which is what the oracle
///   returns for `claim --entity task:<doing> --force`.
/// - A transitioning claim writes an OPEN `claim_check` action row. That
///   row is the transactional evidence that THIS claim moved the task, and
///   it is what lets `abort` and `reconcile` decide whether returning the
///   task to `todo` is theirs to do. It is deliberately left open for the
///   terminal transaction to close; it is not a synthetic terminal event.
///
/// Plan and plan_step claims never touch entity state, whatever
/// `transition_task` says.
/// @param conn An open, migrated connection (this function owns the transaction).
/// @param args The claim to acquire.
/// @param transition_task Whether a task claim also flips `todo -> doing`.
/// @param policy The injected planning policy (only `check_transition` is used).
/// @return The acquired claim, or `claim_contention` / `illegal_transition` /
/// `task_not_found` / `query_failed`.
export auto claim_entity(db::connection& conn, const agentactivity::acquire_args& args, bool transition_task,
                         const task_policy& policy) -> std::expected<claim, agent_error>;

/// @brief Arguments to `pull_next`. Mirrors zig's `PullArgs`.
export struct pull_args {
  std::int64_t                    plan_id{};                 ///< Plan to pull from.
  std::int64_t                    session_id{};              ///< Owning session.
  std::string_view                vendor;                    ///< Vendor tag.
  std::optional<std::string_view> vendor_session_id;         ///< The vendor's own session id.
  std::optional<std::string_view> role;                      ///< Role name.
  std::optional<std::int64_t>     worktree_id;               ///< Worktree row id.
  std::optional<std::string_view> worktree_path;             ///< Worktree path.
  std::optional<std::string_view> purpose;                   ///< Free-text purpose.
  std::optional<std::string_view> base_ref;                  ///< Git ref.
  std::int64_t                    ttl_secs = 600;            ///< Lease TTL in seconds.
  locality                        loc;                       ///< Git snapshot.
  action_kind                     kind = action_kind::coder; ///< Action kind for the dispatch row.
  std::optional<std::string_view> metadata;                  ///< Opaque text for the action row.
  std::optional<std::int64_t>     parent_action_id;          ///< Parent action, for the cross-session tree.
  std::optional<std::int64_t>     run_id;                    ///< `workflow_runs` FK.
  std::optional<std::string_view> stage;                     ///< Workflow stage name.
};

/// @brief What `pull_next` did.
export struct pull_result {
  bool                 no_work = false; ///< True when nothing was eligible; every other field is then unset.
  std::optional<claim> acquired;        ///< The claim taken.
  std::int64_t         task_id   = 0;   ///< The task claimed.
  std::int64_t         action_id = 0;   ///< The dispatch action opened.
};

/// @brief Pick the next eligible task on a plan, claim it, flip it to
/// `doing`, and open a dispatch action — all in one transaction.
///
/// Eligibility, verbatim from the selector: `tasks.plan_id` matches,
/// `tasks.status = 'todo'`, and NO live claim holds the task. Ordered
/// `priority asc, id asc` — LOWER priority integers win, which reads
/// backwards until you notice `priority` is a rank, not a weight.
///
/// Single plan only: there is NO child-plan recursion here, unlike the
/// `next_work` selector `planar plan next` uses. Pulling from an anchor
/// plan therefore returns nothing when all its own direct tasks are taken,
/// even if a milestone child has work.
///
/// The nothing-eligible path COMMITS an empty transaction and returns
/// `no_work` — it writes not one row, which is what makes `peek`'s "same
/// query, no writes" claim checkable against it.
/// @param conn An open, migrated connection (this function owns the transaction).
/// @param args The pull parameters.
/// @param policy The injected planning policy (only `check_transition` is used).
/// @return The pull outcome, or `claim_contention` / `illegal_transition` /
/// `query_failed`.
export auto pull_next(db::connection& conn, const pull_args& args, const task_policy& policy)
    -> std::expected<pull_result, agent_error>;

/// @brief What `peek_next` found.
export struct peek_result {
  bool         no_work = false; ///< True when nothing was eligible.
  std::int64_t task_id = 0;     ///< The task `pull` would take.
};

/// @brief Run `pull_next`'s selector and nothing else.
///
/// No transaction, no writes, no claim. Deliberately shares the selector
/// with `pull_next` rather than reimplementing it, so the two cannot
/// disagree about what "next" means.
/// @param conn An open, migrated connection.
/// @param plan_id The plan to look at.
/// @return The peek outcome, or `query_failed`.
export auto peek_next(db::connection& conn, std::int64_t plan_id) -> std::expected<peek_result, agent_error>;

// =========================================================================
// Engine supervision (plan 1033 D3/D4, task 6488)
// =========================================================================
//
// D3: the orchestrator creates a claim; once it is handed to the engine
// (`associate_supervisor`), the engine ALONE extends its lease and issues its
// one terminal verb. The caller may still report status. D4: every engine
// verb names the Centurion attempt it acts for, and a terminal verb repeated
// under the attempt that already terminated the claim is a no-op success,
// so post-crash reconciliation can re-issue it safely.
//
// A caller-supervised claim — every claim that is never associated — takes
// every path exactly as before; a default `supervisor_gate` changes nothing.

/// @brief Who is issuing a supervised verb.
export enum class actor : std::uint8_t {
  caller, ///< The orchestrator or coder: the default, and the only actor before plan 1033.
  engine, ///< The engine supervisor, acting for one Centurion attempt.
};

/// @brief The supervision half of a terminal verb's arguments.
export struct supervisor_gate {
  actor                           as = actor::caller; ///< Who is issuing the verb.
  std::optional<std::string_view> attempt;            ///< The attempt, required when `as == engine`.
  /// Operator recovery: a CALLER terminal verb on an engine claim, recorded
  /// as a `supervisor_override` action. Meaningless for the engine.
  bool override_supervisor = false;
};

/// @brief What `associate_supervisor` did.
export struct associate_result {
  claim                      held;       ///< The claim, unchanged by the association itself.
  bool                       engine;     ///< The claim's supervisor is now the engine.
  std::optional<std::string> attempt_id; ///< The attempt now associated.
  bool                       changed;    ///< False for a repeat under the same attempt (a no-op).
};

/// @brief Hand a live claim to the engine supervisor under `attempt` (or
/// confirm it is still the caller's).
///
///   - caller claim, `engine = true`: becomes engine-supervised under
///     `attempt`; one `claim_associate` action is written.
///   - engine claim, same attempt: no change, success (idempotent).
///   - engine claim, different attempt: `attempt_id` moves to the new
///     attempt (Centurion retried); one `claim_associate` action is written.
///   - engine claim, `engine = false`: `supervisor_mismatch` — one-way.
///   - caller claim, `engine = false`: no change, success.
/// @param conn An open, migrated connection (this function owns the transaction).
/// @param claim_token The claim.
/// @param engine True to associate with the engine; false to assert `caller`.
/// @param attempt The attempt; required when `engine` is true.
/// @return The outcome, or `claim_not_found` / `claim_not_active` /
/// `supervisor_mismatch` / `attempt_mismatch` (engine without an attempt) /
/// `query_failed`.
export auto associate_supervisor(db::connection& conn, std::string_view claim_token, bool engine,
                                 std::optional<std::string_view> attempt) -> std::expected<associate_result, agent_error>;

/// @brief Heartbeat a claim with supervision enforced.
///
///   - caller claim, caller actor: exactly the pre-plan-1033 heartbeat.
///   - engine claim, engine actor with the associated attempt: extends the lease.
///   - engine claim, caller actor, `status` given, no `ttl`: STATUS ONLY —
///     the status action is written and the lease is left exactly as it was.
///   - engine claim, caller actor, no `status` or a `ttl`: `supervisor_mismatch`.
///   - caller claim, engine actor: `supervisor_mismatch`.
///
/// `status`, when given, is recorded as a closed `heartbeat` action whose
/// summary is the status text, as the unsupervised heartbeat always did.
/// @param conn An open, migrated connection (this function owns the transaction).
/// @param claim_token The claim.
/// @param ttl_secs As `heartbeat_claim`.
/// @param status The status text, or unset.
/// @param gate Who is heartbeating.
/// @return The claim as it now stands, or the refusal.
export auto supervised_heartbeat(db::connection& conn, std::string_view claim_token, std::optional<std::int64_t> ttl_secs,
                                 std::optional<std::string_view> status, const supervisor_gate& gate)
    -> std::expected<claim, agent_error>;

// =========================================================================
// Terminal verbs
// =========================================================================

/// @brief What a terminal verb did.
export struct terminal_result {
  claim        released;    ///< The claim in its new terminal state.
  std::int64_t task_id = 0; ///< The task it held.
  /// An engine verb repeated under the attempt that already terminated the
  /// claim: nothing was written, and `released` is the claim as it stands.
  bool replayed = false;
};

/// @brief End the work session successfully: task -> `done`, claim ->
/// `completed`, open actions -> `ok`.
///
/// All four terminal verbs take a `supervisor_gate` (default: the caller)
/// and check it inside the same transaction, BEFORE the liveness check so an
/// engine replay under the terminating attempt can succeed as a no-op:
/// engine actor on a caller claim, or caller actor on an engine claim
/// without `override_supervisor`, is `supervisor_mismatch`; an engine actor
/// naming another attempt is `attempt_mismatch`. A successful engine verb
/// writes one `claim_terminal` action; an override writes one
/// `supervisor_override` action.
///
/// Note which guard bites first for the common mistake: a claim taken with
/// `--no-transition` leaves its task in `todo`, and `todo -> done` is not
/// a legal edge, so `complete` on such a claim fails `illegal_transition`
/// with the claim untouched — the transaction rolls back whole.
/// @param conn An open, migrated connection (this function owns the transaction).
/// @param claim_token The claim to end.
/// @param summary The completion summary to record on the actions, or unset.
/// @param policy The injected planning policy.
/// @param gate Who is issuing the verb (default: the caller); see `supervisor_gate`.
/// @return The outcome, or `claim_not_found` / `claim_not_active` /
/// `claim_not_on_task` / `illegal_transition` / `task_not_found` /
/// `query_failed`.
export auto complete_work(db::connection& conn, std::string_view claim_token, std::optional<std::string_view> summary,
                          const task_policy& policy, const supervisor_gate& gate = {})
    -> std::expected<terminal_result, agent_error>;

/// @brief Fail the work session: task -> `todo`, claim -> `aborted` with a
/// failure category, open actions -> `error`.
/// @param conn An open, migrated connection (this function owns the transaction).
/// @param claim_token The claim to end.
/// @param reason The failure reason, recorded on the claim.
/// @param category The closed failure category.
/// @param policy The injected planning policy.
/// @param gate Who is issuing the verb (default: the caller); see `supervisor_gate`.
/// @return The outcome, or the same errors as `complete_work`.
export auto fail_work(db::connection& conn, std::string_view claim_token, std::string_view reason, failure_category category,
                      const task_policy& policy, const supervisor_gate& gate = {}) -> std::expected<terminal_result, agent_error>;

/// @brief Give up gracefully: task -> `todo`, claim -> `released`, open
/// actions -> `aborted`.
///
/// Distinct from `fail_work` only in the claim status and the action
/// outcome, and the distinction is the point: `released` says "I chose to
/// stop", `aborted` says "something went wrong". A sweep over
/// `failure_category` counts the second and not the first.
/// @param conn An open, migrated connection (this function owns the transaction).
/// @param claim_token The claim to end.
/// @param reason The reason, or unset.
/// @param policy The injected planning policy.
/// @param gate Who is issuing the verb (default: the caller); see `supervisor_gate`.
/// @return The outcome, or the same errors as `complete_work`.
export auto release_work(db::connection& conn, std::string_view claim_token, std::optional<std::string_view> reason,
                         const task_policy& policy, const supervisor_gate& gate = {})
    -> std::expected<terminal_result, agent_error>;

/// @brief Park the task on a blocker: write a `depends-on` edge, task ->
/// `blocked`, claim -> `released`, open actions -> `aborted`.
///
/// Refuses a dangling `blocker_task_id` BEFORE touching either the task or
/// the claim (`task_not_found`), so a typo in the blocker id cannot leave
/// the claim released against a task that was never blocked.
///
/// The `entity_links` insert carries the table's UNIQUE constraint, so
/// blocking twice on the same blocker fails `query_failed` and the whole
/// verb rolls back — the second `block` is not idempotent.
/// @param conn An open, migrated connection (this function owns the transaction).
/// @param claim_token The claim to end.
/// @param blocker_task_id The task id to depend on.
/// @param reason The reason, recorded on the claim AND as the action summary.
/// @param policy The injected planning policy.
/// @param gate Who is issuing the verb (default: the caller); see `supervisor_gate`.
/// @return The outcome, or the same errors as `complete_work`.
export auto block_work(db::connection& conn, std::string_view claim_token, std::int64_t blocker_task_id,
                       std::optional<std::string_view> reason, const task_policy& policy, const supervisor_gate& gate = {})
    -> std::expected<terminal_result, agent_error>;

} // namespace planar::engine::runtime::agentatomic
