//! integration_tests/remaining_coverage_test.zig
//!
//! Closes the last batch of black-box coverage gaps surfaced by the
//! codebase review that are NOT blocked by external infrastructure:
//!   - the entity-link verbs `task link` / `plan link` / `artifact link`
//!     (distinct from the `links add` entity-link verb);
//!   - the `$EDITOR` flows `config edit`, `plan/question/scenario/decision
//!     edit`, and `workbench edit`, driven with a no-op editor
//!     (PLANAR_EDITOR=/usr/bin/true), mirroring editflow_edit_test;
//!   - `annotate bulk-dismiss` and `annotate verify`.
//!
//! Also locks the session-timeline behavior of `capture note` / `command` /
//! `file`, plus the explicit NotImplemented contracts of the two remaining
//! shipped stubs (`audit publish-decision`, `workbench publish`). `ext create`
//! is covered against the in-process Jira server in propagate_faithful_test.
//!
//! Run via: zig build test-integration

const std = @import("std");
const harness = @import("harness");

const Id = struct { id: i64 };

const SessionOpen = struct { id: i64 };
const CaptureResult = struct { ok: bool, session_id: i64 };
const Timeline = struct {
    entries: []const Entry,

    const Entry = struct {
        prefix: []const u8,
        body: []const u8,
    };
};

fn idStr(arena: std.mem.Allocator, id: i64) []const u8 {
    return std.fmt.allocPrint(arena, "{d}", .{id}) catch unreachable;
}

fn expectOk(suite: *harness.Suite, gpa: std.mem.Allocator, args: []const []const u8, env: []const harness.Suite.ExtraEnvEntry) void {
    const res = suite.execWith(args, env);
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("\n{any} failed (exit {any})\nstdout: {s}\nstderr: {s}\n", .{ args, res.term, res.stdout, res.stderr });
        @panic("expectOk: non-zero exit");
    }
}

test "scenario: entity-link verbs task/plan/artifact link create edges" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("entity-link-verbs");
    const p1 = suite.mustRunJSON(Id, arena, &.{ "plan", "create", "--json", "Link plan one" });
    const p2 = suite.mustRunJSON(Id, arena, &.{ "plan", "create", "--json", "Link plan two" });
    const p1s = idStr(arena, p1.id);
    const p2ref = std.fmt.allocPrint(arena, "plan:{d}", .{p2.id}) catch unreachable;
    const task = suite.mustRunJSON(Id, arena, &.{ "task", "add", "--json", "--plan", p1s, "--next-action", "go", "Linkable task" });
    const artifact = suite.mustRunJSON(Id, arena, &.{ "artifact", "add", "--json", "--plan", p1s, "--kind", "tech_spec", "Linkable artifact" });

    const task_id = idStr(arena, task.id);
    const artifact_id = idStr(arena, artifact.id);

    // task link <task> <ref> --relationship; plan link; artifact link.
    // (artifact add --plan already made artifact->p1 derives-from, so link
    // the artifact to p2 to avoid the unique-edge conflict.)
    gpa.free(suite.mustRun(&.{ "task", "link", task_id, p2ref, "--relationship", "blocks", "--json" }));
    gpa.free(suite.mustRun(&.{ "plan", "link", p1s, p2ref, "--relationship", "blocks", "--json" }));
    gpa.free(suite.mustRun(&.{ "artifact", "link", artifact_id, p2ref, "--relationship", "derives-from", "--json" }));

    // Confirm at least one edge surfaces via links list on the task.
    const links = suite.mustRun(&.{ "links", "list", std.fmt.allocPrint(arena, "task:{d}", .{task.id}) catch unreachable });
    defer gpa.free(links);
    try std.testing.expect(std.mem.containsAtLeast(u8, links, 1, "blocks"));
}

test "scenario: editor flows config/plan/question/scenario/decision/workbench edit" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("editor-flows");
    const wb_root = std.fmt.allocPrint(arena, "{s}/wb", .{suite.tmpAbsPath()}) catch unreachable;
    const cfg_path = std.fmt.allocPrint(arena, "{s}/config.toml", .{suite.tmpAbsPath()}) catch unreachable;

    // No-op editor: opens the file and exits 0 without modifying it.
    const env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_EDITOR", .value = "/usr/bin/true" },
        .{ .key = "PLANAR_WORKBENCH_ROOT", .value = wb_root },
        .{ .key = "PLANAR_CONFIG_PATH", .value = cfg_path },
    };

    const plan = suite.mustRunJSON(Id, arena, &.{ "plan", "create", "--json", "Editable plan" });
    const pid = idStr(arena, plan.id);
    const question = suite.mustRunJSON(Id, arena, &.{ "question", "add", "--json", "--plan", pid, "Editable question" });
    const decision = suite.mustRunJSON(Id, arena, &.{ "decision", "add", "--json", "--plan", pid, "--body", "x", "Editable decision" });
    const scenario = suite.mustRunJSON(Id, arena, &.{ "scenario", "add", "--json", "--plan", pid, "Editable scenario" });

    // config edit: seed the file first, then open it under the no-op editor.
    expectOk(&suite, gpa, &.{ "config", "init" }, &env);
    expectOk(&suite, gpa, &.{ "config", "edit" }, &env);

    // entity edit flows (literal verb args so the leaf-coverage script counts them).
    expectOk(&suite, gpa, &.{ "plan", "edit", pid }, &env);
    expectOk(&suite, gpa, &.{ "question", "edit", idStr(arena, question.id) }, &env);
    expectOk(&suite, gpa, &.{ "decision", "edit", idStr(arena, decision.id) }, &env);
    expectOk(&suite, gpa, &.{ "scenario", "edit", idStr(arena, scenario.id) }, &env);

    // workbench edit: seed the tree, then open a rendered file.
    expectOk(&suite, gpa, &.{ "workbench", "push", pid }, &env);
    expectOk(&suite, gpa, &.{ "workbench", "edit", pid }, &env);
}

const BulkResult = struct { ok: bool, count: i64 };

test "scenario: annotate bulk-dismiss + verify" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("annotate-bulk");

    // Two anchored annotations to sweep.
    gpa.free(suite.mustRun(&.{ "annotate", "add", "--text", "review note A", "--anchor-path", "src/a.zig", "--json" }));
    gpa.free(suite.mustRun(&.{ "annotate", "add", "--text", "review note B", "--anchor-path", "src/b.zig", "--json" }));

    // bulk-dismiss dismisses every active annotation matching the filter.
    const bulk = suite.mustRunJSON(BulkResult, arena, &.{ "annotate", "bulk-dismiss", "--json" });
    try std.testing.expect(bulk.ok);
    try std.testing.expectEqual(@as(i64, 2), bulk.count);

    // verify re-checks anchors against the workspace; exits 0 with a rows list.
    gpa.free(suite.mustRun(&.{ "annotate", "verify", "--json" }));
}

test "scenario: capture note command and file persist in the session timeline" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const session = suite.mustRunJSON(SessionOpen, arena, &.{ "capture", "session", "--json" });
    const sid = idStr(arena, session.id);

    const note = suite.mustRunJSON(CaptureResult, arena, &.{ "capture", "note", "reviewed migration ordering", "--session", sid, "--json" });
    const command = suite.mustRunJSON(CaptureResult, arena, &.{ "capture", "command", "zig build test", "--outcome", "passed", "--session", sid, "--json" });
    const file = suite.mustRunJSON(CaptureResult, arena, &.{ "capture", "file", "src/db/migrate.zig", "--role", "implementation target", "--session", sid, "--json" });
    try std.testing.expect(note.ok and command.ok and file.ok);
    try std.testing.expectEqual(session.id, note.session_id);
    try std.testing.expectEqual(session.id, command.session_id);
    try std.testing.expectEqual(session.id, file.session_id);

    const timeline = suite.mustRunJSON(Timeline, arena, &.{ "audit", "session", sid, "--json" });
    try std.testing.expectEqual(@as(usize, 4), timeline.entries.len);
    try std.testing.expectEqualStrings("action", timeline.entries[0].prefix);
    try std.testing.expectEqualStrings("session opened", timeline.entries[0].body);
    try std.testing.expectEqualStrings("note", timeline.entries[1].prefix);
    try std.testing.expectEqualStrings("reviewed migration ordering", timeline.entries[1].body);
    try std.testing.expectEqualStrings("command", timeline.entries[2].prefix);
    try std.testing.expectEqualStrings("zig build test\noutcome: passed", timeline.entries[2].body);
    try std.testing.expectEqualStrings("file", timeline.entries[3].prefix);
    try std.testing.expectEqualStrings("src/db/migrate.zig [implementation target]", timeline.entries[3].body);
}

test "stub contracts fail loudly for audit publish-decision and workbench publish" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const plan = suite.mustRunJSON(Id, arena, &.{ "plan", "create", "--json", "Publish stub plan" });
    const pid = idStr(arena, plan.id);
    const decision = suite.mustRunJSON(Id, arena, &.{ "decision", "add", "--json", "--plan", pid, "--body", "recorded rationale", "Publish stub decision" });
    const did = idStr(arena, decision.id);

    const audit_res = suite.execWith(&.{ "audit", "publish-decision", did, "--json" }, &.{});
    defer audit_res.deinit(gpa);
    try std.testing.expect(audit_res.term == .exited and audit_res.term.exited != 0);
    try std.testing.expect(std.mem.containsAtLeast(u8, audit_res.stderr, 1, "not available in this build"));

    const publish_res = suite.execWith(&.{ "workbench", "publish", pid, "--system", "missing", "--json" }, &.{});
    defer publish_res.deinit(gpa);
    try std.testing.expect(publish_res.term == .exited and publish_res.term.exited != 0);
    try std.testing.expect(std.mem.containsAtLeast(u8, publish_res.stderr, 1, "not implemented yet"));
}
