/// @file handoff.cppm
/// @brief `planar.engine.runtime.handoff` — the `handoffs` entity, its
/// status transitions, and the payloads the `planar handoff *` leaves
/// write (plan 996, task 6040).
///
/// Port target: zig/src/engine/runtime/handoff.zig plus the rendering half
/// of zig/src/cmd/planar/handlers/handoff/render.zig.
///
/// A handoff is operator vendor-session state: per plan 144 M4 the table
/// carries NO scope columns, so its mutations skip the cross-scope guard.
/// The Zig original emits a `policy.audit` row per mutation (four call
/// sites: create/validate/consume/abandon) and this tree still does not.
/// The layer-1 `planar.policy` module DOES now exist — task 6100 landed
/// it and wired `engine_planning`/`engine_identity` — so this is a real
/// remaining gap rather than a missing dependency. It is one of three
/// left in the tree; see engine/runtime/CMakeLists.txt for why this
/// bucket wants its own cycle.
///
/// ## The transition check is INJECTED, not imported
///
/// `validate`, `consume` and `abandon` each guard their UPDATE with the
/// handoff status matrix. That matrix lives in
/// `planar.engine.planning.transitions` — a sibling layer-2 bucket that
/// cmake/architecture.cmake FATALs an edge to. So these functions take a
/// `transition_check` callable and `cmd_planar` (layer 3, allowed to
/// depend on both) supplies the real one.
///
/// The alternative — a private copy of the four-status matrix in this
/// bucket — is what `agentatomic.cppm` already argued against for the task
/// matrix, and the argument is the same: two copies of a status matrix in
/// one tree are two copies free to drift. The side benefit is the same
/// too: a test can pass a RECORDING probe and observe directly that the
/// guard runs BEFORE the UPDATE rather than after it.
///
/// ## `status` is an enum, and the wire text is the enum's text
///
/// The column is CHECK-constrained to exactly four values, so a row whose
/// status text parses to none of them is `handoff_error::query_failed` —
/// the Zig original's `Status.fromText(...) orelse return Error.QueryFailed`.
/// That is a deliberately blunt mapping of "impossible row" and is
/// preserved rather than improved.
///
/// ## Worktree columns
///
/// `worktree_path` / `repo_root` / `branch` are copied from the active
/// `agent_work_claims` row at create time (plan 297 followup t#2947) so a
/// resumer can recover the worktree even after the claim is released.
/// Resolving them is the CALLER's job — see the `handoff` composite
/// handler — because the claim store's read path and this module are
/// reachable from layer 3 together but not from each other.
module;

export module planar.engine.runtime.handoff;

import std;
import planar.db;

namespace planar::engine::runtime::handoff {

/// @brief The four CHECK-constrained `handoffs.status` values.
export enum class status : std::uint8_t {
  pending,   ///< Created, not yet validated.
  validated, ///< Checked and ready to be picked up.
  consumed,  ///< Picked up by a resumer. Terminal.
  abandoned, ///< Given up on. Terminal.
};

/// @brief Parse a `handoffs.status` value.
/// @param text The stored text.
/// @return The parsed status, or unset when `text` is not one of the four.
export auto status_from_text(std::string_view text) -> std::optional<status>;

/// @brief The stored text for a status — also the JSON and text-mode wire
/// value.
/// @param value The status.
/// @return Its canonical text.
export auto to_text(status value) -> std::string_view;

/// @brief True for `consumed` and `abandoned`, the two states with no
/// outgoing edges.
/// @param value The status.
/// @return Whether the status is terminal.
export auto is_terminal(status value) -> bool;

/// @brief A `handoffs` row. Mirrors zig's `handoff.Handoff`.
export struct handoff {
  std::int64_t                id{};                    ///< Row id.
  std::int64_t                from_snapshot_id{};      ///< The snapshot this handoff anchors on.
  std::optional<std::int64_t> to_session_id;           ///< Bound at `consume --session`, when supplied.
  std::string                 from_vendor;             ///< Vendor that created the handoff.
  std::optional<std::string>  to_vendor;               ///< Target vendor, when `--vendor` was passed.
  status                      state = status::pending; ///< Current status.
  std::optional<std::string>  validated_at;            ///< Set on the transition to `validated`.
  std::optional<std::string>  consumed_at;             ///< Set on the transition to `consumed`.
  std::string                 created_at;              ///< Creation timestamp.
  std::optional<std::string>  worktree_path;           ///< Worktree copied from the active claim, when one was held.
  std::optional<std::string>  repo_root;               ///< Repo root copied from the active claim.
  std::optional<std::string>  branch;                  ///< Branch copied from the active claim.
};

/// @brief Arguments to `create`. Mirrors zig's `handoff.CreateArgs`.
export struct create_args {
  std::int64_t                    from_snapshot_id{}; ///< The anchoring snapshot.
  std::string_view                from_vendor;        ///< Creating vendor.
  std::optional<std::string_view> to_vendor;          ///< Target vendor; unset OR EMPTY stores SQL NULL.
  std::optional<std::string_view> worktree_path;      ///< Worktree path; unset OR EMPTY stores SQL NULL.
  std::optional<std::string_view> repo_root;          ///< Repo root; unset OR EMPTY stores SQL NULL.
  std::optional<std::string_view> branch;             ///< Branch; unset OR EMPTY stores SQL NULL.
};

/// @brief Filter for `list`. Mirrors zig's `handoff.ListFilter`.
export struct list_filter {
  /// @brief Statuses to include. EMPTY MEANS `{pending}`, not "all" —
  /// that Go-parity default is why `planar handoff list --json` prints
  /// ZERO BYTES on a database whose only handoffs are validated.
  std::vector<status>         statuses;
  std::optional<std::int64_t> task_id; ///< Restrict to handoffs whose snapshot names this task.
};

/// @brief Error surface for this module. Mirrors zig's `handoff.Error`.
export enum class handoff_error : std::uint8_t {
  not_found,          ///< No handoff with that id.
  illegal_transition, ///< The requested status move is refused by the matrix.
  query_failed,       ///< An underlying SQL statement failed, or a row held an unparseable status.
};

/// @brief The injected status-matrix guard. Returns true when the move is
/// legal. See this module's header for why it is a parameter.
///
/// The port maps BOTH of the underlying matrix's errors —
/// `illegal_transition` and `unknown_status` — onto a single `false`,
/// reproducing zig's `handoff.validateTransition`, which folds
/// `error.UnknownStatus` into `Error.IllegalTransition` defensively.
export using transition_check = std::function<bool(status from, status to)>;

/// @brief Insert a `pending` handoff and return it as stored.
/// @param conn An open, migrated database connection.
/// @param args The row to write.
/// @return The stored handoff, or `handoff_error::query_failed`.
export auto create(db::connection& conn, const create_args& args) -> std::expected<handoff, handoff_error>;

/// @brief Read one handoff by id.
/// @param conn An open, migrated database connection.
/// @param id The handoff id.
/// @return The handoff, or `handoff_error::not_found`.
export auto show(db::connection& conn, std::int64_t id) -> std::expected<handoff, handoff_error>;

/// @brief Transition to `validated`, stamping `validated_at`.
///
/// Re-validating an already-`validated` handoff SUCCEEDS and re-stamps
/// `validated_at`, because the matrix treats `from == to` as an identity
/// no-op and the UPDATE then runs anyway. That is the oracle's behavior,
/// pinned deliberately: a second `handoff validate 1` returns exit 0 with a
/// LATER `validated_at` than the first.
/// @param conn An open, migrated database connection.
/// @param id The handoff to validate.
/// @param allowed The injected matrix guard.
/// @return The updated handoff, or the failure.
export auto validate(db::connection& conn, std::int64_t id, const transition_check& allowed)
    -> std::expected<handoff, handoff_error>;

/// @brief Transition to `consumed`, stamping `consumed_at` and optionally
/// binding `to_session_id`.
/// @param conn An open, migrated database connection.
/// @param id The handoff to consume.
/// @param session_id Bound to `to_session_id` when supplied; left untouched otherwise.
/// @param allowed The injected matrix guard.
/// @return The updated handoff, or the failure.
export auto consume(db::connection& conn, std::int64_t id, std::optional<std::int64_t> session_id,
                    const transition_check& allowed) -> std::expected<handoff, handoff_error>;

/// @brief Transition to `abandoned`.
///
/// `consumed_at` and `validated_at` are NOT cleared — abandoning a
/// validated handoff keeps its `validated_at`, which the oracle's own
/// `abandon --json` output shows.
///
/// The `--reason` text is recorded ONLY in the audit summary the Zig
/// original writes, and this tree has no audit table, so the reason is
/// accepted by the caller and does not reach the database at all. That is
/// not a regression introduced here: no column stores it in the oracle
/// either.
/// @param conn An open, migrated database connection.
/// @param id The handoff to abandon.
/// @param allowed The injected matrix guard.
/// @return The updated handoff, or the failure.
export auto abandon(db::connection& conn, std::int64_t id, const transition_check& allowed)
    -> std::expected<handoff, handoff_error>;

/// @brief List handoffs, newest first.
///
/// Ordered by `created_at desc` ALONE, with no id tiebreak — reproducing
/// the Zig original exactly. Two handoffs created inside the same
/// millisecond therefore have an unspecified relative order, and adding a
/// tiebreak here would be a divergence, not a fix.
/// @param conn An open, migrated database connection.
/// @param filter Status and task restrictions; see `list_filter`.
/// @return The matching handoffs.
export auto list(db::connection& conn, const list_filter& filter) -> std::expected<std::vector<handoff>, handoff_error>;

/// @brief The most-recent non-abandoned handoff for `task_id` that carries
/// a `worktree_path`, joined through `context_snapshots.task_id`.
///
/// The status filter is deliberately permissive: a CONSUMED handoff still
/// carries authoritative worktree context for the prior cycle.
/// @param conn An open, migrated database connection.
/// @param task_id The task.
/// @return The handoff, or unset when none qualifies.
export auto get_latest_with_worktree_for_task(db::connection& conn, std::int64_t task_id)
    -> std::expected<std::optional<handoff>, handoff_error>;

/// @brief The most-recent `pending`-or-`validated` handoff anchored at
/// `snapshot_id`.
/// @param conn An open, migrated database connection.
/// @param snapshot_id The anchoring snapshot.
/// @return The handoff, or unset when none is open.
export auto get_pending_for_snapshot(db::connection& conn, std::int64_t snapshot_id)
    -> std::expected<std::optional<handoff>, handoff_error>;

/// @brief Render one handoff as JSON.
///
/// Field order is FIXED and oracle-exact: the five always-present fields
/// `id`, `from_snapshot_id`, `from_vendor`, `status`, `created_at`, then
/// each optional in this order when and only when it is set —
/// `to_session_id`, `to_vendor`, `validated_at`, `consumed_at`,
/// `worktree_path`, `repo_root`, `branch`. An absent optional is OMITTED,
/// never emitted as `null`.
/// @param value The handoff.
/// @return The JSON object with NO trailing newline — a FRAGMENT, because
/// `list` composes many of these into NDJSON and `show` appends the
/// terminator itself.
export auto render_json(const handoff& value) -> std::string;

/// @brief Render `planar handoff show`'s text block.
/// @param value The handoff.
/// @return The complete stdout payload, WITH its trailing newline.
export auto render_text(const handoff& value) -> std::string;

/// @brief Render `planar handoff list --json` — NDJSON, one object per
/// line.
/// @param items The handoffs.
/// @return The complete stdout payload. EMPTY INPUT YIELDS ZERO BYTES, not
/// a bare newline and not `[]`; that is the same complete-payload
/// zero-byte case `workflow list --json` already pins.
export auto render_list_json(std::span<const handoff> items) -> std::string;

/// @brief Render `planar handoff list`'s table.
/// @param items The handoffs.
/// @return The complete stdout payload. Empty input yields the literal
/// `no handoffs\n`; a non-empty list yields a header row plus one row per
/// handoff, with `-` standing in for an unset `to_vendor`.
export auto render_list_text(std::span<const handoff> items) -> std::string;

} // namespace planar::engine::runtime::handoff
