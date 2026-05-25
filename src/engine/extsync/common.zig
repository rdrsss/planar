//! engine/extsync - external-system adapter boundary and shared types.
//!
//! The Go side uses interfaces for adapter dispatch. In Zig we keep the
//! boundary as comptime duck-typing: adapters export `pull`, `push`,
//! `validate`, and `render`; `dispatch` checks those declarations at
//! compile time.

const std = @import("std");

pub const AuthKind = enum { bearer, basic };

pub const AuthCredential = struct {
    kind: AuthKind,
    token: ?[]const u8 = null,
    user: ?[]const u8 = null,
    pass: ?[]const u8 = null,
};

pub const RemoteState = struct {
    external_id: []const u8,
    title: []const u8,
    body: []const u8,
    status: []const u8,
    assignee: []const u8,
    priority: i64,
    due_at: []const u8,
    url: []const u8,
    raw_status: []const u8,
};

pub fn deinitRemoteState(state: RemoteState, allocator: std.mem.Allocator) void {
    allocator.free(state.external_id);
    allocator.free(state.title);
    allocator.free(state.body);
    allocator.free(state.status);
    allocator.free(state.assignee);
    allocator.free(state.due_at);
    allocator.free(state.url);
    allocator.free(state.raw_status);
}

pub const LocalEntity = struct {
    kind: []const u8,
    id: i64,
    title: []const u8,
    body: []const u8,
    status: []const u8,
    priority: i64 = 0,
    due_at: []const u8 = "",
};

pub const CreateOptions = struct {
    issue_type: ?[]const u8 = null,
    project: ?[]const u8 = null,
};

pub const CreatedEntity = struct {
    external_id: []const u8,
    external_url: []const u8,
};

pub fn deinitCreatedEntity(created: CreatedEntity, allocator: std.mem.Allocator) void {
    allocator.free(created.external_id);
    allocator.free(created.external_url);
}

pub const FieldChangeSet = struct {
    title: ?[]const u8 = null,
    status: ?[]const u8 = null,
    assignee: ?[]const u8 = null,
    priority: ?[]const u8 = null,
};

pub const UpdateOutcome = struct {
    fields_applied: []const []const u8 = &.{},
};

pub fn deinitUpdateOutcome(outcome: UpdateOutcome, allocator: std.mem.Allocator) void {
    if (outcome.fields_applied.len > 0) allocator.free(outcome.fields_applied);
}

pub const Method = enum {
    get,
    post,
    put,
    patch,

    pub fn toText(self: Method) []const u8 {
        return switch (self) {
            .get => "GET",
            .post => "POST",
            .put => "PUT",
            .patch => "PATCH",
        };
    }
};

pub const Header = struct {
    name: []const u8,
    value: []const u8,
};

pub const Request = struct {
    method: Method,
    url: []const u8,
    headers: []const Header = &.{},
    body: ?[]const u8 = null,
};

pub const Response = struct {
    status: u16,
    body: []const u8,

    pub fn deinit(self: Response, allocator: std.mem.Allocator) void {
        allocator.free(self.body);
    }
};

pub const Transport = struct {
    ctx: *anyopaque,
    sendFn: *const fn (ctx: *anyopaque, allocator: std.mem.Allocator, req: Request) anyerror!Response,

    pub fn send(self: Transport, allocator: std.mem.Allocator, req: Request) anyerror!Response {
        return self.sendFn(self.ctx, allocator, req);
    }
};

pub const Operation = enum {
    pull,
    push,
    validate,
    render,
};

pub const PullArgs = struct {
    allocator: std.mem.Allocator,
    external_id: []const u8,
};

pub const PushArgs = struct {
    allocator: std.mem.Allocator,
    external_id: []const u8,
    fields: FieldChangeSet,
};

pub const ValidateArgs = struct {
    external_id: []const u8,
};

pub const RenderArgs = struct {
    allocator: std.mem.Allocator,
    local: LocalEntity,
    opts: CreateOptions,
};

pub fn operationResultType(comptime op: Operation) type {
    return switch (op) {
        .pull => RemoteState,
        .push => UpdateOutcome,
        .validate => void,
        .render => []const u8,
    };
}

pub fn dispatch(comptime Adapter: type, comptime op: Operation, adapter: *Adapter, args: anytype) !operationResultType(op) {
    comptime requireAdapter(Adapter);
    return switch (op) {
        .pull => try adapter.pull(args.allocator, args.external_id),
        .push => try adapter.push(args.allocator, args.external_id, args.fields),
        .validate => try adapter.validate(args.external_id),
        .render => try adapter.render(args.allocator, args.local, args.opts),
    };
}

fn requireAdapter(comptime Adapter: type) void {
    if (!@hasDecl(Adapter, "pull")) {
        @compileError("extsync adapter missing `pull`");
    }
    if (!@hasDecl(Adapter, "push")) {
        @compileError("extsync adapter missing `push`");
    }
    if (!@hasDecl(Adapter, "validate")) {
        @compileError("extsync adapter missing `validate`");
    }
    if (!@hasDecl(Adapter, "render")) {
        @compileError("extsync adapter missing `render`");
    }
}

pub fn jsonStringifyAlloc(allocator: std.mem.Allocator, value: anytype) ![]const u8 {
    var out: std.Io.Writer.Allocating = .init(allocator);
    defer out.deinit();
    try std.json.Stringify.value(value, .{}, &out.writer);
    return try allocator.dupe(u8, out.written());
}

pub fn urlEncodeQuery(allocator: std.mem.Allocator, s: []const u8) ![]const u8 {
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(allocator);
    for (s) |c| {
        if ((c >= 'A' and c <= 'Z') or (c >= 'a' and c <= 'z') or (c >= '0' and c <= '9') or c == '-' or c == '_' or c == '.' or c == '~') {
            try buf.append(allocator, c);
            continue;
        }
        if (c == ' ') {
            try buf.appendSlice(allocator, "%20");
            continue;
        }
        const hex = [_]u8{
            "0123456789ABCDEF"[c >> 4],
            "0123456789ABCDEF"[c & 0x0F],
        };
        try buf.append(allocator, '%');
        try buf.appendSlice(allocator, &hex);
    }
    return try buf.toOwnedSlice(allocator);
}

test "dispatch checks adapter declarations and routes by operation" {
    const Fake = struct {
        pub fn pull(_: *const @This(), allocator: std.mem.Allocator, external_id: []const u8) !RemoteState {
            return .{
                .external_id = try allocator.dupe(u8, external_id),
                .title = try allocator.dupe(u8, "t"),
                .body = try allocator.dupe(u8, ""),
                .status = try allocator.dupe(u8, "todo"),
                .assignee = try allocator.dupe(u8, ""),
                .priority = 0,
                .due_at = try allocator.dupe(u8, ""),
                .url = try allocator.dupe(u8, ""),
                .raw_status = try allocator.dupe(u8, "To Do"),
            };
        }
        pub fn push(_: *const @This(), allocator: std.mem.Allocator, _: []const u8, _: FieldChangeSet) !UpdateOutcome {
            var fields: std.ArrayList([]const u8) = .empty;
            defer fields.deinit(allocator);
            try fields.append(allocator, "title");
            return .{ .fields_applied = try fields.toOwnedSlice(allocator) };
        }
        pub fn validate(_: *const @This(), external_id: []const u8) !void {
            if (external_id.len == 0) return error.InvalidInput;
        }
        pub fn render(_: *const @This(), allocator: std.mem.Allocator, _: LocalEntity, _: CreateOptions) ![]const u8 {
            return try allocator.dupe(u8, "{\"ok\":true}");
        }
    };

    const a = std.testing.allocator;
    var adapter: Fake = .{};

    const state = try dispatch(Fake, .pull, &adapter, PullArgs{ .allocator = a, .external_id = "X-1" });
    defer deinitRemoteState(state, a);
    try std.testing.expectEqualStrings("X-1", state.external_id);

    const pushed = try dispatch(Fake, .push, &adapter, PushArgs{
        .allocator = a,
        .external_id = "X-1",
        .fields = .{ .title = "new title" },
    });
    defer deinitUpdateOutcome(pushed, a);
    try std.testing.expectEqual(@as(usize, 1), pushed.fields_applied.len);

    try dispatch(Fake, .validate, &adapter, ValidateArgs{ .external_id = "X-1" });

    const rendered = try dispatch(Fake, .render, &adapter, RenderArgs{
        .allocator = a,
        .local = .{ .kind = "task", .id = 1, .title = "t", .body = "", .status = "todo" },
        .opts = .{},
    });
    defer a.free(rendered);
    try std.testing.expectEqualStrings("{\"ok\":true}", rendered);
}

test "urlEncodeQuery uses percent encoding and %20 for space" {
    const a = std.testing.allocator;
    const encoded = try urlEncodeQuery(a, "repo:acme/api is:open");
    defer a.free(encoded);
    try std.testing.expectEqualStrings("repo%3Aacme%2Fapi%20is%3Aopen", encoded);
}
