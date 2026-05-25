//! engine/planning — domain barrel: plan, task, question, scenario,
//! decision, artifact, annotation.
//!
//! The work-artifact layer of Planar's data plane. Each submodule owns
//! one entity kind: its struct shape, its CRUD, its `renderText`. All
//! mutations route through engine.policy.* (scope_guard, status,
//! audit) before touching the DB.

pub const plan = @import("planning/plan.zig");
pub const plan_step = @import("planning/plan_step.zig");
pub const task = @import("planning/task.zig");
pub const question = @import("planning/question.zig");
pub const scenario = @import("planning/scenario.zig");
pub const decision = @import("planning/decision.zig");
pub const artifact = @import("planning/artifact.zig");
pub const annotation = @import("planning/annotation.zig");
pub const test_spec_status = @import("planning/test_spec_status.zig");

test {
    _ = plan;
    _ = plan_step;
    _ = task;
    _ = question;
    _ = scenario;
    _ = decision;
    _ = artifact;
    _ = annotation;
    _ = test_spec_status;
}
