//! Integration contract from an authoritative packet through coder brief.

const std = @import("std");
const packet = @import("routing_packet");

test "current packet is the sole coder brief source and stale lineage fails closed" {
    const allocator = std.testing.allocator;
    const citations = [_]packet.Evidence{
        .{ .kind = "product_spec", .id = 539, .locator = "product-spec.md#requirements", .text = "product" },
        .{ .kind = "tech_spec", .id = 540, .locator = "tech-spec.md#dispatch-time-packet-compiler", .text = "tech" },
        .{ .kind = "roadmap", .id = 541, .locator = "roadmap.md#cross-stack-contract", .text = "roadmap" },
        .{ .kind = "test_spec", .id = 542, .locator = "test-spec.md#coder-brief", .text = "tests" },
    };
    const linked = [_]packet.Evidence{
        .{ .kind = "plan", .id = 950, .locator = "plan:950", .text = "owning plan" },
    };
    const fresh = [_]packet.Evidence{
        .{ .kind = "acceptance_complete", .id = 5526, .locator = "task:5526#acceptance", .text = "true", .source_digest = "current", .current_digest = "current" },
    };
    const input: packet.TaskInput = .{
        .task_id = 5526,
        .title = "Compile authoritative packets",
        .body = "Preserve exact packet evidence.",
        .next_action = "Compile live linked context, reject incomplete context, and run four named gates.",
        .acceptance_criteria = "The coder brief preserves exact mandatory fields and digest.",
        .owning_plans = &linked,
        .anchor_plans = &linked,
        .citations = &citations,
        .decisions = &linked,
        .questions = &.{},
        .scenarios = &linked,
        .dependencies = &linked,
        .touches = &linked,
        .claims = &linked,
        .validation_gates = &linked,
        .facts = &fresh,
    };

    var compiled = try packet.compileTask(allocator, input);
    defer compiled.deinit(allocator);
    const brief = try packet.coderBrief(compiled);
    try std.testing.expectEqualStrings(input.title, brief.title);
    try std.testing.expectEqualStrings(input.acceptance_criteria, brief.acceptance_criteria);
    try std.testing.expectEqualStrings(&compiled.digest, &brief.packet_digest);

    const stale = [_]packet.Evidence{
        .{ .kind = "acceptance_complete", .id = 5526, .locator = "task:5526#acceptance", .text = "true", .source_digest = "prior", .current_digest = "changed" },
    };
    var stale_input = input;
    stale_input.facts = &stale;
    var rejected = try packet.compileTask(allocator, stale_input);
    defer rejected.deinit(allocator);
    try std.testing.expectError(error.PacketNotReady, packet.coderBrief(rejected));
}
