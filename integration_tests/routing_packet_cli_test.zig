//! Black-box contract for `planar task packet` — the CLI surface an operator
//! uses to ask "is this task ready to dispatch, and if not, why?".
//!
//! The module-level assembler is covered by `routing_packet_test.zig`; this
//! suite pins the operator-visible contract: the JSON shape, the readiness
//! reasons, and the digest's role as a change detector.

const std = @import("std");
const harness = @import("harness");

fn contains(haystack: []const u8, needle: []const u8) !void {
    try std.testing.expect(std.mem.indexOf(u8, haystack, needle) != null);
}

/// Pull an integer field (e.g. `"id"`) out of a JSON payload.
fn extractIntField(json: []const u8, key: []const u8) ?i64 {
    const idx = std.mem.indexOf(u8, json, key) orelse return null;
    var i = idx + key.len;
    while (i < json.len and (json[i] == ' ' or json[i] == ':' or json[i] == '\t')) i += 1;
    var end = i;
    while (end < json.len and json[end] >= '0' and json[end] <= '9') end += 1;
    if (end == i) return null;
    return std.fmt.parseInt(i64, json[i..end], 10) catch null;
}

/// Pull the `"digest":"..."` value out of a packet payload.
fn digestOf(payload: []const u8) ![]const u8 {
    const key = "\"digest\":\"";
    const start = (std.mem.indexOf(u8, payload, key) orelse return error.NoDigest) + key.len;
    const end = std.mem.indexOfScalarPos(u8, payload, start, '"') orelse return error.NoDigest;
    return payload[start..end];
}

test "task packet compiles a readiness verdict an operator can act on" {
    var suite = harness.Suite.init(std.testing.allocator);
    defer suite.deinit();
    const root = suite.registerProject("packet-cli");
    suite.addAssoc("packet-cli", null);

    const plan_out = suite.mustRunInDir(root, &.{ "plan", "create", "Routing packet plan", "--json" });
    defer suite.allocator.free(plan_out);
    const plan_num = extractIntField(plan_out, "\"id\"") orelse return error.NoPlanId;
    const plan_id = try std.fmt.allocPrint(suite.allocator, "{d}", .{plan_num});
    defer suite.allocator.free(plan_id);

    const task_out = suite.mustRunInDir(root, &.{
        "task", "add", "--plan", plan_id, "--json", "Compile a packet",
    });
    defer suite.allocator.free(task_out);
    const task_num = extractIntField(task_out, "\"id\"") orelse return error.NoTaskId;
    const task_id = try std.fmt.allocPrint(suite.allocator, "{d}", .{task_num});
    defer suite.allocator.free(task_id);

    // A bare task is not dispatchable, and the packet must say so by NAME —
    // an operator cannot act on a bare "not ready".
    const packet = suite.mustRunInDir(root, &.{ "task", "packet", task_id, "--json" });
    defer suite.allocator.free(packet);
    try contains(packet, "\"task_id\":");
    try contains(packet, "\"canonical\":");
    try contains(packet, "\"digest\":");
    try contains(packet, "\"reasons\":");
    try contains(packet, "missing_acceptance_section");

    // The digest is a change detector: recompiling unchanged state must
    // reproduce it, otherwise it could never certify "this is the same packet
    // the dispatch was authorized against".
    const again = suite.mustRunInDir(root, &.{ "task", "packet", task_id, "--json" });
    defer suite.allocator.free(again);
    try std.testing.expectEqualStrings(try digestOf(packet), try digestOf(again));

    // Changing the task's content must move the digest. A packet that ignored
    // an edit would authorize a dispatch against state that no longer exists.
    const edited = suite.mustRunInDir(root, &.{
        "task",                                                        "update",
        task_id,                                                       "--next-action",
        "Read src/engine/routing/packet.zig, then compile the packet",
    });
    suite.allocator.free(edited);

    const after = suite.mustRunInDir(root, &.{ "task", "packet", task_id, "--json" });
    defer suite.allocator.free(after);
    try std.testing.expect(!std.mem.eql(u8, try digestOf(packet), try digestOf(after)));
}

test "task packet rejects an unknown task instead of emitting an empty packet" {
    var suite = harness.Suite.init(std.testing.allocator);
    defer suite.deinit();
    const root = suite.registerProject("packet-cli-missing");
    suite.addAssoc("packet-cli-missing", null);

    // Silently returning a well-formed packet for a task that does not exist
    // would be the worst failure here: it reads as "ready enough".
    const res = suite.execWithInDir(root, &.{ "task", "packet", "999999", "--json" }, &.{});
    defer res.deinit(suite.allocator);
    try std.testing.expect(res.term.exited != 0);
}

test "skills points at scriptorium instead of pretending to still render" {
    var suite = harness.Suite.init(std.testing.allocator);
    defer suite.deinit();
    const root = suite.registerProject("skills-stub");

    // `planar skills` is a signpost left behind when plan 918 moved rendering
    // out of the binary. An operator reaching for the retired verb must be
    // told where rendering actually lives — silence would read as "rendering
    // happened".
    const out = suite.mustRunInDir(root, &.{"skills"});
    defer suite.allocator.free(out);
    try contains(out, "scriptorium");
    try contains(out, "no subcommands");

    // It really is a leaf: a subcommand must be rejected, not silently ignored.
    const res = suite.execWithInDir(root, &.{ "skills", "render" }, &.{});
    defer res.deinit(suite.allocator);
    try std.testing.expect(res.term.exited != 0);
}
