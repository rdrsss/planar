//! handlers/sync/common — shared helpers for sync pull/push/status/resolve.
//!
//! Adapter dispatch goes through engine.external.sync.{pullLink, pushLink,
//! resolveConflict}, which take an adapter via `anytype`. Since Jira and
//! GitHub adapters are distinct types, we branch on `handle.kind` here and
//! pass the matching field down.

const std = @import("std");
const engine = @import("engine");
const adapter_factory = @import("../ext/adapter_factory.zig");

pub const Op = enum { pull, push };

pub fn entityScopeSlug(d: anytype, allocator: std.mem.Allocator, link: engine.external.link.ExtLink) !?[]const u8 {
    if (link.entity_kind == .session) return null;
    const table: [:0]const u8 = switch (link.entity_kind) {
        .plan => "plans",
        .task => "tasks",
        .question => "questions",
        .test_scenario => "test_scenarios",
        .artifact => "artifacts",
        .decision => "decisions",
        .session => unreachable,
    };
    var sql_buf: [128]u8 = undefined;
    const sql = try std.fmt.bufPrintZ(&sql_buf, "select scope_kind, scope_id from {s} where id = ?", .{table});
    var stmt = try d.prepare(sql);
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = link.entity_id }});
    if ((try stmt.step()) != .row) return error.NotFound;
    const kind_text = try stmt.columnTextAlloc(0, allocator);
    defer allocator.free(kind_text);
    const kind: engine.identity.scope.ScopeKind = if (std.mem.eql(u8, kind_text, "global"))
        .global
    else if (std.mem.eql(u8, kind_text, "association"))
        .association
    else if (std.mem.eql(u8, kind_text, "repo"))
        .repo
    else
        return error.UnsupportedScope;
    return try engine.identity.scope.slugFromRef(d, allocator, kind, stmt.columnIntOpt(1));
}

/// A parsed `<kind>:<id>` external-entity reference (e.g. `task:42`). Uses
/// the narrower `ExternalEntityKind` (the kinds an external link can point
/// at), NOT the wider entity-link `EntityKind` — so kinds like `plan_step`
/// or `repo` are rejected here by design.
pub const KindID = struct {
    kind: engine.external.link.ExternalEntityKind,
    id: i64,
};

/// Parse a `<kind>:<id>` reference into a `KindID`. Splits on the LAST
/// colon so external ids that themselves contain colons are handled.
/// Returns `error.InvalidInput` on any malformed or unknown-kind input.
/// Shared by sync pull / push / status (was duplicated byte-for-byte).
pub fn parseKindIDRef(s: []const u8) !KindID {
    var i: usize = s.len;
    while (i > 0) : (i -= 1) {
        if (s[i - 1] == ':') {
            const kind_text = s[0 .. i - 1];
            const id_text = s[i..];
            if (kind_text.len == 0 or id_text.len == 0) break;
            const kind = engine.external.link.ExternalEntityKind.fromText(kind_text) orelse break;
            const id = std.fmt.parseInt(i64, id_text, 10) catch break;
            return .{ .kind = kind, .id = id };
        }
    }
    return error.InvalidInput;
}

pub const PullPushResult = struct {
    link_id: i64,
    outcome: engine.external.sync.Outcome,
    fields_changed: []const []const u8,
    detail: []const u8,
};

/// Drive `engine.external.sync.pullLink` against either the jira or github
/// adapter on the provided handle.
pub fn pullLink(
    d: anytype,
    allocator: std.mem.Allocator,
    link: engine.external.link.ExtLink,
    handle: *adapter_factory.Handle,
) !engine.external.sync.PullResult {
    return switch (handle.kind) {
        .jira => try engine.external.sync.pullLink(d, allocator, link, &handle.jira_adapter.?),
        .github => try engine.external.sync.pullLink(d, allocator, link, &handle.github_adapter.?),
    };
}

/// Drive `engine.external.sync.pushLink` similarly.
pub fn pushLink(
    d: anytype,
    allocator: std.mem.Allocator,
    link: engine.external.link.ExtLink,
    handle: *adapter_factory.Handle,
) !engine.external.sync.PushResult {
    return switch (handle.kind) {
        .jira => try engine.external.sync.pushLink(d, allocator, link, &handle.jira_adapter.?),
        .github => try engine.external.sync.pushLink(d, allocator, link, &handle.github_adapter.?),
    };
}

/// Drive `engine.external.sync.resolveConflict`.
pub fn resolveConflict(
    d: anytype,
    allocator: std.mem.Allocator,
    event_id: i64,
    keep: engine.external.sync.ResolveKeep,
    handle: *adapter_factory.Handle,
) !engine.external.sync.ResolveResult {
    return switch (handle.kind) {
        .jira => try engine.external.sync.resolveConflict(d, allocator, event_id, keep, &handle.jira_adapter.?),
        .github => try engine.external.sync.resolveConflict(d, allocator, event_id, keep, &handle.github_adapter.?),
    };
}

/// Open + build adapter for a system row, returning the owned handle. Caller
/// is responsible for calling `handle.deinit()` and `allocator.destroy(handle)`.
pub fn handleForSystem(
    allocator: std.mem.Allocator,
    io: std.Io,
    environ: *const std.process.Environ,
    sys: engine.external.system.ExternalSystem,
) !*adapter_factory.Handle {
    return adapter_factory.build(allocator, io, environ, sys);
}
