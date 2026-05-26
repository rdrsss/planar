//! integration_tests/parity_resume_test.zig
//!
//! Cluster J-resume-content-loss parity tests for plan 351 (2 audit
//! rows). See parity-triage.md §J-resume-content-loss.
//!
//! Both binaries produce a resume packet but zig's Section 4 says
//! "operational plane not available in M7 build (external plane lands
//! in M8)" while Go renders the operational-plane content. The
//! `--json` variant is worse: zig returns a 1-line minimal JSON while
//! Go returns the full 180-line packet with all 8 documented sections.
//!
//! Either M8 work was never ported to zig, or the M7-gate string is
//! leftover dead text. Two test blocks:
//! - Resume text must not carry the M7-gate guard string.
//! - Resume --json must carry the structured 8-section packet shape
//!   (not a 1-line minimal object).
//!
//! Today: both fail. Phase 4 strips the M7 guard from
//! src/engine/runtime/resume.zig and restores the rich JSON shape.

const std = @import("std");
const harness = @import("harness");

const TaskJSON = struct {
    id: i64,
};

test "parity: 'planar resume <id>' text omits the M7-gate guard string (Cluster J text; red until M7 guard removed)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const t = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", "--next-action", "verify", "RESUME_TEXT_TEST",
    });
    const id_str = std.fmt.allocPrint(arena, "{d}", .{t.id}) catch unreachable;

    const stdout = suite.mustRun(&.{ "resume", id_str });
    defer gpa.free(stdout);

    // Forbidden substrings — these mark the M7 leftover guard zig
    // emits in place of the operational-plane content.
    const forbidden = [_][]const u8{
        "M7 build",
        "external plane lands in M8",
    };

    var present: std.ArrayList([]const u8) = .empty;
    defer present.deinit(gpa);
    inline for (forbidden) |needle| {
        if (std.mem.containsAtLeast(u8, stdout, 1, needle)) {
            present.append(gpa, needle) catch @panic("OOM");
        }
    }

    if (present.items.len > 0) {
        std.debug.print("\nresume text carries {d} forbidden M7-gate string(s):\n", .{present.items.len});
        for (present.items) |p| std.debug.print("  - '{s}'\n", .{p});
        try std.testing.expect(false);
    }
}

test "parity: 'planar resume --json <id>' operational_plane refresh_note omits M7 guard (Cluster J json; red until M7 guard removed)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const t = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", "--next-action", "verify", "RESUME_JSON_TEST",
    });
    const id_str = std.fmt.allocPrint(arena, "{d}", .{t.id}) catch unreachable;

    const stdout = suite.mustRun(&.{ "resume", "--json", id_str });
    defer gpa.free(stdout);

    const trimmed = std.mem.trim(u8, stdout, " \n");
    const parsed = std.json.parseFromSlice(std.json.Value, arena, trimmed, .{
        .allocate = .alloc_always,
    }) catch |e| {
        std.debug.print("\nresume --json parse failed: {s}\nraw: {s}\n", .{ @errorName(e), stdout });
        try std.testing.expect(false);
        unreachable;
    };
    try std.testing.expect(parsed.value == .object);
    const obj = parsed.value.object;

    // Inspect the operational_plane sub-object. Zig today emits
    // `refresh_note: "operational plane not available in M7 build
    // (external plane lands in M8)"` — the M7 leftover guard string.
    // Phase 4 removes it; this test pins the absence.
    const op_plane = obj.get("operational_plane") orelse {
        std.debug.print("\nresume --json missing 'operational_plane' key; raw: {s}\n", .{stdout});
        try std.testing.expect(false);
        unreachable;
    };
    if (op_plane != .object) {
        std.debug.print("\nresume --json 'operational_plane' is not an object; raw: {s}\n", .{stdout});
        try std.testing.expect(false);
    }

    if (op_plane.object.get("refresh_note")) |note| {
        if (note == .string) {
            const s = note.string;
            if (std.mem.containsAtLeast(u8, s, 1, "M7 build") or
                std.mem.containsAtLeast(u8, s, 1, "external plane lands in M8"))
            {
                std.debug.print(
                    "\nresume --json operational_plane.refresh_note carries M7 guard: '{s}'\nraw: {s}\n",
                    .{ s, stdout },
                );
                try std.testing.expect(false);
            }
        }
    }
}
