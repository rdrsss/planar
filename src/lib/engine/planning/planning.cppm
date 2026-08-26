/// @file planning.cppm
/// @brief `planar.engine.planning` — single import point re-exporting the
/// whole `lib/engine/planning` surface landed so far (plan 996, task
/// cpp-planning-verbs). Matches `lib/engine/identity/identity.cppm`'s
/// umbrella pattern. `scenario` and `artifact` are still NOT ported —
/// each cycle here takes ONE family (task 6188 took `question`, task 6194
/// took `decision`) rather than a shallow sweep across four; see
/// decision.cppm's header for what that cycle cut and where. This umbrella
/// re-exports only what exists so far.
module;

export module planar.engine.planning;

export import planar.engine.planning.transitions;
export import planar.engine.planning.plan;
export import planar.engine.planning.task;
export import planar.engine.planning.question;
export import planar.engine.planning.decision;
export import planar.engine.planning.annotation;
export import planar.engine.planning.plan_step;
export import planar.engine.planning.test_spec_status;
