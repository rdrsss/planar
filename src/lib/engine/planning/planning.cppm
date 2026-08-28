/// @file planning.cppm
/// @brief `planar.engine.planning` — single import point re-exporting the
/// whole `lib/engine/planning` surface landed so far (plan 996, task
/// cpp-planning-verbs). Matches `lib/engine/identity/identity.cppm`'s
/// umbrella pattern. Each cycle here took ONE family (task 6188 took
/// `question`, 6194 `decision`, 6195 `scenario`, 6196 `artifact`) rather
/// than a shallow sweep across four; see each module's header for what its
/// cycle cut and where.
///
/// With `artifact` landed at task 6196 the planning ENGINE surface is
/// complete — every planning entity's CRUD half is ported. What remains
/// unported is a CMD-layer dependency, not an engine one: `editflow`
/// (zig/src/cmd/planar/editflow.zig) still gates the
/// `edit`/`view`/`diff`/`review` drafting quartet on all FOUR families.
module;

export module planar.engine.planning;

export import planar.engine.planning.transitions;
export import planar.engine.planning.plan;
export import planar.engine.planning.task;
export import planar.engine.planning.question;
export import planar.engine.planning.decision;
export import planar.engine.planning.scenario;
export import planar.engine.planning.artifact;
export import planar.engine.planning.annotation;
export import planar.engine.planning.plan_step;
export import planar.engine.planning.test_spec_status;
export import planar.engine.planning.descendants;
