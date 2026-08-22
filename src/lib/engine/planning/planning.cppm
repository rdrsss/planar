/// @file planning.cppm
/// @brief `planar.engine.planning` — single import point re-exporting the
/// whole `lib/engine/planning` surface landed so far (plan 996, task
/// cpp-planning-verbs). Matches `lib/engine/identity/identity.cppm`'s
/// umbrella pattern. `question`, `scenario`, `decision`, `artifact`, and
/// `plan_step` are NOT ported by this task (see task.cppm/plan.cppm file
/// headers and this task's coder report for the scoping rationale) — this
/// umbrella re-exports only what exists so far.
module;

export module planar.engine.planning;

export import planar.engine.planning.transitions;
export import planar.engine.planning.plan;
export import planar.engine.planning.task;
