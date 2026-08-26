/// @file transitions.cppm
/// @brief `planar.engine.planning.transitions` — per-entity-kind status
/// transition rules (tech-spec § "engine buckets", plan 996 task
/// cpp-planning-verbs).
///
/// Behavior-preserving port (D2) of the `.plan`, `.task` and `.annotation`
/// arms of zig/src/engine/policy/status.zig's `check`. The Zig module also
/// enforces `.question`, `.scenario`, `.decision`, `.artifact` and
/// arms; those entity kinds are not ported yet, so porting their
/// transition arms here would be dead code with no caller. Only the arms
/// whose entities exist in this tree are ported; the rest is intentionally
/// left for the task(s) that port those entities.
///
/// The `.handoff` arm landed with task 6040, which ported
/// `planar.engine.runtime.handoff` — the caller that needed it. It is
/// reached by INJECTION rather than by import, because that caller lives
/// in a sibling layer-2 bucket and cmake/architecture.cmake FATALs on an
/// `engine_* -> engine_*` edge: `handoff::create`/`validate`/`consume`/
/// `abandon` take a `transition_check` callable and `cmd_planar` — which is
/// layer 3 and may depend on both buckets — supplies this function bound to
/// `transition_kind::handoff`. That is the same seam
/// `agentatomic::task_policy` already uses, and for the same reason: the
/// status matrix stays in ONE file instead of being copied into a second
/// bucket where the two could drift.
///
/// The `.annotation` arm landed with task 6094 alongside
/// `planar.engine.planning.annotation`, which is the caller that needed
/// it. `annotation::transition` maps this module's
/// `illegal_transition` AND `unknown_status` back to its own
/// `annotation_error::terminal_status`, exactly as zig's
/// `annotation.transition` does — that spelling is what `bulk-*` and
/// `sweep` observe.
module;

export module planar.engine.planning.transitions;

import std;

namespace planar::engine::planning {

/// @brief Which entity kind's transition matrix to apply. Mirrors the
/// `.plan` / `.task` arms of zig's `policy.status.EntityKind`.
export enum class transition_kind : std::uint8_t {
  plan,
  task,
  question,
  annotation,
  handoff,
};

/// @brief Error surface for `check_transition`.
export enum class transition_error : std::uint8_t {
  illegal_transition, ///< The move is not in the legal edge set for `kind`.
  unknown_status,     ///< `from` is not a recognized status for `kind`.
};

/// @brief Validate a status transition for the given entity kind. Mirrors
/// zig's `policy.status.check`.
///
/// Identity transitions (`from == to`) are always a no-op success,
/// regardless of `kind` or `force`, matching the Zig original's
/// unconditional top-of-function check.
///
/// Plan matrix (status set: draft, active, paused, done, abandoned):
///   draft    -> active
///   active   -> {paused, done, abandoned}
///   paused   -> active
///   done, abandoned -> terminal (no outgoing operator edges)
/// `force` has no effect on the plan arm — `plan update` has no --force
/// flag in the Zig original, so this port never receives `force=true` for
/// `kind=plan`, but the parameter is still honored uniformly (see below).
///
/// Task matrix (status set: todo, doing, blocked, done, cancelled):
///   todo     -> {doing, blocked, cancelled}
///   doing    -> {todo, blocked, done, cancelled}
///   blocked  -> {doing, done, cancelled}
///   done, cancelled -> terminal for bare update; `force=true` bypasses
///   the matrix entirely (the call site is responsible for recording any
///   reopen-audit row it wants).
///
/// Question matrix (status set: open, answered, wontfix):
///   open              -> {answered, wontfix}
///   answered, wontfix -> TERMINAL; every outgoing edge is refused
/// This arm NEVER reports `unknown_status`: an unrecognized `from` is
/// refused as `illegal_transition` alongside the two terminal states,
/// because the Zig original's question branch has no unknown-source arm.
/// That asymmetry with the plan/task arms is reproduced deliberately (D2) —
/// it is operator-visible as the error name in `question wontfix:
/// IllegalTransition`.
///
/// Because `check_transition` short-circuits on `from == to` BEFORE
/// consulting any arm, `answered -> answered` and `wontfix -> wontfix`
/// succeed: re-answering an answered question overwrites its answer, and
/// re-`wontfix`-ing bumps `updated_at`. Both were confirmed by running the
/// oracle, not inferred. `answered -> wontfix` is refused.
/// `force` has no effect — the question verbs expose no `--force`.
///
/// Annotation matrix (status set: active, resolved, dismissed, archived —
/// the retention-tier model, plan 692):
///   active            -> {resolved, dismissed, archived}
///   resolved          -> {archived}   (outcome state may progress)
///   dismissed         -> {archived}   (outcome state may progress)
///   archived          -> terminal; the SOLE final state, no outgoing edges
/// `resolved -> dismissed` and `dismissed -> resolved` are refused: outcome
/// states never move laterally. `force` has no effect on this arm — the
/// annotate verbs expose no `--force`.
///
/// Handoff matrix (status set: pending, validated, consumed, abandoned):
///   pending           -> {validated, consumed, abandoned}
///   validated         -> {consumed, abandoned}
///   consumed          -> terminal
///   abandoned         -> terminal
/// Validation is not reversible: there is no `validated -> pending` edge.
/// `force` has no effect — every handoff call site passes false.
///
/// @param kind Which entity's matrix to apply.
/// @param from The current status text.
/// @param to The desired status text.
/// @param force When true, bypasses the matrix unconditionally (task arm
/// only — see above; harmless no-op-relevant for the plan arm since no
/// caller passes true there).
/// @return Success when the transition is legal (or force=true, or
/// identity), `transition_error::illegal_transition` when refused, or
/// `transition_error::unknown_status` when `from` is not a recognized
/// status for `kind`.
export auto check_transition(transition_kind kind, std::string_view from, std::string_view to, bool force)
    -> std::expected<void, transition_error>;

} // namespace planar::engine::planning
