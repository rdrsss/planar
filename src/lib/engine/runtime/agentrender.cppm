/// @file agentrender.cppm
/// @brief `planar.engine.runtime.agentrender` — the stdout payloads every
/// `planar-agent` claim verb writes, in both text and JSON form (plan 996,
/// task 6038).
///
/// Behavior-preserving port (D2) of
/// zig/src/engine/runtime/agentactivity/json.zig plus the `print` calls in
/// each `zig/src/cmd/planar-agent/handlers/*.zig`.
///
/// ## Terminator contract
///
/// Every `*_text` and `*_json` function here returns a COMPLETE stdout
/// payload — the oracle's exact bytes, trailing newline INCLUDED — and the
/// layer-3 handler writes the string verbatim and appends nothing. The
/// `append_*` helpers are the other half of the contract: they are
/// FRAGMENTS composed into a larger document and return no terminator.
/// Each `@return` says which it is; the rule is per-function, not a
/// blanket policy (task 6114, and `planar.cli.output`'s `emit()` is
/// explicitly NOT for renderer-backed leaves because it appends
/// unconditionally).
///
/// ## Field order is the contract
///
/// The Zig originals are hand-rolled `w.print` calls, so the emitted key
/// order is whatever the source says — there is no serializer to
/// normalise it. Every array below therefore reproduces that order
/// literally, and the parity tests diff whole lines. Reordering a field
/// for tidiness is a parity break.
///
/// Two fields `writeClaim` can emit are deliberately absent here:
/// `entity_scope` and `latest_action`. Every `planar-agent` call site
/// passes null/false for both, so they never appear in this binary's
/// output; they exist for `planar-watch ps` / `claims` and belong to that
/// binary's port.
module;

export module planar.engine.runtime.agentrender;

import std;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentatomic;

namespace planar::engine::runtime::agentrender {

namespace aa = agentactivity;

// =========================================================================
// Fragments
// =========================================================================

/// @brief Append a claim as a JSON object.
/// @param out The buffer to append to.
/// @param value The claim.
///
/// NOTE: appends a FRAGMENT — no trailing newline. The `*_json` and
/// `*_text` functions below return COMPLETE payloads instead.
export auto append_claim(std::string& out, const aa::claim& value) -> void;

/// @brief Append a task as a JSON object.
/// @param out The buffer to append to.
/// @param value The task row.
///
/// NOTE: appends a FRAGMENT — no trailing newline. The `*_json` and
/// `*_text` functions below return COMPLETE payloads instead.
export auto append_task(std::string& out, const aa::task_row& value) -> void;

/// @brief Append an action as a JSON object.
///
/// `metadata` is emitted as a JSON STRING, not spliced as a nested
/// object — the column is opaque text this tree never parses, and
/// splicing it would turn a malformed blob into malformed output.
/// @param out The buffer to append to.
/// @param value The action row.
///
/// NOTE: appends a FRAGMENT — no trailing newline. The `*_json` and
/// `*_text` functions below return COMPLETE payloads instead.
export auto append_action(std::string& out, const aa::action& value) -> void;

// =========================================================================
// pull / peek
// =========================================================================

/// @brief `{"ok":true,"no_work":true}` — shared verbatim by `pull` and `peek`.
/// @return The COMPLETE payload, newline included.
export auto no_work_json() -> std::string;

/// @brief `no_work` — shared verbatim by `pull` and `peek`.
/// @return The COMPLETE payload, newline included.
export auto no_work_text() -> std::string;

/// @brief `pull`'s success payload.
/// @param result The pull outcome (must not be `no_work`).
/// @param task The claimed task.
/// @return The COMPLETE payload, newline included.
export auto pull_json(const agentatomic::pull_result& result, const aa::task_row& task) -> std::string;

/// @brief `pulled task:<id> claim:<token> action:<id>`.
/// @param result The pull outcome (must not be `no_work`).
/// @return The COMPLETE payload, newline included.
export auto pull_text(const agentatomic::pull_result& result) -> std::string;

/// @brief `peek`'s success payload.
/// @param task The task `pull` would take.
/// @return The COMPLETE payload, newline included.
export auto peek_json(const aa::task_row& task) -> std::string;

/// @brief `next: task:<id> status:<status>`.
/// @param task The task `pull` would take.
/// @return The COMPLETE payload, newline included.
export auto peek_text(const aa::task_row& task) -> std::string;

// =========================================================================
// claim / heartbeat
// =========================================================================

/// @brief The `{"ok":true,"claim_token":…,"claim":…}` envelope both
/// `claim` and `heartbeat` emit — byte-identical between the two verbs.
/// @param value The claim.
/// @return The COMPLETE payload, newline included.
export auto claim_json(const aa::claim& value) -> std::string;

/// @brief `claim:<token> entity:<kind>:<id> status:<status>`.
/// @param value The claim.
/// @return The COMPLETE payload, newline included.
export auto claim_text(const aa::claim& value) -> std::string;

/// @brief `ok claim:<token> expires:<lease_expires_at>`.
///
/// The expiry it prints is the NEW absolute one, which is what makes the
/// reset-not-extend behaviour visible to an operator who reads the line —
/// see `agentactivity::heartbeat_claim`.
/// @param value The refreshed claim.
/// @return The COMPLETE payload, newline included.
export auto heartbeat_text(const aa::claim& value) -> std::string;

// =========================================================================
// terminal verbs
// =========================================================================

/// @brief The envelope all four terminal verbs share.
/// @param result The terminal outcome.
/// @param task The task in its new state.
/// @return The COMPLETE payload, newline included.
export auto terminal_json(const agentatomic::terminal_result& result, const aa::task_row& task) -> std::string;

/// @brief `ok task:<id> status:<status> claim_status:<status>`.
/// @param result The terminal outcome.
/// @param task The task in its new state.
/// @return The COMPLETE payload, newline included.
export auto terminal_text(const agentatomic::terminal_result& result, const aa::task_row& task) -> std::string;

// =========================================================================
// abort / claim-associate
// =========================================================================

/// @brief `abort`'s payload, which carries the aborting session id — the
/// one envelope that is NOT the shared claim envelope.
/// @param value The aborted claim.
/// @param aborting_session The session that performed the abort.
/// @return The COMPLETE payload, newline included.
export auto abort_json(const aa::claim& value, std::int64_t aborting_session) -> std::string;

/// @brief `aborted claim:<token> by session:<id>`.
/// @param value The aborted claim.
/// @param aborting_session The session that performed the abort.
/// @return The COMPLETE payload, newline included.
export auto abort_text(const aa::claim& value, std::int64_t aborting_session) -> std::string;

/// @brief `{"ok":true,"updated":<n>}`.
/// @param updated The number of claims stamped.
/// @return The COMPLETE payload, newline included.
export auto associate_json(std::int64_t updated) -> std::string;

/// @brief `ok updated:<n> claim:<token>`.
/// @param updated The number of claims stamped.
/// @param claim_token The token the operator passed.
/// @return The COMPLETE payload, newline included.
export auto associate_text(std::int64_t updated, std::string_view claim_token) -> std::string;

// =========================================================================
// action start / end
// =========================================================================

/// @brief `{"ok":true,"action_id":<id>,"action":…}` — shared by
/// `action start` and `action end`.
/// @param value The action row.
/// @return The COMPLETE payload, newline included.
export auto action_json(const aa::action& value) -> std::string;

/// @brief `action:<id> kind:<kind> claim:<token>`.
/// @param value The started action.
/// @param claim_token The claim it attached to.
/// @return The COMPLETE payload, newline included.
export auto action_start_text(const aa::action& value, std::string_view claim_token) -> std::string;

/// @brief `ok action:<id> outcome:<outcome>`.
/// @param action_id The action that was closed.
/// @param result The outcome recorded.
/// @return The COMPLETE payload, newline included.
export auto action_end_text(std::int64_t action_id, aa::outcome result) -> std::string;

// =========================================================================
// reconcile
// =========================================================================

/// @brief `reconcile`'s JSON payload.
///
/// `dry_run` selects between two genuinely different documents, not just
/// different numbers: the dry-run form appends `candidates` and
/// `run_candidates` arrays that the applied form omits entirely.
///
/// KNOWN DEFECT, REPRODUCED DELIBERATELY: `run_identifier` inside
/// `run_candidates` is emitted WITHOUT JSON escaping, exactly as the
/// oracle's `"{s}"` format string does it, so an identifier containing a
/// quote or backslash produces invalid JSON. Every other string in this
/// module is escaped. It is preserved under D2 and pinned by a test so
/// that fixing it is a deliberate contract change rather than an
/// accidental divergence.
/// @param result The claim sweep result.
/// @param runs The run sweep result.
/// @param dry_run Whether this was a dry run.
/// @return The COMPLETE payload, newline included.
export auto reconcile_json(const aa::reconcile_result& result, const aa::reconcile_runs_result& runs, bool dry_run)
    -> std::string;

/// @brief `reconcile`'s text payload — a one-line summary when applied, a
/// header plus one indented line per candidate when dry-run.
/// @param result The claim sweep result.
/// @param runs The run sweep result.
/// @param dry_run Whether this was a dry run.
/// @return The COMPLETE payload, newline included.
export auto reconcile_text(const aa::reconcile_result& result, const aa::reconcile_runs_result& runs, bool dry_run)
    -> std::string;

} // namespace planar::engine::runtime::agentrender
