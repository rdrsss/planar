/// @file agentactivity.cppm
/// @brief `planar.engine.runtime.agentactivity` — the `agent_work_claims`
/// and `agent_actions` store: the single-table primitives underneath the
/// claim ritual (plan 996, task 6038).
///
/// Behavior-preserving port (D2) of
/// zig/src/engine/runtime/agentactivity/{types,store}.zig. The multi-table
/// atomic wrappers live next door in
/// `planar.engine.runtime.agentatomic`, exactly as the Zig original splits
/// `store.zig` from `atomic.zig`, and for the reason that split records:
/// every function HERE assumes the caller already holds the writer lock.
///
/// ## Why this bucket
///
/// `engine/runtime` and not a bucket of its own, because the claim store
/// needs `session::ensure_active` to mint the `sessions` row every claim's
/// `session_id` FK points at, and D15/D18 forbid an `engine_* -> engine_*`
/// edge. Co-locating with `planar.engine.runtime.session` makes that a
/// same-target import instead of an illegal one. The Zig tree draws the
/// same line: `engine/runtime/session.zig` and
/// `engine/runtime/agentactivity/` are siblings.
///
/// ## Time, and why every predicate is a string comparison
///
/// Every timestamp this subsystem writes comes from SQLite, not from the
/// host clock: `strftime('%Y-%m-%dT%H:%M:%fZ','now')`. That is UTC
/// ISO-8601 with milliseconds and a FIXED width, so lexicographic
/// comparison is chronological — which is what makes
///
///     lease_expires_at >= strftime('%Y-%m-%dT%H:%M:%fZ','now')
///
/// a correct liveness test rather than a lucky one. Do not "improve" any
/// of these into host-side time arithmetic: two processes on the same
/// database would then disagree about whether a lease is live.
///
/// A lease that has passed keeps `status='active'` in the row until
/// `reconcile_stale` flips it. So "is this claim honored right now?" is
/// ALWAYS the conjunction `status='active' AND lease_expires_at >= now`,
/// never the status column alone. Every guard in this module spells out
/// both halves; a reader who sees only one has found a bug.
///
/// ## The TTL is formatted into the SQL text, not bound
///
/// SQLite's `strftime` modifier argument is a string literal
/// (`'+600 seconds'`), and a bound parameter there does not participate
/// in the date computation. The Zig original therefore renders the
/// integer into the statement, and so does this port. The value is an
/// `std::int64_t` that has already been through
/// `parse_ttl_seconds`, so there is no injection surface — but the
/// negative case still needs care: `'+-10 seconds'` is a SQLite syntax
/// error that silently yields NULL, so a negative TTL renders WITHOUT the
/// leading `+`. Negative TTLs are unreachable from the CLI (the duration
/// parser cannot produce one); they exist so a test can mint an
/// already-expired claim without sleeping, which is the only way to test
/// the expiry paths deterministically.
///
/// ## Deliberate omissions, named rather than dropped silently
///
/// - ~~**`policy.audit.record`.**~~ NOT AN OMISSION on this surface. The
///   layer-1 `planar.policy` module exists as of task 6100, and the claim
///   verbs were audited against it: the Zig originals write
///   `agent_actions`, not `audit_log`, and carry zero
///   `policy.audit.record` call sites. The paragraph this replaces
///   inherited a gap from a sibling that has one. The real gap in this
///   bucket is session/handoff/snapshot — see CMakeLists.txt.
/// - **`latest_active_claim_for_session`, `next_work`,
///   `record_entity_create_action`.** The remaining read paths, which
///   serve `planar` verbs (`resume`, `status`, entity-create auditing)
///   rather than any leaf landed so far. Deferred with the verbs that
///   consume them. Task 6120 landed the `planar-watch` half —
///   `resolve_claim_scope`, `list_claims`, `list_active_claims_sorted`,
///   `list_stale_claims`, `list_actions*`, `list_claims_by_*`,
///   `latest_action_for_claim`, `collect_plan_activity`,
///   `walk_action_forest` — see § "Read paths" below.
///
/// ## Read paths (task 6120)
///
/// The display half of zig's `store.zig` plus the four per-verb query
/// families the Zig tree kept inline in
/// `zig/src/cmd/planar-watch/handlers/{ps,claims,actions,plans,log,tree}.zig`.
/// They are HERE rather than in those handlers for one concrete reason:
/// `k_claim_columns` and `read_claim_row` are the single place the 27-column
/// projection's ORDER is written down, and the Zig tree paid for its
/// alternative — six handler files each carrying a verbatim copy of that
/// SELECT and its decoder, free to drift. Every read below composes the
/// shared projection with a WHERE/ORDER tail.
///
/// They are also in THIS module rather than a sibling `agentread` for the
/// same reason: the projection constant and the row decoder live in this
/// translation unit's anonymous namespace, and a sibling module could only
/// reuse them by exporting them (widening the public surface for one
/// caller) or by copying them (the drift the constant exists to prevent).
/// - **The git locality probe.** `locality` is an INPUT here: the caller
///   hands over whatever snapshot it managed to take. The probe itself
///   shells `git` three times and lives at layer 3 (see
///   `planar.cmd.planar_agent.locality` for what this port does and does
///   not capture).
module;

export module planar.engine.runtime.agentactivity;

import std;
import planar.db;

namespace planar::engine::runtime::agentactivity {

// =========================================================================
// Enumerations
//
// Each mirrors a CHECK constraint in migrations/00015_agent_activity.up.sql
// (plus 00023 / 00029's ALTERs). `to_text` produces the exact stored token;
// `*_from_text` is the inverse and is TOTAL — an unrecognised token yields
// `nullopt` rather than a default, because silently mapping an unknown
// status onto a known one would make a corrupt row look healthy.
// =========================================================================

/// @brief What a claim can be held on. Mirrors `agent_work_claims.entity_kind`.
export enum class entity_kind : std::uint8_t {
  plan,
  plan_step,
  task,
};

/// @brief Exclusivity mode. Mirrors `agent_work_claims.claim_scope`.
///
/// NOTE, because the name promises more than the code delivers: the
/// contention check does NOT read this column. A `shared` claim blocks a
/// second claim exactly as an `exclusive` one does. That is the Zig
/// original's behavior (`has_active_claim`'s WHERE clause never mentions
/// `claim_scope`) and it is reproduced here rather than "fixed" — D2. The
/// column is written and read back faithfully; it simply has no effect on
/// acquisition.
export enum class claim_scope : std::uint8_t {
  exclusive,
  shared,
};

/// @brief Claim lifecycle status. Mirrors `agent_work_claims.status`.
export enum class claim_status : std::uint8_t {
  active,
  released,
  completed,
  aborted,
  stale,
};

/// @brief Closed failure taxonomy (migration 00029). Mirrors
/// `agent_work_claims.failure_category`.
export enum class failure_category : std::uint8_t {
  usage_limit,
  context_limit,
  output_limit,
  tool_failure,
  validation,
  unknown,
};

/// @brief Action kind. Mirrors `agent_actions.action_kind`.
///
/// `resume_` and `error_` carry trailing underscores only because
/// `resume` reads awkwardly beside C++ keywords in this tree's style and
/// `error` collides; `to_text` emits the unadorned stored token.
export enum class action_kind : std::uint8_t {
  planner,
  ingestor,
  coder,
  test_coder,
  reviewer,
  ext_sync,
  ext_propagate,
  orchestrator,
  resume_,
  workbench_sync,
  spec_draft,
  claim_check,
  heartbeat,
  tool_call,
  user_message,
  assistant_message,
  other,
};

/// @brief Entity kinds an ACTION may reference. A strict superset of
/// `entity_kind` — mirrors `agent_actions.entity_kind`'s wider CHECK set.
export enum class action_entity_kind : std::uint8_t {
  plan,
  plan_step,
  task,
  question,
  test_scenario,
  artifact,
  decision,
};

/// @brief Action outcome. Mirrors `agent_actions.outcome`.
export enum class outcome : std::uint8_t {
  ok,
  error_,
  aborted,
  timeout,
};

/// @brief Worktree cleanliness at capture time. Mirrors
/// `agent_work_claims.dirty_at_claim` / `agent_actions.dirty`.
export enum class dirty_state : std::uint8_t {
  clean,
  dirty,
  unknown,
};

/// @brief The stored token for `value`.
/// @param value The enumerator.
/// @return The exact text the CHECK constraint accepts.
export auto to_text(entity_kind value) -> std::string_view;
/// @brief The stored token for `value`.
/// @param value The enumerator.
/// @return The exact text the CHECK constraint accepts.
export auto to_text(claim_scope value) -> std::string_view;
/// @brief The stored token for `value`.
/// @param value The enumerator.
/// @return The exact text the CHECK constraint accepts.
export auto to_text(claim_status value) -> std::string_view;
/// @brief The stored token for `value`.
/// @param value The enumerator.
/// @return The exact text the CHECK constraint accepts.
export auto to_text(failure_category value) -> std::string_view;
/// @brief The stored token for `value`.
/// @param value The enumerator.
/// @return The exact text the CHECK constraint accepts.
export auto to_text(action_kind value) -> std::string_view;
/// @brief The stored token for `value`.
/// @param value The enumerator.
/// @return The exact text the CHECK constraint accepts.
export auto to_text(action_entity_kind value) -> std::string_view;
/// @brief The stored token for `value`.
/// @param value The enumerator.
/// @return The exact text the CHECK constraint accepts.
export auto to_text(outcome value) -> std::string_view;
/// @brief The stored token for `value`.
/// @param value The enumerator.
/// @return The exact text the CHECK constraint accepts.
export auto to_text(dirty_state value) -> std::string_view;

/// @brief Parse a stored `entity_kind` token.
/// @param text The token.
/// @return The enumerator, or unset when `text` is not a member.
export auto entity_kind_from_text(std::string_view text) -> std::optional<entity_kind>;
/// @brief Parse a stored `claim_scope` token.
/// @param text The token.
/// @return The enumerator, or unset when `text` is not a member.
export auto claim_scope_from_text(std::string_view text) -> std::optional<claim_scope>;
/// @brief Parse a stored `claim_status` token.
/// @param text The token.
/// @return The enumerator, or unset when `text` is not a member.
export auto claim_status_from_text(std::string_view text) -> std::optional<claim_status>;
/// @brief Parse a stored `failure_category` token.
/// @param text The token.
/// @return The enumerator, or unset when `text` is not a member.
export auto failure_category_from_text(std::string_view text) -> std::optional<failure_category>;
/// @brief Parse a stored `action_kind` token.
/// @param text The token.
/// @return The enumerator, or unset when `text` is not a member.
export auto action_kind_from_text(std::string_view text) -> std::optional<action_kind>;
/// @brief Parse a stored `action_entity_kind` token.
/// @param text The token.
/// @return The enumerator, or unset when `text` is not a member.
export auto action_entity_kind_from_text(std::string_view text) -> std::optional<action_entity_kind>;
/// @brief Parse a stored `outcome` token.
/// @param text The token.
/// @return The enumerator, or unset when `text` is not a member.
export auto outcome_from_text(std::string_view text) -> std::optional<outcome>;
/// @brief Parse a stored `dirty_state` token.
/// @param text The token.
/// @return The enumerator, or unset when `text` is not a member.
export auto dirty_state_from_text(std::string_view text) -> std::optional<dirty_state>;

/// @brief Whether an action of `kind` probes git locality by default.
///
/// Mirrors zig's `ActionKind.probeDefault()`: false for `heartbeat` and
/// `tool_call` (a heartbeat every few minutes must not fork `git` three
/// times), true for everything else.
/// @param kind The action kind.
/// @return `true` when the probe runs unless `--no-locality-probe` is given.
export auto probe_default(action_kind kind) -> bool;

// =========================================================================
// Rows
// =========================================================================

/// @brief A git-state snapshot recorded on a claim or action.
///
/// The all-default value is zig's `Locality.skipped`: nothing known,
/// `dirty = unknown`. Handed in by the caller; this module never probes.
export struct locality {
  std::optional<std::string> repo_root;                    ///< Absolute checkout path, when known.
  std::optional<std::string> branch;                       ///< Branch name; unset on detached HEAD.
  std::optional<std::string> head_sha;                     ///< Resolved `HEAD` sha, when known.
  dirty_state                dirty = dirty_state::unknown; ///< Worktree cleanliness.
};

/// @brief An `agent_work_claims` row. Field order matches the canonical
/// 27-column projection every claim SELECT in this module shares.
export struct claim {
  std::int64_t                    id{};              ///< Row id.
  std::string                     claim_token;       ///< 32 lowercase hex chars, minted in SQL.
  std::int64_t                    session_id{};      ///< Owning `sessions` row.
  entity_kind                     kind{};            ///< What is claimed.
  std::int64_t                    entity_id{};       ///< Which one.
  claim_scope                     scope{};           ///< Exclusivity mode (see `claim_scope`).
  claim_status                    status{};          ///< Lifecycle status.
  std::string                     vendor;            ///< Vendor tag.
  std::optional<std::string>      vendor_session_id; ///< The vendor's own session id.
  std::optional<std::string>      role;              ///< Role name.
  std::optional<std::string>      model;             ///< Model identifier, opaque.
  std::optional<std::int64_t>     worktree_id;       ///< Worktree row id, when supplied.
  std::optional<std::string>      worktree_path;     ///< Worktree path, when supplied.
  std::optional<std::string>      repo_root;         ///< Locality: checkout path.
  std::optional<std::string>      branch;            ///< Locality: branch.
  std::optional<std::string>      head_sha_at_claim; ///< Locality: HEAD sha.
  std::optional<dirty_state>      dirty_at_claim;    ///< Locality: cleanliness, or NULL.
  std::optional<std::string>      purpose;           ///< Free-text purpose.
  std::optional<std::string>      base_ref;          ///< Git ref the work is based on.
  std::string                     claimed_at;        ///< Creation timestamp.
  std::string                     last_heartbeat_at; ///< Last heartbeat timestamp.
  std::string                     lease_expires_at;  ///< Absolute lease expiry.
  std::optional<std::string>      released_at;       ///< Terminal timestamp.
  std::optional<std::string>      release_reason;    ///< Terminal reason.
  std::optional<std::int64_t>     run_id;            ///< `workflow_runs` FK.
  std::optional<std::string>      stage;             ///< Workflow stage name.
  std::optional<failure_category> category;          ///< Closed failure category.
};

/// @brief An `agent_actions` row.
export struct action {
  std::int64_t                      id{};             ///< Row id.
  std::int64_t                      session_id{};     ///< Owning session.
  std::optional<std::int64_t>       session_entry_id; ///< Linked timeline entry.
  std::optional<std::int64_t>       parent_action_id; ///< Parent action, for the tree view.
  std::optional<std::int64_t>       claim_id;         ///< Owning claim.
  action_kind                       kind{};           ///< Action kind.
  std::optional<action_entity_kind> entity{};         ///< Referenced entity kind.
  std::optional<std::int64_t>       entity_id;        ///< Referenced entity id.
  std::string                       vendor;           ///< Vendor tag.
  std::optional<std::string>        vendor_role;      ///< Vendor role tag.
  std::optional<std::string>        model;            ///< Model identifier.
  std::string                       started_at;       ///< Start timestamp.
  std::optional<std::string>        ended_at;         ///< End timestamp; unset while open.
  std::optional<outcome>            result;           ///< Outcome; unset while open.
  std::optional<std::string>        summary;          ///< Free-text summary.
  std::optional<std::string>        head_sha;         ///< Locality: HEAD sha.
  std::optional<dirty_state>        dirty;            ///< Locality: cleanliness.
  std::optional<std::string>        metadata;         ///< Opaque caller text; never parsed here.
};

/// @brief The subset of a `tasks` row the agent verbs render.
///
/// Read straight from the table rather than through
/// `planar.engine.planning.task`, because that bucket is a SIBLING
/// layer-2 module and D15/D18 forbid the edge. This is the same call
/// `engine/workbench` documented for its own entity reads, and the Zig
/// original's `writeTask` emits exactly these fourteen columns in exactly
/// this order.
export struct task_row {
  std::int64_t                id{};           ///< Row id.
  std::string                 scope_kind;     ///< `global` / `association` / `repo`.
  std::optional<std::int64_t> scope_id;       ///< Scope row id.
  std::optional<std::int64_t> plan_id;        ///< Owning plan.
  std::optional<std::int64_t> parent_task_id; ///< Parent task.
  std::string                 title;          ///< Task title.
  std::optional<std::string>  body;           ///< Task body.
  std::optional<std::string>  slug;           ///< Task slug.
  std::string                 status;         ///< Task status, verbatim.
  std::int64_t                priority{};     ///< Priority; LOWER sorts first.
  std::optional<std::string>  next_action;    ///< Next-action text.
  std::optional<std::string>  due_at;         ///< Due timestamp.
  std::string                 created_at;     ///< Creation timestamp.
  std::string                 updated_at;     ///< Last-update timestamp.
};

// =========================================================================
// Errors
// =========================================================================

/// @brief The failure surface of the claim store and its atomic wrappers.
///
/// One enum for both modules rather than two, because the CLI collapses
/// them anyway: every `planar-agent` handler reports
/// `error: <verb>: <ErrorName>` using the Zig error TAG, so the tag is
/// operator-visible output and `error_name` below is the contract. Two
/// enums would mean two tag tables that could drift.
///
/// zig's `ClaimSessionMismatch` and `NoEligibleTask` are declared in the
/// original's error sets but never produced by any code path in it. They
/// are omitted here rather than carried as unreachable enumerators — a
/// member no function can return is a member no test can pin.
export enum class agent_error : std::uint8_t {
  claim_contention,   ///< A live exclusive claim already holds the entity.
  claim_not_found,    ///< No row for that claim token (or action id).
  claim_not_active,   ///< The claim exists but is terminal or its lease has passed.
  worktree_not_found, ///< `--worktree <id>` named a row the `worktrees` table lacks.
  task_not_found,     ///< A `tasks` row the operation needs is missing.
  claim_not_on_task,  ///< A task-only verb was handed a plan / plan_step claim.
  illegal_transition, ///< The task status transition the verb needs is not legal.
  unknown_status,     ///< The task's current status is not a known member.
  query_failed,       ///< Backstop for an underlying SQL failure.
};

/// @brief The Zig error TAG for `err`, as it appears on the operator's
/// stderr.
///
/// These CamelCase strings are output bytes, not diagnostics: the oracle
/// prints `error: complete: ClaimNotActive` and a parity test compares the
/// whole line. Changing one changes observable behavior.
/// @param err The failure.
/// @return The tag.
export auto error_name(agent_error err) -> std::string_view;

// =========================================================================
// Acquire
// =========================================================================

/// @brief Arguments to `acquire_claim`. Mirrors zig's `AcquireArgs`.
export struct acquire_args {
  std::int64_t                    session_id{};                   ///< Owning session.
  entity_kind                     kind{};                         ///< What to claim.
  std::int64_t                    entity_id{};                    ///< Which one.
  claim_scope                     scope = claim_scope::exclusive; ///< Exclusivity mode.
  std::string_view                vendor;                         ///< Vendor tag; never empty.
  std::optional<std::string_view> vendor_session_id;              ///< The vendor's own session id.
  std::optional<std::string_view> role;                           ///< Role name.
  std::optional<std::string_view> model;                          ///< Model identifier.
  std::optional<std::int64_t>     worktree_id;                    ///< Worktree row id.
  std::optional<std::string_view> worktree_path;                  ///< Worktree path.
  std::optional<std::string_view> purpose;                        ///< Free-text purpose.
  std::optional<std::string_view> base_ref;                       ///< Git ref.
  /// Lease TTL in seconds. Rendered into the SQL text, not bound — see
  /// this file's header. Negative values are legal at this API (they mint
  /// an already-expired claim for tests) and unreachable from the CLI.
  std::int64_t ttl_secs = 600;
  locality     loc; ///< Git snapshot to record.
  /// Take over a live claim: mark every active claim on the same
  /// `(kind, entity_id)` stale first. Operator recovery only; the normal
  /// path returns `claim_contention` instead.
  bool                            force = false;
  std::optional<std::int64_t>     run_id; ///< `workflow_runs` FK.
  std::optional<std::string_view> stage;  ///< Workflow stage name.
};

/// @brief Insert a new claim.
///
/// MUST be called with the writer lock already held (`BEGIN IMMEDIATE`) —
/// the exclusivity check is a SELECT followed by an INSERT, and that pair
/// is only safe against a concurrent claimer if no other writer can
/// interleave between them. `planar.engine.runtime.agentatomic`'s wrappers
/// are what hold it; calling this directly outside one is a race, not a
/// shortcut.
///
/// The token is minted by SQLite (`lower(hex(randomblob(16)))`), never
/// host-side. It is opaque: never parse meaning out of it.
/// @param conn An open, migrated connection, inside an immediate transaction.
/// @param args The claim to insert.
/// @return The inserted row, or `claim_contention` / `worktree_not_found` /
/// `query_failed`.
export auto acquire_claim(db::connection& conn, const acquire_args& args) -> std::expected<claim, agent_error>;

// =========================================================================
// Lease and terminal primitives
// =========================================================================

/// @brief Refresh the lease on a live claim.
///
/// `ttl_secs` semantics:
///   - engaged — set the lease to `now + *ttl_secs`, ABSOLUTELY. A
///     deliberate re-TTL; may lengthen OR shorten the lease.
///   - `std::nullopt` — RENEW THE CLAIM'S CURRENT LEASE LENGTH. The new
///     expiry is `now + (lease_expires_at - last_heartbeat_at)`, so the
///     claim keeps the lease length it already holds, measured from now.
///
/// The renew case needs no stored-TTL column: the pair
/// (`last_heartbeat_at`, `lease_expires_at`) already encodes the current
/// TTL exactly, and that invariant is maintained by construction at every
/// write site — `acquire_claim` inserts `lease_expires_at = now + ttl`
/// while `last_heartbeat_at` takes its `now` schema default in the same
/// statement, and this function rewrites both columns in one UPDATE.
/// SQLite evaluates an UPDATE's RHS expressions against the OLD row, so
/// reading the delta while overwriting both columns is safe.
///
/// HISTORY: this used to reset the lease to `now + ttl_secs` with the CLI
/// defaulting an omitted `--ttl` to 600, so a claim taken with `--ttl 8h`
/// and then heartbeated bare had its lease CUT from eight hours to ten
/// minutes — a faithfully-heartbeating long dispatch was MORE likely to
/// lose its claim than one that never heartbeated. That was Planar task
/// 6093, and this port pinned it as observed oracle behavior. The ORACLE
/// HAS NOW BEEN FIXED (zig/src/engine/runtime/agentactivity/store.zig),
/// and this implementation and its tests were re-pinned to match. The
/// weaker "extend, never shrink" rule was rejected: it is inert on exactly
/// the claims that need it, since for any lease longer than the default
/// every bare heartbeat becomes a no-op and the lease still lapses at its
/// original expiry.
///
/// Refuses a claim that is terminal or whose lease has already passed
/// (`claim_not_active`) — heartbeating a dead lease does not resurrect it.
/// @param conn An open, migrated connection, inside an immediate transaction.
/// @param claim_token The token to refresh.
/// @param ttl_secs The NEW lease length in seconds measured from now, or
/// `std::nullopt` to renew the lease length the claim currently holds.
/// @return The refreshed row, or `claim_not_active` / `claim_not_found` /
/// `query_failed`.
export auto heartbeat_claim(db::connection& conn, std::string_view claim_token, std::optional<std::int64_t> ttl_secs)
    -> std::expected<claim, agent_error>;

/// @brief Move a live claim to a terminal status.
///
/// Guarded on BOTH `status='active'` and an unexpired lease, so it cannot
/// terminalise a claim reconcile has already swept. `new_status` must not
/// be `active` (release IS the transition away from active);
/// `query_failed` if it is.
///
/// `failure_category` is written unconditionally, so passing unset CLEARS
/// any previously recorded category. That is the Zig original's behavior.
/// @param conn An open, migrated connection, inside an immediate transaction.
/// @param claim_token The token to release.
/// @param new_status The terminal status.
/// @param reason The reason to record, or unset.
/// @param category The failure category to record, or unset (clears).
/// @return The released row, or `claim_not_active` / `claim_not_found` /
/// `query_failed`.
export auto release_claim(db::connection& conn, std::string_view claim_token, claim_status new_status,
                          std::optional<std::string_view> reason, std::optional<failure_category> category)
    -> std::expected<claim, agent_error>;

/// @brief Operator force-release: mark a claim `aborted` regardless of who
/// owns it or what state it is in.
///
/// Deliberately UNGUARDED where `release_claim` is guarded — no session
/// ownership check, no status check, no lease check. That is the whole
/// point of the verb: it is the recovery path for a claim whose owner is
/// gone, and a guard would make it useless in exactly the situation it
/// exists for. Only a token that matches no row fails.
/// @param conn An open, migrated connection, inside an immediate transaction.
/// @param claim_token The token to force-release.
/// @param reason The reason to record, or unset.
/// @param category The failure category to record, or unset.
/// @return The aborted row, or `claim_not_found` / `query_failed`.
export auto abort_claim(db::connection& conn, std::string_view claim_token, std::optional<std::string_view> reason,
                        std::optional<failure_category> category) -> std::expected<claim, agent_error>;

/// @brief After an abort, put a task back to `todo` if — and only if —
/// THIS claim is what moved it to `doing`.
///
/// The evidence is the `claim_check` action row that a transitioning
/// `claim --entity` writes: a `--no-transition` claim has none and
/// therefore cannot reset a status it never set, and a `pull` claim has a
/// role action rather than a `claim_check` one, so its task keeps the
/// longstanding pull abort semantics. The reset also refuses while any
/// OTHER live claim holds the task, so a takeover's new owner is not
/// yanked out from under.
/// @param conn An open, migrated connection, inside an immediate transaction.
/// @param claim_id The aborted claim's row id.
/// @param task_id The task it held.
/// @return Success, or `query_failed`.
export auto reset_direct_claim_task_after_abort(db::connection& conn, std::int64_t claim_id, std::int64_t task_id)
    -> std::expected<void, agent_error>;

/// @brief Is this claim honored right now — `status='active'` AND lease
/// unexpired?
/// @param conn An open, migrated connection.
/// @param claim_token The token to test.
/// @return `true` when live, or `query_failed`.
export auto is_claim_active_unexpired(db::connection& conn, std::string_view claim_token) -> std::expected<bool, agent_error>;

/// @brief Does ANY live claim currently hold this task (task 6141)?
///
/// Port of zig/src/engine/planning/task.zig's private
/// `hasActiveClaimOnTask`, which gates every operator status flip
/// (`task done` / `cancel` / `block` / `reopen`, and `task update` when the
/// patch carries a `--status`) behind `error.TaskClaimed`.
///
/// It lives HERE, in the bucket that owns `agent_work_claims`, and not in
/// `engine_planning` where the oracle keeps it, for one reason:
/// `engine_planning` and `engine_runtime` are both layer 2, and
/// `cmake/architecture.cmake` FATALs on a same-layer edge (D15/D18). The
/// guard is therefore composed at layer 3 by the `task` handlers — the
/// same D20 shape `annotate add` and `unlink` already use.
///
/// ONE behavioural narrowing follows from that move, and it is stated
/// rather than buried: the oracle runs this check INSIDE the verb's own
/// write transaction, so it is TOCTOU-safe against a claim acquired
/// mid-verb. Composed at layer 3 it runs just BEFORE the engine call, so a
/// claim taken in that window is missed. Single-process behaviour — every
/// test, and every interactive operator invocation — is identical; only a
/// genuine race differs. That is strictly better than the alternative this
/// replaces, which was no guard at all.
///
/// `claim_scope` is deliberately not consulted: a `shared` claim blocks
/// exactly as an `exclusive` one does, mirroring the Zig original's WHERE
/// clause. See `claim_scope`'s own note.
/// @param conn An open, migrated connection.
/// @param task_id The task's row id.
/// @return `true` when a claim with `status='active'` and an unexpired
/// lease exists on this task, or `query_failed`.
export auto has_active_claim_on_task(db::connection& conn, std::int64_t task_id) -> std::expected<bool, agent_error>;

// =========================================================================
// Reads
// =========================================================================

/// @brief Fetch a claim by row id.
/// @param conn An open, migrated connection.
/// @param id The row id.
/// @return The row, or `claim_not_found` / `query_failed`.
export auto get_claim_by_id(db::connection& conn, std::int64_t id) -> std::expected<claim, agent_error>;

/// @brief Fetch a claim by token.
/// @param conn An open, migrated connection.
/// @param token The claim token.
/// @return The row, or `claim_not_found` / `query_failed`.
export auto get_claim_by_token(db::connection& conn, std::string_view token) -> std::expected<claim, agent_error>;

/// @brief Fetch the fourteen `tasks` columns the agent verbs render.
/// @param conn An open, migrated connection.
/// @param id The task row id.
/// @return The row, or `task_not_found` / `query_failed`.
export auto get_task(db::connection& conn, std::int64_t id) -> std::expected<task_row, agent_error>;

/// @brief Read a task's current `status` text.
/// @param conn An open, migrated connection.
/// @param id The task row id.
/// @return The status text, or `task_not_found` / `query_failed`.
export auto current_task_status(db::connection& conn, std::int64_t id) -> std::expected<std::string, agent_error>;

/// @brief Read a task's `plan_id`, treating a missing row or a NULL
/// column alike as "no plan". Best-effort by design — the callers use it
/// only to decide whether a plan roll-up recompute is warranted.
/// @param conn An open, migrated connection.
/// @param task_id The task row id.
/// @return The plan id, or unset.
export auto task_plan_id(db::connection& conn, std::int64_t task_id) -> std::optional<std::int64_t>;

// =========================================================================
// Run association
// =========================================================================

/// @brief Stamp a `workflow_runs` id (and optional stage) onto a live claim.
///
/// Returns the number of rows updated rather than an error when the token
/// is unknown or terminal — `0` is a legitimate answer the CLI surfaces as
/// `updated:0` with exit 0. Not wrapped in a transaction (a single UPDATE).
/// @param conn An open, migrated connection.
/// @param claim_token The token to stamp.
/// @param run_id The `workflow_runs` row id.
/// @param stage The stage name, or unset for NULL.
/// @return The number of rows updated, or `query_failed`.
export auto associate_claim_run(db::connection& conn, std::string_view claim_token, std::int64_t run_id,
                                std::optional<std::string_view> stage) -> std::expected<std::int64_t, agent_error>;

// =========================================================================
// Actions
// =========================================================================

/// @brief Arguments to `start_action`. Mirrors zig's `StartActionArgs`.
export struct start_action_args {
  std::int64_t                      session_id{};     ///< Owning session.
  std::optional<std::int64_t>       session_entry_id; ///< Linked timeline entry.
  std::optional<std::int64_t>       parent_action_id; ///< Parent action.
  std::optional<std::int64_t>       claim_id;         ///< Owning claim.
  action_kind                       kind{};           ///< Action kind.
  std::optional<action_entity_kind> entity;           ///< Referenced entity kind.
  std::optional<std::int64_t>       entity_id;        ///< Referenced entity id.
  std::string_view                  vendor;           ///< Vendor tag.
  std::optional<std::string_view>   vendor_role;      ///< Vendor role tag.
  std::optional<std::string_view>   model;            ///< Model identifier.
  locality                          loc;              ///< Git snapshot.
  /// Opaque caller text persisted to `agent_actions.metadata`. Typically
  /// JSON, but this module never parses it — validation is the CLI's job.
  std::optional<std::string_view> metadata;
};

/// @brief Open an action row (`started_at` defaulted, `ended_at` NULL).
///
/// `entity` and `entity_id` must be both set or both unset — the table's
/// own CHECK says so, and this refuses with `query_failed` before SQLite
/// has to.
/// @param conn An open, migrated connection.
/// @param args The action to open.
/// @return The new row id, or `query_failed`.
export auto start_action(db::connection& conn, const start_action_args& args) -> std::expected<std::int64_t, agent_error>;

/// @brief Close an open action.
///
/// A no-op (zero rows) when the action is already closed or absent — NOT
/// an error, matching the Zig original's SQL despite its doc comment's
/// claim to the contrary. `summary` uses `coalesce(?, summary)`, so
/// passing unset PRESERVES an existing summary rather than clearing it.
/// @param conn An open, migrated connection.
/// @param id The action row id.
/// @param result The outcome to record.
/// @param summary The summary to record, or unset to preserve.
/// @return Success, or `query_failed`.
export auto end_action(db::connection& conn, std::int64_t id, outcome result, std::optional<std::string_view> summary)
    -> std::expected<void, agent_error>;

/// @brief Fetch an action by row id.
/// @param conn An open, migrated connection.
/// @param id The row id.
/// @return The row, or `claim_not_found` (the Zig original reuses that tag
/// for a missing action) / `query_failed`.
export auto get_action_by_id(db::connection& conn, std::int64_t id) -> std::expected<action, agent_error>;

/// @brief Close every still-open action attached to a claim.
/// @param conn An open, migrated connection, inside an immediate transaction.
/// @param claim_id The owning claim.
/// @param result The outcome to record on each.
/// @param summary The summary to record, or unset to preserve each action's own.
/// @return Success, or `query_failed`.
export auto close_open_actions_for_claim(db::connection& conn, std::int64_t claim_id, outcome result,
                                         std::optional<std::string_view> summary) -> std::expected<void, agent_error>;

/// @brief The newest still-open action on a claim — the parent
/// `action start` attaches a nested action under.
/// @param conn An open, migrated connection.
/// @param claim_id The owning claim.
/// @return The action id, or unset when the claim has no open action.
export auto latest_open_action_for_claim(db::connection& conn, std::int64_t claim_id) -> std::optional<std::int64_t>;

// =========================================================================
// Reconcile
// =========================================================================

/// @brief Sweep policy. Mirrors zig's `ReconcilePolicy`.
export struct reconcile_policy {
  /// Extra grace beyond lease expiry. `0` (the CLI default) still renders
  /// a `'-0 seconds'` modifier, which is a no-op offset.
  std::int64_t                    stale_after_secs = 0;
  bool                            dry_run          = false; ///< Report candidates, write nothing at all.
  std::optional<std::int64_t>     session_id;               ///< Scope to one session; unset sweeps globally.
  std::optional<std::int64_t>     plan_id;                  ///< Scope to one plan; unset sweeps globally.
  std::optional<failure_category> category;                 ///< Category to stamp on claims made stale.
};

/// @brief What a sweep did (or, in dry-run, would do).
export struct reconcile_result {
  /// Number of claims moved to `stale`. Taken from the CANDIDATE COUNT,
  /// not from `changes()` — so a claim that raced to a terminal status
  /// between the SELECT and the UPDATE still counts. Zig's behavior,
  /// preserved.
  std::int64_t       claims_marked_stale = 0;
  std::int64_t       actions_closed      = 0; ///< Number of action rows closed.
  std::vector<claim> candidates;              ///< The expired claims found (populated in every mode).
};

/// @brief Mark expired claims stale, return their tasks to `todo` where
/// the claim owns that transition, and close actions orphaned by ended
/// sessions.
///
/// Caller wraps in `BEGIN IMMEDIATE` when `!dry_run`. In `dry_run` the
/// function writes NOTHING and returns after collecting candidates — both
/// counters stay zero even though the candidate list is non-empty, which
/// is exactly what the oracle reports.
/// @param conn An open, migrated connection.
/// @param policy The sweep policy.
/// @return The sweep result, or `query_failed`.
export auto reconcile_stale(db::connection& conn, const reconcile_policy& policy) -> std::expected<reconcile_result, agent_error>;

/// @brief A `workflow_runs` row the sweep considers dead.
export struct run_candidate {
  std::int64_t                id{};           ///< Row id.
  std::string                 run_identifier; ///< The caller-supplied identifier.
  std::int64_t                pid{};          ///< The recorded process id.
  std::optional<std::int64_t> plan_id;        ///< The owning plan, when set.
};

/// @brief What the run sweep did.
export struct reconcile_runs_result {
  std::int64_t               abandoned = 0; ///< Rows moved to `abandoned`.
  std::vector<run_candidate> candidates;    ///< The dead-pid rows found.
};

/// @brief Is a process id live?
///
/// POSIX `kill(pid, 0)`: `ESRCH` means gone, `EPERM` means it exists under
/// another owner (so: alive), any other errno is treated as alive because
/// the conservative direction here is to NOT abandon a run we are unsure
/// about. A non-positive pid is dead by definition.
/// @param pid The process id.
/// @return `true` when the process appears to exist.
export auto pid_alive(std::int64_t pid) -> bool;

/// @brief Abandon `running` workflow runs whose recorded pid is gone.
/// @param conn An open, migrated connection.
/// @param dry_run When true, collect candidates and write nothing.
/// @param plan_id Scope to one plan; unset sweeps globally.
/// @return The sweep result, or `query_failed`.
export auto reconcile_runs(db::connection& conn, bool dry_run, std::optional<std::int64_t> plan_id)
    -> std::expected<reconcile_runs_result, agent_error>;

// =========================================================================
// Read paths — the display half (task 6120)
//
// Every function below is a pure SELECT. None opens a transaction, none
// writes, and none is reachable from a `planar-agent` verb; they exist for
// `planar-watch`'s six read verbs. That property is what lets the READ-ONLY
// binary link this module at all.
// =========================================================================

/// @brief Where the entity under a claim is STORED — the answer to "which
/// project/association does this claim's task belong to", which the claim
/// row itself does not carry.
///
/// Port of zig `store.resolveClaimScope`. Degrades rather than fails: any
/// query failure, missing row, or unrecognised `scope_kind` yields
/// `kind = "?"` with no slug, because a viewer that refused to render a
/// claim because its entity had been deleted would be less useful than one
/// that renders `scope:?`.
export struct claim_scope_info {
  /// @brief `global`, `association`, `repo`, or `?` when unresolvable.
  std::string kind = "?";
  /// @brief The association/project slug; unset for `global` and `?`, and
  /// for a scope row that no longer exists.
  std::optional<std::string> slug;

  /// @brief The single-token form the text columns print after `scope:`.
  /// @return The slug when present, otherwise `kind`.
  [[nodiscard]] auto label() const -> std::string_view {
    return slug.has_value() ? std::string_view{*slug} : std::string_view{kind};
  }
};

/// @brief Resolve where `value`'s entity is stored.
/// @param conn An open connection (read-only is sufficient).
/// @param value The claim whose entity to resolve.
/// @return The resolved scope, or the degraded `?` form.
export auto resolve_claim_scope(db::connection& conn, const claim& value) -> claim_scope_info;

/// @brief `planar-watch claims --status`'s three arms.
export enum class claim_status_filter : std::uint8_t {
  /// @brief `status='active'` AND an unexpired lease.
  active,
  /// @brief `status='stale'` OR an expired-lease `active` row.
  stale,
  /// @brief Every row in the ledger.
  all,
};

/// @brief Parse a `--status` value.
///
/// TOTAL, and deliberately so: zig's `parseStatus` maps an unrecognised
/// value onto `.active` rather than raising, so `--status nonsense` lists
/// active claims on the reference binary. Reproduced (D2), not "fixed".
/// @param text The flag value, or unset when the flag was absent.
/// @return The filter; `active` for absent AND for unrecognised.
export auto parse_claim_status_filter(std::optional<std::string_view> text) -> claim_status_filter;

/// @brief List ledger claims under `filter`, newest-claimed first.
/// @param conn An open connection.
/// @param filter Which arm to list.
/// @return The rows, or `query_failed`.
export auto list_claims(db::connection& conn, claim_status_filter filter) -> std::expected<std::vector<claim>, agent_error>;

/// @brief `planar-watch ps --sort-by`'s two arms.
export enum class ps_sort : std::uint8_t {
  /// @brief `last_heartbeat_at desc`, NULLs last. The default.
  heartbeat,
  /// @brief `claimed_at desc` — the pre-M3 ordering.
  lease,
};

/// @brief Parse a `--sort-by` value.
///
/// NOT total, unlike `parse_claim_status_filter`: zig's `parseSortBy`
/// raises `InvalidValue` on an unrecognised value and `ps` dies with a
/// specific message. The two flags really do differ in strictness on the
/// reference binary.
/// @param text The flag value, or unset when the flag was absent.
/// @return The sort order, or unset when the value is unrecognised.
export auto parse_ps_sort(std::optional<std::string_view> text) -> std::optional<ps_sort>;

/// @brief List every `status='active'` claim for `ps`.
///
/// NOTE THE MISSING LEASE PREDICATE, which is not an oversight here: zig's
/// `listActiveSorted` filters on the STATUS COLUMN ALONE, where
/// `list_claims(active)` also requires an unexpired lease. The observable
/// consequence is that under `ps --stale` an expired-but-not-yet-reconciled
/// claim appears in BOTH the `active` and `stale` buckets. Reproduced (D2);
/// `agentactivity.t.cpp` pins it so a later "cleanup" cannot quietly change
/// what `ps` shows.
/// @param conn An open connection.
/// @param sort The ordering.
/// @return The rows, or `query_failed`.
export auto list_active_claims_sorted(db::connection& conn, ps_sort sort) -> std::expected<std::vector<claim>, agent_error>;

/// @brief List `status='stale'` claims plus expired-lease `active` ones.
/// @param conn An open connection.
/// @return The rows, newest-claimed first, or `query_failed`.
export auto list_stale_claims(db::connection& conn) -> std::expected<std::vector<claim>, agent_error>;

/// @brief List `agent_actions`, newest-started first, capped at `limit`.
/// @param conn An open connection.
/// @param limit The row cap.
/// @return The rows, or `query_failed`.
export auto list_actions(db::connection& conn, std::int64_t limit) -> std::expected<std::vector<action>, agent_error>;

/// @brief The newest action attached to a claim — what `ps` and `tree`
/// render in their `activity:` column.
///
/// Distinct from `latest_open_action_for_claim`, which requires
/// `ended_at IS NULL`; this one takes the newest action whether it is still
/// running or not.
/// @param conn An open connection.
/// @param claim_id The claim's row id.
/// @return The action, unset when the claim has none, or `query_failed`.
export auto latest_action_for_claim(db::connection& conn, std::int64_t claim_id)
    -> std::expected<std::optional<action>, agent_error>;

/// @brief `planar-watch log`'s action source, filtered to one entity.
/// @param conn An open connection.
/// @param entity_kind The stored `entity_kind` token, verbatim — NOT parsed
/// into the enum, because `log --entity nonsense:1` must return no rows
/// rather than raise, exactly as the reference binary does.
/// @param entity_id The entity's row id.
/// @param limit The row cap.
/// @return The rows OLDEST-first (a timeline, not a feed), or `query_failed`.
export auto list_actions_by_entity(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id, std::int64_t limit)
    -> std::expected<std::vector<action>, agent_error>;

/// @brief `planar-watch log --session`'s action source.
/// @param conn An open connection.
/// @param session_id The session's row id.
/// @param limit The row cap.
/// @return The rows oldest-first, or `query_failed`.
export auto list_actions_by_session(db::connection& conn, std::int64_t session_id, std::int64_t limit)
    -> std::expected<std::vector<action>, agent_error>;

/// @brief `planar-watch log --claim`'s action source.
/// @param conn An open connection.
/// @param token The claim token.
/// @param limit The row cap.
/// @return The rows oldest-first, or `query_failed`.
export auto list_actions_by_claim_token(db::connection& conn, std::string_view token, std::int64_t limit)
    -> std::expected<std::vector<action>, agent_error>;

/// @brief `planar-watch log`'s claim source, filtered to one entity.
/// @param conn An open connection.
/// @param entity_kind The stored token, verbatim (see `list_actions_by_entity`).
/// @param entity_id The entity's row id.
/// @return The rows oldest-claimed first, or `query_failed`.
export auto list_claims_by_entity(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id)
    -> std::expected<std::vector<claim>, agent_error>;

/// @brief `planar-watch log --session`'s claim source.
/// @param conn An open connection.
/// @param session_id The session's row id.
/// @return The rows oldest-claimed first, or `query_failed`.
export auto list_claims_by_session(db::connection& conn, std::int64_t session_id)
    -> std::expected<std::vector<claim>, agent_error>;

/// @brief `planar-watch log --claim`'s claim source.
/// @param conn An open connection.
/// @param token The claim token.
/// @return The matching row as a (0-or-1 element) list, or `query_failed`.
export auto list_claims_by_token(db::connection& conn, std::string_view token) -> std::expected<std::vector<claim>, agent_error>;

/// @brief Does a CLAIM on `(kind, id)` roll up to `plan_id`?
///
/// Port of zig `planfilter.claimBelongsToPlan`: `--plan N` is WIDENED to
/// match a plan-direct claim, a claim on a task whose `plan_id` is N, and a
/// claim on a `plan_step` under N. Matching only the first was the pre-fix
/// behavior and silently dropped every task claim, which is the case that
/// actually occurs.
/// @param conn An open connection.
/// @param kind The claim's entity kind.
/// @param entity_id The claim's entity id.
/// @param plan_id The plan to test against.
/// @return `true` when the claim rolls up to the plan.
export auto claim_belongs_to_plan(db::connection& conn, entity_kind kind, std::int64_t entity_id, std::int64_t plan_id) -> bool;

/// @brief Does an ACTION on `(kind, id)` roll up to `plan_id`?
///
/// The action's entity kind is a strict superset of the claim's. The four
/// extra kinds (`question`, `test_scenario`, `artifact`, `decision`) have no
/// direct plan link in the schema and therefore NEVER match `--plan`; zig's
/// `actionBelongsToPlan` says so explicitly and this reproduces it.
/// @param conn An open connection.
/// @param kind The action's entity kind.
/// @param entity_id The action's entity id.
/// @param plan_id The plan to test against.
/// @return `true` when the action rolls up to the plan.
export auto action_belongs_to_plan(db::connection& conn, action_entity_kind kind, std::int64_t entity_id, std::int64_t plan_id)
    -> bool;

/// @brief One plan's in-flight summary, as `planar-watch plans` renders it.
export struct plan_activity {
  /// @brief Claims with `status='active'` and an unexpired lease on the
  /// plan or on a task under it.
  std::int64_t active_claims = 0;
  /// @brief `agent_actions` rows with `ended_at IS NULL` on the plan or on
  /// a task under it.
  std::int64_t active_actions = 0;
  /// @brief The newest watermark across the plan's claim and action rows;
  /// unset when the plan has no recorded agent activity at all.
  std::optional<std::string> last_event_at;
};

/// @brief Summarize one plan's agent activity.
///
/// NOTE the asymmetry with `claim_belongs_to_plan` above, which is zig's
/// and is preserved: these three aggregates match `plan` and `task` rows
/// only — a `plan_step` claim rolls up under `--plan` on `ps`/`claims` but
/// is NOT counted here. Reproduced (D2) rather than harmonised.
/// @param conn An open connection.
/// @param plan_id The plan's row id.
/// @return The summary, or `query_failed`.
export auto collect_plan_activity(db::connection& conn, std::int64_t plan_id) -> std::expected<plan_activity, agent_error>;

/// @brief Does a `sessions` row with this id exist?
/// @param conn An open connection.
/// @param session_id The row id to probe.
/// @return `true` when present, or `query_failed`.
export auto session_exists(db::connection& conn, std::int64_t session_id) -> std::expected<bool, agent_error>;

/// @brief One node of the `agent_actions.parent_action_id` forest.
export struct forest_node {
  std::int64_t                id{};             ///< The action's row id.
  std::optional<std::int64_t> parent_action_id; ///< Parent, unset at a root.
  std::int64_t                session_id{};     ///< The action's session.
  std::optional<std::int64_t> claim_id;         ///< Linked claim, when any.
  std::int64_t                depth{};          ///< 0 at a root.
  /// @brief Whether this node has no later sibling — the `└──` vs `├──`
  /// decision. Computed in a second pass over the flattened walk.
  bool is_last_sibling = false;
};

/// @brief Walk the orchestrator -> sub-agent action forest.
///
/// Roots are `parent_action_id IS NULL`; children extend through a
/// `WITH RECURSIVE` term. Rows come back ordered by `id asc`, which is the
/// order `tree` renders and the order `is_last_sibling` is computed against.
/// @param conn An open connection.
/// @param root_session_id Scope to one session's roots; unset walks all.
/// @return The flattened forest, or `query_failed`.
export auto walk_action_forest(db::connection& conn, std::optional<std::int64_t> root_session_id)
    -> std::expected<std::vector<forest_node>, agent_error>;

} // namespace planar::engine::runtime::agentactivity
