//! handlers/test_spec/cmd.zig — `planar test-spec {status}`
//!
//! Read-only test-spec coverage inspector. Mirrors Go's
//! `planning.TestSpecCmd` (src/cmd/planar/internal/planning/test_spec.go).
//! The verb name uses a hyphen on the CLI (`test-spec`) while the
//! handler directory is `test_spec/` per the per-entity layout
//! convention.

const cli = @import("cli");

const status = @import("status.zig");

pub const verb: cli.Cmd = .{
    .name = "test-spec",
    .desc = "Test-spec coverage inspectors.",
    .cmds = &.{
        .{
            .name = "status",
            .desc = "Print per-milestone test-spec coverage for an anchor plan.",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "plan", .kind = .string, .required = true, .desc = "Plan slug or numeric id" },
            },
            .run = cli.handler(status.handle),
        },
    },
};
