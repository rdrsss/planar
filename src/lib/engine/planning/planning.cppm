/// @file planning.cppm
/// @brief `planar.engine.planning` — single import point re-exporting the
/// whole `lib/engine/planning` surface landed so far (plan 996, task
/// cpp-planning-verbs). Matches `lib/engine/identity/identity.cppm`'s
/// umbrella pattern. `scenario`, `decision` and `artifact` are still NOT
/// ported (task 6188 took the `question` family alone — see
/// question.cppm's header and that task's coder report for the one-family
/// scoping rationale) — this umbrella re-exports only what exists so far.
module;

export module planar.engine.planning;

export import planar.engine.planning.transitions;
export import planar.engine.planning.plan;
export import planar.engine.planning.task;
export import planar.engine.planning.question;
export import planar.engine.planning.annotation;
export import planar.engine.planning.plan_step;
export import planar.engine.planning.test_spec_status;
