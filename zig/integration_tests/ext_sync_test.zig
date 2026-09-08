//! integration_tests/ext_sync_test.zig
//!
//! TASK 6548 (decisions 1035/1036): the `ext register`/`ext list`/`ext test`
//! smoke test and the two `sync resolve` tests this file used to hold are
//! deleted -- covered by src/cmd/planar-ext/{ext_leaves,ext_factory,
//! sync_leaves}.t.cpp and src/lib/engine/external/sync.t.cpp. What remains
//! is `runSyncConflictEvidenceAndResolutionLifecycle` (and its
//! `FakeSyncJira` fixture / `expectResolveRefusedWithoutMutation` helper),
//! which is NOT redundant: it is called directly by
//! `scenarios/scenario_agent_skill_lifecycle_test.zig`, one of task 6547's
//! irreplaceable cross-process cases. Do not delete this file or its `pub
//! fn` while that caller still imports it.

const std = @import("std");
const harness = @import("harness");

const RegisterJSON = struct {
    id: i64,
    slug: []const u8,
    kind: []const u8,
};

const TestJSON = struct {
    slug: []const u8,
    ok: bool,
};

const FakeSyncJira = struct {
    gpa: std.mem.Allocator,
    io: std.Io,
    server: std.Io.net.Server,
    port: u16,
    thread: std.Thread,
    stop_flag: std.atomic.Value(u8),
    get_count: std.atomic.Value(usize),
    put_count: std.atomic.Value(usize),
    remote_race: std.atomic.Value(u8),

    fn init(gpa: std.mem.Allocator, io: std.Io) !*FakeSyncJira {
        const self = try gpa.create(FakeSyncJira);
        self.* = .{ .gpa = gpa, .io = io, .server = undefined, .port = 0, .thread = undefined, .stop_flag = .init(0), .get_count = .init(0), .put_count = .init(0), .remote_race = .init(0) };
        var addr = std.Io.net.IpAddress{ .ip4 = std.Io.net.Ip4Address.loopback(0) };
        self.server = try std.Io.net.IpAddress.listen(&addr, io, .{});
        self.port = self.server.socket.address.getPort();
        self.thread = try std.Thread.spawn(.{}, runLoop, .{self});
        return self;
    }

    fn deinit(self: *FakeSyncJira) void {
        self.stop_flag.store(1, .release);
        self.server.deinit(self.io);
        self.thread.join();
        self.gpa.destroy(self);
    }

    fn runLoop(self: *FakeSyncJira) void {
        while (self.stop_flag.load(.acquire) == 0) {
            const stream = self.server.accept(self.io) catch break;
            self.handleOne(stream) catch {};
        }
    }

    fn handleOne(self: *FakeSyncJira, stream: std.Io.net.Stream) !void {
        defer stream.close(self.io);
        var in_buf: [8192]u8 = undefined;
        var out_buf: [4096]u8 = undefined;
        var rdr = stream.reader(self.io, &in_buf);
        var wtr = stream.writer(self.io, &out_buf);
        var hs = std.http.Server.init(&rdr.interface, &wtr.interface);
        var req = hs.receiveHead() catch return;
        if (req.head.method == .GET) {
            _ = self.get_count.fetchAdd(1, .acq_rel);
            const mode = self.remote_race.load(.acquire);
            if (mode == 2) {
                try req.respond("unavailable", .{ .status = .service_unavailable, .extra_headers = &.{.{ .name = "Connection", .value = "close" }} });
                return;
            }
            // Mode 1 deliberately changes only the provider version while
            // returning the same field values. This catches changed-then-
            // returned remote state that value-only comparison would miss.
            const title = if (mode == 0) "Baseline" else "Remote edit";
            const status = if (mode == 0) "To Do" else "In Progress";
            var body_buf: [512]u8 = undefined;
            const version = if (mode == 1) "2026-07-13T13:00:01Z" else if (mode == 4) "" else "2026-07-13T13:00:00Z";
            const body = try std.fmt.bufPrint(&body_buf, "{{\"key\":\"SYNC-1\",\"fields\":{{\"summary\":\"{s}\",\"updated\":\"{s}\",\"status\":{{\"name\":\"{s}\"}},\"description\":null,\"assignee\":null,\"priority\":null,\"duedate\":null}}}}", .{ title, version, status });
            try req.respond(body, .{ .status = .ok, .extra_headers = &.{ .{ .name = "Content-Type", .value = "application/json" }, .{ .name = "Connection", .value = "close" } } });
        } else if (req.head.method == .PUT) {
            _ = self.put_count.fetchAdd(1, .acq_rel);
            try req.respond("", .{ .status = .no_content, .extra_headers = &.{.{ .name = "Connection", .value = "close" }} });
        } else {
            try req.respond("", .{ .status = .not_found, .extra_headers = &.{.{ .name = "Connection", .value = "close" }} });
        }
    }
};

fn expectResolveRefusedWithoutMutation(
    suite: *harness.Suite,
    gpa: std.mem.Allocator,
    task_id: []const u8,
    task_ref: []const u8,
    link_id: []const u8,
    args: []const []const u8,
    env: []const harness.Suite.ExtraEnvEntry,
    put_count: *std.atomic.Value(usize),
) !void {
    const task_before = suite.mustRun(&.{ "task", "show", task_id, "--json" });
    defer gpa.free(task_before);
    const status_before = suite.mustRunExt(&.{ "sync", "status", "--entity", task_ref, "--json" });
    defer gpa.free(status_before);
    const audit_before = suite.mustRun(&.{ "audit", "trail", "--link", link_id, "--json" });
    defer gpa.free(audit_before);
    const puts_before = put_count.load(.acquire);

    // `args` is always a `sync resolve ...` invocation (see call sites
    // below), so this routes to ext_bin like the rest of the sync surface.
    const refused = suite.execExtWith(args, env);
    defer refused.deinit(gpa);
    try std.testing.expect(refused.term.exited != 0);
    try std.testing.expectEqual(puts_before, put_count.load(.acquire));

    const task_after = suite.mustRun(&.{ "task", "show", task_id, "--json" });
    defer gpa.free(task_after);
    const status_after = suite.mustRunExt(&.{ "sync", "status", "--entity", task_ref, "--json" });
    defer gpa.free(status_after);
    const audit_after = suite.mustRun(&.{ "audit", "trail", "--link", link_id, "--json" });
    defer gpa.free(audit_after);
    try std.testing.expectEqualStrings(task_before, task_after);
    try std.testing.expectEqualStrings(status_before, status_after);
    try std.testing.expectEqualStrings(audit_before, audit_after);
}

pub fn runSyncConflictEvidenceAndResolutionLifecycle() !void {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();
    const server = try FakeSyncJira.init(gpa, std.testing.io);
    defer server.deinit();
    const base_url = try std.fmt.allocPrint(arena, "http://127.0.0.1:{d}", .{server.port});
    const env: []const harness.Suite.ExtraEnvEntry = &.{.{ .key = "PLANAR_SYNC_TOKEN", .value = "test-token" }};

    const task = suite.mustRunJSON(struct { id: i64 }, arena, &.{ "task", "add", "--json", "Baseline" });
    const task_id = try std.fmt.allocPrint(arena, "{d}", .{task.id});
    const task_ref = try std.fmt.allocPrint(arena, "task:{d}", .{task.id});
    _ = suite.mustRunExtJSON(RegisterJSON, arena, &.{ "ext", "register", "jira", "sync-jira", "--base-url", base_url, "--project", "SYNC", "--auth-env", "PLANAR_SYNC_TOKEN", "--json" });
    const linked_raw = suite.mustRun(&.{ "link", task_ref, "--to", "sync-jira:SYNC-1", "--role", "mirror", "--sync", "two-way", "--json" });
    defer gpa.free(linked_raw);
    var linked = try std.json.parseFromSlice(struct { link_id: i64 }, arena, linked_raw, .{ .ignore_unknown_fields = true });
    defer linked.deinit();
    const link_id = try std.fmt.allocPrint(arena, "{d}", .{linked.value.link_id});

    gpa.free(suite.mustRunExtWith(&.{ "sync", "pull", task_ref, "--json" }, env));
    try std.testing.expectEqual(@as(usize, 0), server.put_count.load(.acquire));
    gpa.free(suite.mustRun(&.{ "task", "update", task_id, "--title", "Local edit", "--json" }));
    server.remote_race.store(3, .release);
    const conflict_run = suite.execExtWith(&.{ "sync", "pull", task_ref, "--json" }, env);
    defer conflict_run.deinit(gpa);
    try std.testing.expectEqual(@as(u8, 3), conflict_run.term.exited);

    const trail_raw = suite.mustRun(&.{ "audit", "trail", "--link", link_id, "--json" });
    defer gpa.free(trail_raw);
    var trail = try std.json.parseFromSlice(std.json.Value, arena, trail_raw, .{});
    defer trail.deinit();
    const events = trail.value.object.get("sync_events").?.array.items;
    const conflict = events[events.len - 1].object;
    const event_id = conflict.get("id").?.integer;
    const evidence = conflict.get("evidence").?.object;
    try std.testing.expectEqualStrings("Local edit", evidence.get("local").?.object.get("title").?.string);
    try std.testing.expectEqualStrings("Remote edit", evidence.get("remote").?.object.get("title").?.string);
    try std.testing.expect(evidence.get("observed_at").?.string.len > 0);
    const token = evidence.get("token").?.string;
    const local_version = evidence.get("local").?.object.get("updated_at").?.string;
    const event_arg = try std.fmt.allocPrint(arena, "{d}", .{event_id});

    // The manual-merge proposal alone is not a second gate: required CAS
    // evidence is absent, so link, entity, audit log, and remote stay unchanged.
    try expectResolveRefusedWithoutMutation(&suite, gpa, task_id, task_ref, link_id, &.{ "sync", "resolve", event_arg, "--keep", "local", "--json" }, env, &server.put_count);

    // A local edit invalidates the approved local version. This is also the
    // manual-merge boundary: the original approval cannot authorize a push.
    gpa.free(suite.mustRun(&.{ "task", "update", task_id, "--title", "Reviewed manual merge", "--json" }));
    try expectResolveRefusedWithoutMutation(&suite, gpa, task_id, task_ref, link_id, &.{ "sync", "resolve", event_arg, "--keep", "local", "--evidence-token", token, "--expected-local-updated-at", local_version, "--json" }, env, &server.put_count);

    const task_show_raw = suite.mustRun(&.{ "task", "show", task_id, "--json" });
    defer gpa.free(task_show_raw);
    var task_show = try std.json.parseFromSlice(struct { updated_at: []const u8 }, arena, task_show_raw, .{ .ignore_unknown_fields = true });
    defer task_show.deinit();
    const reviewed_local_version = task_show.value.updated_at;

    // Even with the reviewed local version, a changed fresh remote read
    // invalidates the evidence and changes no observable state.
    server.remote_race.store(1, .release);
    try expectResolveRefusedWithoutMutation(&suite, gpa, task_id, task_ref, link_id, &.{ "sync", "resolve", event_arg, "--keep", "local", "--evidence-token", token, "--expected-local-updated-at", reviewed_local_version, "--json" }, env, &server.put_count);
    server.remote_race.store(3, .release);

    // A provider response without a fresh version cannot authorize either
    // side, even when its field values still match the approved evidence.
    server.remote_race.store(4, .release);
    try expectResolveRefusedWithoutMutation(&suite, gpa, task_id, task_ref, link_id, &.{ "sync", "resolve", event_arg, "--keep", "remote", "--evidence-token", token, "--expected-local-updated-at", reviewed_local_version, "--json" }, env, &server.put_count);
    server.remote_race.store(3, .release);

    // Adapter unavailability defers safely: no resolution event or PUT.
    server.remote_race.store(2, .release);
    try expectResolveRefusedWithoutMutation(&suite, gpa, task_id, task_ref, link_id, &.{ "sync", "resolve", event_arg, "--keep", "local", "--evidence-token", token, "--expected-local-updated-at", reviewed_local_version, "--json" }, env, &server.put_count);
    server.remote_race.store(3, .release);

    // A fresh pull records a new latest conflict. The former event is now
    // stale and cannot be resolved even with otherwise-current inputs.
    const second_conflict_run = suite.execExtWith(&.{ "sync", "pull", task_ref, "--json" }, env);
    defer second_conflict_run.deinit(gpa);
    try std.testing.expectEqual(@as(u8, 3), second_conflict_run.term.exited);
    try expectResolveRefusedWithoutMutation(&suite, gpa, task_id, task_ref, link_id, &.{ "sync", "resolve", event_arg, "--keep", "local", "--evidence-token", token, "--expected-local-updated-at", reviewed_local_version, "--json" }, env, &server.put_count);

    const fresh_trail_raw = suite.mustRun(&.{ "audit", "trail", "--link", link_id, "--json" });
    defer gpa.free(fresh_trail_raw);
    var fresh_trail = try std.json.parseFromSlice(std.json.Value, arena, fresh_trail_raw, .{});
    defer fresh_trail.deinit();
    const fresh_events = fresh_trail.value.object.get("sync_events").?.array.items;
    const fresh_conflict = fresh_events[fresh_events.len - 1].object;
    const fresh_event_id = fresh_conflict.get("id").?.integer;
    const fresh_evidence = fresh_conflict.get("evidence").?.object;
    const fresh_token = fresh_evidence.get("token").?.string;
    const fresh_local_version = fresh_evidence.get("local").?.object.get("updated_at").?.string;
    const fresh_event_arg = try std.fmt.allocPrint(arena, "{d}", .{fresh_event_id});

    const resolved_raw = suite.mustRunExtWith(&.{ "sync", "resolve", fresh_event_arg, "--keep", "local", "--evidence-token", fresh_token, "--expected-local-updated-at", fresh_local_version, "--json" }, env);
    defer gpa.free(resolved_raw);
    const resolved = try std.json.parseFromSlice(struct { ok: bool, event_id: i64, new_event_id: i64 }, arena, resolved_raw, .{ .ignore_unknown_fields = true });
    try std.testing.expect(resolved.value.ok);
    try std.testing.expectEqual(fresh_event_id, resolved.value.event_id);
    try std.testing.expect(resolved.value.new_event_id > fresh_event_id);
    try std.testing.expectEqual(@as(usize, 1), server.put_count.load(.acquire));

    const status_raw = suite.mustRunExt(&.{ "sync", "status", "--entity", task_ref, "--json" });
    defer gpa.free(status_raw);
    try std.testing.expect(std.mem.indexOf(u8, status_raw, "\"last_sync_status\":\"ok\"") != null);

    const resolved_trail_raw = suite.mustRun(&.{ "audit", "trail", "--link", link_id, "--json" });
    defer gpa.free(resolved_trail_raw);
    var resolved_trail = try std.json.parseFromSlice(std.json.Value, arena, resolved_trail_raw, .{});
    defer resolved_trail.deinit();
    const resolved_events = resolved_trail.value.object.get("sync_events").?.array.items;
    const resolution_event = resolved_events[resolved_events.len - 1].object;
    try std.testing.expectEqual(resolved.value.new_event_id, resolution_event.get("id").?.integer);
    try std.testing.expectEqualStrings("push", resolution_event.get("direction").?.string);
    try std.testing.expectEqualStrings("ok", resolution_event.get("outcome").?.string);
    const expected_detail = try std.fmt.allocPrint(arena, "resolved=local; from sync_event={d}", .{fresh_event_id});
    try std.testing.expectEqualStrings(expected_detail, resolution_event.get("detail").?.string);

    const resolved_task_raw = suite.mustRun(&.{ "task", "show", task_id, "--json" });
    defer gpa.free(resolved_task_raw);
    var resolved_task = try std.json.parseFromSlice(struct { title: []const u8, status: []const u8 }, arena, resolved_task_raw, .{ .ignore_unknown_fields = true });
    defer resolved_task.deinit();
    try std.testing.expectEqualStrings("Reviewed manual merge", resolved_task.value.title);
    try std.testing.expectEqualStrings("todo", resolved_task.value.status);
}

