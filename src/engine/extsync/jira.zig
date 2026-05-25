//! engine/extsync/jira - Jira operational-plane adapter.

const std = @import("std");
const extsync = @import("common.zig");

pub const Error = error{
    InvalidExternalId,
    NotFound,
    UnexpectedStatus,
    TransportFailed,
    ParseFailed,
    EncodeFailed,
    InvalidAuth,
    WriteFailed,
} || std.mem.Allocator.Error;

pub const JiraAdapter = struct {
    base_url: []const u8,
    cred: extsync.AuthCredential,
    transport: extsync.Transport,

    pub fn init(base_url: []const u8, cred: extsync.AuthCredential, transport: extsync.Transport) JiraAdapter {
        return .{
            .base_url = trimTrailingSlash(base_url),
            .cred = cred,
            .transport = transport,
        };
    }

    pub fn validate(_: *const JiraAdapter, external_id: []const u8) Error!void {
        if (!isValidIssueKey(external_id)) return Error.InvalidExternalId;
    }

    pub fn pull(self: *const JiraAdapter, allocator: std.mem.Allocator, external_id: []const u8) Error!extsync.RemoteState {
        try self.validate(external_id);

        const url = try std.fmt.allocPrint(allocator, "{s}/rest/api/3/issue/{s}", .{ self.base_url, external_id });
        defer allocator.free(url);

        const auth = try authHeader(allocator, self.cred);
        defer allocator.free(auth);
        const headers = [_]extsync.Header{
            .{ .name = "Accept", .value = "application/json" },
            .{ .name = "Authorization", .value = auth },
        };
        const response = self.transport.send(allocator, .{
            .method = .get,
            .url = url,
            .headers = &headers,
        }) catch return Error.TransportFailed;
        defer response.deinit(allocator);

        if (response.status == 404) return Error.NotFound;
        if (response.status != 200) return Error.UnexpectedStatus;
        return try parseIssue(allocator, external_id, response.body);
    }

    pub fn push(self: *const JiraAdapter, allocator: std.mem.Allocator, external_id: []const u8, fields: extsync.FieldChangeSet) Error!extsync.UpdateOutcome {
        try self.validate(external_id);

        var applied: std.ArrayList([]const u8) = .empty;
        defer applied.deinit(allocator);

        var body: std.Io.Writer.Allocating = .init(allocator);
        defer body.deinit();
        const writer = &body.writer;
        try writer.writeAll("{\"fields\":{");
        var has_field = false;

        if (fields.title) |title| {
            has_field = true;
            try writer.writeAll("\"summary\":");
            try writeJSONString(writer, title);
            try applied.append(allocator, "title");
        }
        if (fields.assignee) |assignee| {
            if (has_field) try writer.writeAll(",");
            has_field = true;
            try writer.writeAll("\"assignee\":{\"accountId\":");
            try writeJSONString(writer, assignee);
            try writer.writeAll("}");
            try applied.append(allocator, "assignee");
        }
        if (fields.priority) |priority| {
            if (has_field) try writer.writeAll(",");
            has_field = true;
            try writer.writeAll("\"priority\":{\"name\":");
            try writeJSONString(writer, priority);
            try writer.writeAll("}");
            try applied.append(allocator, "priority");
        }
        // Jira status transitions require /transitions; direct issue updates skip it.
        _ = fields.status;

        try writer.writeAll("}}");

        if (!has_field) return .{};

        const url = try std.fmt.allocPrint(allocator, "{s}/rest/api/3/issue/{s}", .{ self.base_url, external_id });
        defer allocator.free(url);

        const auth = try authHeader(allocator, self.cred);
        defer allocator.free(auth);
        const payload = try allocator.dupe(u8, body.written());
        defer allocator.free(payload);

        const headers = [_]extsync.Header{
            .{ .name = "Content-Type", .value = "application/json" },
            .{ .name = "Authorization", .value = auth },
        };
        const response = self.transport.send(allocator, .{
            .method = .put,
            .url = url,
            .headers = &headers,
            .body = payload,
        }) catch return Error.TransportFailed;
        defer response.deinit(allocator);
        if (response.status != 204) return Error.UnexpectedStatus;

        return .{ .fields_applied = try applied.toOwnedSlice(allocator) };
    }

    pub fn render(_: *const JiraAdapter, allocator: std.mem.Allocator, local: extsync.LocalEntity, opts: extsync.CreateOptions) Error![]const u8 {
        const issue_type = opts.issue_type orelse "Story";
        const description = if (local.body.len == 0) "(no description)" else local.body;

        var out: std.Io.Writer.Allocating = .init(allocator);
        defer out.deinit();
        const writer = &out.writer;
        try writer.writeAll("{\"fields\":{");
        if (opts.project) |project| {
            try writer.writeAll("\"project\":{\"key\":");
            try writeJSONString(writer, project);
            try writer.writeAll("},");
        }
        try writer.writeAll("\"summary\":");
        try writeJSONString(writer, local.title);
        try writer.writeAll(",\"issuetype\":{\"name\":");
        try writeJSONString(writer, issue_type);
        try writer.writeAll("},\"description\":{\"type\":\"doc\",\"version\":1,\"content\":[{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":");
        try writeJSONString(writer, description);
        try writer.writeAll("}]}]}}}");
        return try allocator.dupe(u8, out.written());
    }
};

fn trimTrailingSlash(s: []const u8) []const u8 {
    if (s.len == 0) return s;
    if (s[s.len - 1] == '/') return s[0 .. s.len - 1];
    return s;
}

fn authHeader(allocator: std.mem.Allocator, cred: extsync.AuthCredential) Error![]const u8 {
    return switch (cred.kind) {
        .bearer => blk: {
            const token = cred.token orelse return Error.InvalidAuth;
            break :blk try std.fmt.allocPrint(allocator, "Bearer {s}", .{token});
        },
        .basic => blk: {
            const user = cred.user orelse return Error.InvalidAuth;
            const pass = cred.pass orelse return Error.InvalidAuth;
            const joined = try std.fmt.allocPrint(allocator, "{s}:{s}", .{ user, pass });
            defer allocator.free(joined);
            const enc_len = std.base64.standard.Encoder.calcSize(joined.len);
            const encoded = try allocator.alloc(u8, enc_len);
            _ = std.base64.standard.Encoder.encode(encoded, joined);
            defer allocator.free(encoded);
            break :blk try std.fmt.allocPrint(allocator, "Basic {s}", .{encoded});
        },
    };
}

fn writeJSONString(writer: *std.Io.Writer, s: []const u8) !void {
    var jsw: std.json.Stringify = .{ .writer = writer, .options = .{} };
    try jsw.write(s);
}

fn isValidIssueKey(external_id: []const u8) bool {
    const dash = std.mem.indexOfScalar(u8, external_id, '-') orelse return false;
    if (dash == 0 or dash + 1 >= external_id.len) return false;
    for (external_id[0..dash]) |c| {
        const ok = (c >= 'A' and c <= 'Z') or (c >= '0' and c <= '9') or c == '_';
        if (!ok) return false;
    }
    for (external_id[dash + 1 ..]) |c| {
        if (c < '0' or c > '9') return false;
    }
    return true;
}

fn mapStatus(raw: []const u8) []const u8 {
    if (std.mem.eql(u8, raw, "To Do")) return "todo";
    if (std.mem.eql(u8, raw, "In Progress")) return "doing";
    if (std.mem.eql(u8, raw, "Blocked")) return "blocked";
    if (std.mem.eql(u8, raw, "Done")) return "done";
    if (std.mem.eql(u8, raw, "Cancelled")) return "cancelled";
    if (std.mem.eql(u8, raw, "Won't Do")) return "cancelled";
    return "";
}

fn parseIssue(allocator: std.mem.Allocator, fallback_external_id: []const u8, raw: []const u8) Error!extsync.RemoteState {
    var parsed = std.json.parseFromSlice(std.json.Value, allocator, raw, .{}) catch return Error.ParseFailed;
    defer parsed.deinit();

    const root = parsed.value;
    if (root != .object) return Error.ParseFailed;
    const key = getObjectString(root.object, "key") orelse fallback_external_id;
    const fields_v = root.object.get("fields") orelse return Error.ParseFailed;
    if (fields_v != .object) return Error.ParseFailed;
    const fields = fields_v.object;

    const summary = getObjectString(fields, "summary") orelse "";
    const status_name = blk: {
        const status_v = fields.get("status") orelse break :blk "";
        if (status_v != .object) break :blk "";
        break :blk getObjectString(status_v.object, "name") orelse "";
    };
    const description = fields.get("description");
    const body = try extractADFText(allocator, description);

    const assignee = blk: {
        const assignee_v = fields.get("assignee") orelse break :blk try allocator.dupe(u8, "");
        if (assignee_v != .object) break :blk try allocator.dupe(u8, "");
        if (getObjectString(assignee_v.object, "emailAddress")) |mail| {
            break :blk try allocator.dupe(u8, mail);
        }
        if (getObjectString(assignee_v.object, "accountId")) |account| {
            break :blk try allocator.dupe(u8, account);
        }
        break :blk try allocator.dupe(u8, "");
    };

    const priority = blk: {
        const pr_v = fields.get("priority") orelse break :blk @as(i64, 0);
        if (pr_v != .object) break :blk @as(i64, 0);
        const id_str = getObjectString(pr_v.object, "id") orelse break :blk @as(i64, 0);
        break :blk std.fmt.parseInt(i64, id_str, 10) catch 0;
    };
    const due_at = getObjectString(fields, "duedate") orelse "";

    return .{
        .external_id = try allocator.dupe(u8, key),
        .title = try allocator.dupe(u8, summary),
        .body = body,
        .status = try allocator.dupe(u8, mapStatus(status_name)),
        .assignee = assignee,
        .priority = priority,
        .due_at = try allocator.dupe(u8, due_at),
        .url = try allocator.dupe(u8, ""),
        .raw_status = try allocator.dupe(u8, status_name),
    };
}

fn extractADFText(allocator: std.mem.Allocator, description_opt: ?std.json.Value) Error![]const u8 {
    if (description_opt == null) return try allocator.dupe(u8, "");
    const description = description_opt.?;
    if (description != .object) return try allocator.dupe(u8, "");
    const content_v = description.object.get("content") orelse return try allocator.dupe(u8, "");
    if (content_v != .array) return try allocator.dupe(u8, "");

    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(allocator);
    var first_line = true;
    for (content_v.array.items) |block| {
        if (block != .object) continue;
        const inner = block.object.get("content") orelse continue;
        if (inner != .array) continue;
        if (!first_line) try out.append(allocator, '\n');
        first_line = false;
        for (inner.array.items) |inline_node| {
            if (inline_node != .object) continue;
            const text = getObjectString(inline_node.object, "text") orelse continue;
            try out.appendSlice(allocator, text);
        }
    }
    return try out.toOwnedSlice(allocator);
}

fn getObjectString(obj: std.json.ObjectMap, key: []const u8) ?[]const u8 {
    const value = obj.get(key) orelse return null;
    if (value != .string) return null;
    return value.string;
}

test "validate accepts canonical Jira issue keys" {
    const fake = extsync.Transport{ .ctx = undefined, .sendFn = unreachableSend };
    const adapter = JiraAdapter.init("https://acme.atlassian.net", .{ .kind = .bearer, .token = "t" }, fake);
    try adapter.validate("PROJ-123");
    try std.testing.expectError(Error.InvalidExternalId, adapter.validate("proj-123"));
    try std.testing.expectError(Error.InvalidExternalId, adapter.validate("PROJ"));
}

test "pull maps Jira response into RemoteState" {
    const a = std.testing.allocator;
    var t = FakeTransport.init(a, 200,
        \\{"key":"PROJ-1","fields":{"summary":"Test issue","description":{"type":"doc","version":1,"content":[{"type":"paragraph","content":[{"type":"text","text":"Issue body text"}]}]},"status":{"name":"In Progress"},"assignee":{"accountId":"acc","emailAddress":"dev@example.com"},"priority":{"id":"2","name":"High"},"duedate":"2026-06-30"}}
    );
    defer t.deinit();

    const adapter = JiraAdapter.init("https://acme.atlassian.net", .{ .kind = .bearer, .token = "tok" }, t.transport());
    const state = try adapter.pull(a, "PROJ-1");
    defer extsync.deinitRemoteState(state, a);

    try std.testing.expectEqualStrings("PROJ-1", state.external_id);
    try std.testing.expectEqualStrings("Test issue", state.title);
    try std.testing.expectEqualStrings("Issue body text", state.body);
    try std.testing.expectEqualStrings("doing", state.status);
    try std.testing.expectEqualStrings("dev@example.com", state.assignee);
    try std.testing.expectEqual(@as(i64, 2), state.priority);
}

test "push skips status-only update and sends title update" {
    const a = std.testing.allocator;
    var t = FakeTransport.init(a, 204, "");
    defer t.deinit();
    const adapter = JiraAdapter.init("https://acme.atlassian.net", .{ .kind = .bearer, .token = "tok" }, t.transport());

    const no_send = try adapter.push(a, "PROJ-1", .{ .status = "done" });
    defer extsync.deinitUpdateOutcome(no_send, a);
    try std.testing.expectEqual(@as(usize, 0), t.calls);

    const sent = try adapter.push(a, "PROJ-1", .{ .title = "Updated title" });
    defer extsync.deinitUpdateOutcome(sent, a);
    try std.testing.expectEqual(@as(usize, 1), t.calls);
    try std.testing.expect(t.last_body != null);
    try std.testing.expect(std.mem.indexOf(u8, t.last_body.?, "\"summary\":\"Updated title\"") != null);
}

test "render applies default issue type and description placeholder" {
    const a = std.testing.allocator;
    const fake = extsync.Transport{ .ctx = undefined, .sendFn = unreachableSend };
    const adapter = JiraAdapter.init("https://acme.atlassian.net", .{ .kind = .bearer, .token = "t" }, fake);

    const json = try adapter.render(a, .{
        .kind = "task",
        .id = 42,
        .title = "New task",
        .body = "",
        .status = "todo",
    }, .{});
    defer a.free(json);

    try std.testing.expect(std.mem.indexOf(u8, json, "\"name\":\"Story\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, json, "(no description)") != null);
}

const FakeTransport = struct {
    allocator: std.mem.Allocator,
    status: u16,
    body: []const u8,
    calls: usize = 0,
    last_method: ?extsync.Method = null,
    last_url: ?[]const u8 = null,
    last_body: ?[]const u8 = null,

    fn init(allocator: std.mem.Allocator, status: u16, body: []const u8) FakeTransport {
        return .{ .allocator = allocator, .status = status, .body = body };
    }

    fn deinit(self: *FakeTransport) void {
        if (self.last_url) |s| self.allocator.free(s);
        if (self.last_body) |s| self.allocator.free(s);
    }

    fn transport(self: *FakeTransport) extsync.Transport {
        return .{
            .ctx = self,
            .sendFn = send,
        };
    }

    fn send(ctx: *anyopaque, allocator: std.mem.Allocator, req: extsync.Request) anyerror!extsync.Response {
        const self: *FakeTransport = @ptrCast(@alignCast(ctx));
        self.calls += 1;
        self.last_method = req.method;
        if (self.last_url) |old| self.allocator.free(old);
        self.last_url = try self.allocator.dupe(u8, req.url);
        if (self.last_body) |old| self.allocator.free(old);
        self.last_body = if (req.body) |b| try self.allocator.dupe(u8, b) else null;
        return .{
            .status = self.status,
            .body = try allocator.dupe(u8, self.body),
        };
    }
};

fn unreachableSend(_: *anyopaque, _: std.mem.Allocator, _: extsync.Request) anyerror!extsync.Response {
    return error.UnreachableTransport;
}
