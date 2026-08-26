/// @file planning.cppm
/// @brief `planar.engine.planning` — single import point re-exporting the
/// whole `lib/engine/planning` surface landed so far (plan 996, task
/// cpp-planning-verbs). Matches `lib/engine/identity/identity.cppm`'s
/// umbrella pattern. `artifact` is still NOT ported — each cycle here takes
/// ONE family (task 6188 took `question`, 6194 `decision`, 6195
/// `scenario`) rather than a shallow sweep across four; see
/// scenario.cppm's header for what that cycle cut and where. This umbrella
/// re-exports only what exists so far.
module;

export module planar.engine.planning;

export import planar.engine.planning.transitions;
export import planar.engine.planning.plan;
export import planar.engine.planning.task;
export import planar.engine.planning.question;
export import planar.engine.planning.decision;
export import planar.engine.planning.scenario;
export import planar.engine.planning.annotation;
export import planar.engine.planning.plan_step;
export import planar.engine.planning.test_spec_status;
