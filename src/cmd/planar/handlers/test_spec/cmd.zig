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
    .long_desc = "Commands for inspecting test-spec coverage of a plan's tasks.\n\n  'test-spec status' prints a per-milestone breakdown of which tasks\n  have verifying scenarios. This is a read-only complement to the\n  ingest-time coverage gate (see `planar spec ingest --strict`).",
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
