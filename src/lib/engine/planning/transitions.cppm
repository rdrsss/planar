/// @file transitions.cppm
/// @brief `planar.engine.planning.transitions` — per-entity-kind status
/// transition rules (tech-spec § "engine buckets", plan 996 task
/// cpp-planning-verbs).
///
/// Behavior-preserving port (D2) of the `.plan` and `.task` arms ONLY of
/// zig/src/engine/policy/status.zig's `check`. The Zig module also
/// enforces `.question`, `.scenario`, `.decision`, `.artifact`,
/// `.handoff`, and `.annotation` arms; those entity kinds are not ported
/// by this task at all (see this task's coder report and
/// engine_planning's CMakeLists.txt file header for the scoping
/// rationale), so porting their transition arms here would be dead code
/// with no caller. Only the two arms this task's entities actually need
/// are ported; the rest is intentionally left for the task(s) that port
/// those entities.
module;

export module planar.engine.planning.transitions;

import std;

namespace planar::engine::planning {

/// @brief Which entity kind's transition matrix to apply. Mirrors the
/// `.plan` / `.task` arms of zig's `policy.status.EntityKind`.
export enum class transition_kind : std::uint8_t {
  plan,
  task,
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
