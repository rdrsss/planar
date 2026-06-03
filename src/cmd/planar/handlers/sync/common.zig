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
