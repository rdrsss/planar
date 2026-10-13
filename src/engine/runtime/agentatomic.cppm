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

/// @brief What `task_policy::claim_policy` is asked about: one claim about to be taken.
export struct claim_request {
  entity_kind  kind = entity_kind::task; ///< What is claimed; `plan` for a `pull`, whose task is not yet chosen.
  std::int64_t entity_id{};              ///< Which one.
  std::optional<std::string_view> role;  ///< The claim's role, when given.
  std::optional<std::string_view> model; ///< The claim's model, when given.
};

/// @brief The four planning-policy decisions this module must make but may
/// not own. See the file header for why these are callables.
///
/// All four are REQUIRED — none has a permissive default, deliberately. A
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

  /// @brief Clear `blocked` on every dependent of a task that just reached
  /// `done` (task 6875).
  ///
  /// Layer 3 binds this to `planar.engine.planning.task`'s
  /// `clear_unblocked_dependents`. Called INSIDE the terminal transaction,
  /// immediately after the flip to `done`, for the same reason
  /// `recompute_plan` is: `planar task done` and `planar-agent complete`
  /// are two different entry points onto the same "a task just went
  /// terminal" event, and a dependent left stale at `blocked` because it
  /// happened to unblock through the agent plane rather than the
  /// operator-facing verb is the defect this callable exists to close.
  std::function<std::expected<void, agent_error>(db::connection& conn, std::int64_t blocker_id)> clear_unblocked_dependents;

  /// @brief The readiness reasons that hold a task back from being claimed (plan 1142).
  ///
  /// Layer 3 binds this to the task packet's readiness (`planar.engine.ingest.packet`), the same
  /// verdict `task packet` and `plan next` report. An empty list means ready. The reasons are the
  /// packet's stable names (`blocked_by_open_task`, `open_question_linked`, ...). Called INSIDE the
  /// claim or pull transaction, on the connection that inserts the claim, so a link written by
  /// another process cannot slip between this read and the claim.
  std::function<std::expected<std::vector<std::string>, agent_error>(db::connection& conn, std::int64_t task_id)> unready_reasons;

  /// @brief The policy check `complete` runs before it ends a task claim (plan 1142).
  ///
  /// Layer 3 binds this to the configured policy (`review_required`, `task_gate`) and the host queue's
  /// history, neither of which this bucket may import. Called INSIDE the `complete` transaction, on
  /// the connection that ends the claim, after the claim and the task's transition were checked and
  /// before anything is written; it returns `completion_unmet` to refuse, and the refusal rolls the
  /// transaction back so the claim stays active. Called for `complete` only, never for `fail`,
  /// `release` or `block`. Unlike the four callables above it MAY be left unbound: an unbound check
  /// admits every completion, which is how a caller that has no policy (and every engine test) runs.
  std::function<std::expected<void, agent_error>(db::connection& conn, const claim& held)> check_completion;

  /// @brief The policy decision for a claim about to be taken (plan 1142).
  ///
  /// Layer 3 binds this to the configured policy, which this bucket may not import. Called INSIDE the
  /// claim or pull transaction, on the connection that inserts the claim, before the exclusivity check
  /// and before anything is written. It returns `claim_policy_unmet` to refuse, which leaves no claim,
  /// or the digest of the effective policy for the claim's scope, which `claim_entity` and `pull_next`
  /// store as `agent_work_claims.policy_digest`; an unset digest stores NULL. Like `check_completion`
  /// it MAY be left unbound: an unbound decision admits every claim and records no digest.
  std::function<std::expected<std::optional<std::string>, agent_error>(db::connection& conn, const claim_request& request)>
      claim_policy;
};

/// @brief One task that `pull` or `peek` passed over because it was not ready.
export struct skipped_task {
  std::int64_t             task_id = 0; ///< The task skipped.
  std::vector<std::string> reasons;     ///< Its readiness reasons, in the packet's order.
};

/// @brief The readiness half of a task claim: what the caller allows, and what the claim found.
///
/// Owned by the caller for one `claim_entity` call; the engine writes only `unready_reasons`.
export struct claim_readiness {
  /// In: claim an unready task anyway. The override is audited (see `claim_entity`).
  bool allow_unready = false;
  /// Out: the readiness reasons found for the claimed task. Set when the claim was refused with
  /// `task_not_ready`, and when it succeeded only because `allow_unready` overrode them.
  std::vector<std::string> unready_reasons;
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
///
/// **Readiness (plan 1142).** A task claim is also refused with `task_not_ready` when
/// `policy.unready_reasons` returns any reason. The check runs inside this transaction, after the
/// exclusivity check and the `--force` takeover and before the claim row is inserted, so the order
/// of refusals is contention, then (for a transitioning claim) `illegal_transition`, then
/// readiness, and a refusal writes nothing. A task whose claim has lapsed is ready for recovery by
/// the packet's rule; a live claim is `claim_contention` before readiness is asked. When
/// `readiness->allow_unready` is set an unready task is claimed anyway and exactly one closed
/// `other` action is written on the claim, with `unready_override` and the overridden reasons in its
/// summary and metadata. Plan and plan_step claims are not subject to readiness.
///
/// **Claim policy (plan 1142).** `policy.claim_policy`, when bound, is asked first of all, inside the
/// transaction: it may refuse with `claim_policy_unmet` (nothing is written), and the digest it
/// returns is stored on the claim as `policy_digest`. Contention and readiness are consulted after it.
/// @param conn An open, migrated connection (this function owns the transaction).
/// @param args The claim to acquire.
/// @param transition_task Whether a task claim also flips `todo -> doing`.
/// @param policy The injected planning policy.
/// @param readiness The override request and the reasons found, or null for no override and no report.
/// @return The acquired claim, or `claim_contention` / `illegal_transition` /
/// `task_not_ready` / `claim_policy_unmet` / `task_not_found` / `query_failed`.
export auto claim_entity(db::connection& conn, const agentactivity::acquire_args& args, bool transition_task,
                         const task_policy& policy, claim_readiness* readiness = nullptr) -> std::expected<claim, agent_error>;

/// @brief Arguments to `pull_next`. Mirrors zig's `PullArgs`.
export struct pull_args {
  std::int64_t                    plan_id{};         ///< Plan to pull from.
  std::int64_t                    session_id{};      ///< Owning session.
  std::string_view                vendor;            ///< Vendor tag.
  std::optional<std::string_view> vendor_session_id; ///< The vendor's own session id.
  std::optional<std::string_view> role;              ///< Role name.
  std::optional<std::string_view> model;             ///< Model identifier, recorded verbatim on the claim.
  std::optional<std::int64_t>     worktree_id;       ///< Worktree row id.
  std::optional<std::string_view> worktree_path;     ///< Worktree path.
  std::optional<std::string_view> purpose;           ///< Free-text purpose.
  std::optional<std::string_view> base_ref;          ///< Git ref.
  std::int64_t                    ttl_secs = agentactivity::default_lease_secs; ///< Lease TTL in seconds.
  locality                        loc;                                          ///< Git snapshot.
  action_kind                     kind = action_kind::coder;                    ///< Action kind for the dispatch row.
  std::optional<std::string_view> metadata;                                     ///< Opaque text for the action row.
  std::optional<std::int64_t>     parent_action_id;                             ///< Parent action, for the cross-session tree.
};

/// @brief What `pull_next` did.
export struct pull_result {
  bool                 no_work = false; ///< True when nothing was eligible; every other field is then unset.
  std::optional<claim> acquired;        ///< The claim taken.
  std::int64_t         task_id   = 0;   ///< The task claimed.
  std::int64_t         action_id = 0;   ///< The dispatch action opened.
  /// Eligible tasks passed over because they were not ready, in selector order (plan 1142). Set
  /// on `no_work` and on a successful pull alike.
  std::vector<skipped_task> skipped;
};

/// @brief Pick the next eligible task on a plan, claim it, flip it to
/// `doing`, and open a dispatch action — all in one transaction.
///
/// Eligibility, verbatim from the selector: `tasks.plan_id` matches,
/// `tasks.status = 'todo'`, and NO live claim holds the task. Ordered
/// `priority asc, id asc` — LOWER priority integers win, which reads
/// backwards until you notice `priority` is a rank, not a weight.
///
/// **Readiness (plan 1142).** Each candidate is then asked `policy.unready_reasons`, inside the
/// same transaction that inserts the claim. An unready candidate is skipped and recorded in
/// `pull_result::skipped`; the first ready one is claimed. When none is ready the result is
/// `no_work` with the skipped list, and nothing is written.
///
/// Single plan only: there is NO child-plan recursion here, unlike the
/// `next_work` selector `planar plan next` uses. Pulling from an anchor
/// plan therefore returns nothing when all its own direct tasks are taken,
/// even if a milestone child has work.
///
/// **Claim policy (plan 1142).** `policy.claim_policy`, when bound, is asked once with the plan
/// before any candidate is read: a refusal (`claim_policy_unmet`) writes nothing, and the digest it
/// returns is stored on the claim taken, whose scope is the plan's.
///
/// The nothing-eligible path COMMITS an empty transaction and returns
/// `no_work` — it writes not one row, which is what makes `peek`'s "same
/// query, no writes" claim checkable against it.
/// @param conn An open, migrated connection (this function owns the transaction).
/// @param args The pull parameters.
/// @param policy The injected planning policy.
/// @return The pull outcome, or `claim_contention` / `illegal_transition` /
/// `claim_policy_unmet` / `query_failed`.
export auto pull_next(db::connection& conn, const pull_args& args, const task_policy& policy)
    -> std::expected<pull_result, agent_error>;

/// @brief What `peek_next` found.
export struct peek_result {
  bool         no_work = false; ///< True when nothing was eligible.
  std::int64_t task_id = 0;     ///< The task `pull` would take.
  /// Eligible tasks `pull` would pass over because they are not ready, in selector order.
  std::vector<skipped_task> skipped;
};

/// @brief Run `pull_next`'s selector and nothing else.
///
/// No transaction, no writes, no claim. Deliberately shares the selector
/// with `pull_next` rather than reimplementing it, so the two cannot
/// disagree about what "next" means.
/// @param conn An open, migrated connection.
/// @param plan_id The plan to look at.
/// @param policy The injected planning policy (only `unready_reasons` is used), so `peek` skips the
/// tasks `pull` would skip.
/// @return The peek outcome, or `query_failed`.
export auto peek_next(db::connection& conn, std::int64_t plan_id, const task_policy& policy)
    -> std::expected<peek_result, agent_error>;

// =========================================================================
// Engine supervision: heartbeat only (plan 1033 D3, task 6488)
// =========================================================================
//
// The engine actor below is no longer reachable from any verb: no verb hands
// a claim to the engine, and `heartbeat` refuses `--as` and `--attempt` as
// unknown flags, so the CLI only ever builds a caller gate. The terminal
// verbs, `reconcile`, `abort` and the heartbeat gate itself no longer consult
// the supervisor column for the caller, so a claim carrying a legacy
// `supervisor` value is an ordinary claim to its caller everywhere.

/// @brief Who is issuing a supervised heartbeat.
export enum class actor : std::uint8_t {
  caller, ///< The orchestrator or coder: the default.
  engine, ///< The engine supervisor, acting for one Centurion attempt.
};

/// @brief The supervision half of a heartbeat's arguments.
export struct supervisor_gate {
  actor                           as = actor::caller; ///< Who is heartbeating.
  std::optional<std::string_view> attempt;            ///< The attempt, required when `as == engine`.
};

/// @brief Heartbeat a claim with supervision enforced.
///
///   - caller claim, caller actor: exactly the pre-plan-1033 heartbeat.
///   - engine claim, caller actor: an ordinary heartbeat. The caller extends the
///     lease, and reports status, as on any other claim (task 7458).
///   - engine claim, engine actor with the associated attempt: extends the lease.
///     Not reachable from the CLI (see the note above).
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
  claim released; ///< The claim in its new terminal state.
  /// The task it held; UNSET for a plan / plan_step claim (only `release` ends
  /// one), which holds no task. Never fall back to the claim's entity id: it is
  /// a plan or step id there, and a task row may share the number (task 7118).
  std::optional<std::int64_t> task_id;
};

/// @brief End the work session successfully: task -> `done`, claim ->
/// `completed`, open actions -> `ok`.
///
/// The four terminal verbs each run one transaction: the claim's status and
/// its task's status flip together, or neither does. The claim must still be
/// live (`status` active and the lease unexpired) before anything is written.
///
/// Note which guard bites first for the common mistake: a claim taken with
/// `--no-transition` leaves its task in `todo`, and `todo -> done` is not
/// a legal edge, so `complete` on such a claim fails `illegal_transition`
/// with the claim untouched — the transaction rolls back whole.
/// @param conn An open, migrated connection (this function owns the transaction).
/// @param claim_token The claim to end.
/// @param summary The completion summary to record on the actions, or unset.
/// @param policy The injected planning policy.
/// @return The outcome, or `claim_not_found` / `claim_not_active` /
/// `claim_not_on_task` / `illegal_transition` / `task_not_found` /
/// `query_failed`.
export auto complete_work(db::connection& conn, std::string_view claim_token, std::optional<std::string_view> summary,
                          const task_policy& policy) -> std::expected<terminal_result, agent_error>;

/// @brief Fail the work session: task -> `todo`, claim -> `aborted` with a
/// failure category, open actions -> `error`.
/// @param conn An open, migrated connection (this function owns the transaction).
/// @param claim_token The claim to end.
/// @param reason The failure reason, recorded on the claim.
/// @param category The closed failure category.
/// @param policy The injected planning policy.
/// @return The outcome, or the same errors as `complete_work`.
export auto fail_work(db::connection& conn, std::string_view claim_token, std::string_view reason, failure_category category,
                      const task_policy& policy) -> std::expected<terminal_result, agent_error>;

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
/// @return The outcome, or the same errors as `complete_work`.
export auto release_work(db::connection& conn, std::string_view claim_token, std::optional<std::string_view> reason,
                         const task_policy& policy) -> std::expected<terminal_result, agent_error>;

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
/// @return The outcome, or the same errors as `complete_work`.
export auto block_work(db::connection& conn, std::string_view claim_token, std::int64_t blocker_task_id,
                       std::optional<std::string_view> reason, const task_policy& policy)
    -> std::expected<terminal_result, agent_error>;

/// @brief The inputs of `record_review`.
export struct review_args {
  std::string_view                reviewed_claim_token;    ///< The claim under review (any status).
  std::optional<std::string_view> reviewer_claim_token;    ///< The reviewer's own claim, when it has one.
  std::optional<std::string_view> reviewer_vendor_session; ///< The reviewer's vendor session id, when no claim is named.
  std::string_view                vendor;                  ///< Vendor tag for the action when no reviewer claim names one.
  agentactivity::review_verdict   verdict{};               ///< The verdict to record.
  std::string_view                summary;                 ///< The reviewer's summary, stored as the action's summary.
};

/// @brief What `record_review` wrote.
export struct review_result {
  std::int64_t                  action_id{};      ///< The new `review` action.
  std::optional<std::int64_t>   parent_action_id; ///< The reviewed claim's earliest action, unset when it has none.
  agentactivity::review_verdict verdict{};        ///< The recorded verdict.
  agentactivity::claim          reviewed;         ///< The reviewed claim.
};

/// @brief Record a review verdict on a claim, in one transaction (plan 1142).
///
/// Writes exactly one closed `agent_actions` row of kind `review`: `claim_id` is the REVIEWED claim
/// (so `reviews_for_claim` finds it), `parent_action_id` is that claim's earliest non-review
/// action (so `planar-watch tree` hangs it under the reviewed work), `session_id` is the
/// REVIEWER's session, `verdict` and `summary` are the arguments, and `metadata` is
/// `{"reviewer_session":..,"reviewer_claim":..}` (a key is omitted when unknown).
///
/// **Independence.** The caller's session is the reviewer claim's `vendor_session_id` when a
/// reviewer claim is named, else `reviewer_vendor_session`. The review is refused with
/// `reviewer_not_independent` when that equals the reviewed claim's `vendor_session_id`, when the
/// reviewer claim IS the reviewed claim or shares its `sessions` row, and, because a claim without a
/// vendor session cannot be compared, whenever the reviewed claim has none and the reviewer does not
/// name its own claim carrying a vendor session. Naming neither a claim nor a session is
/// `reviewer_unidentified`. Every refusal writes nothing (not even a `sessions` row).
/// @param conn An open, migrated connection (this function owns the transaction).
/// @param args The review.
/// @return The result, or `claim_not_found` (the reviewed claim), `reviewer_claim_not_found`,
/// `reviewer_not_independent`, `reviewer_unidentified`, `busy` or `query_failed`.
export auto record_review(db::connection& conn, const review_args& args) -> std::expected<review_result, agent_error>;

} // namespace planar::engine::runtime::agentatomic
