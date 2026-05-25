//! handlers/ext/remote — POST a rendered payload to a Jira or GitHub Issues
//! REST endpoint and extract the resulting (external_id, external_url).
//!
//! Factored out of `ext create` so `ext propagate` can reuse the same path.
//! No retries, no rate-limit handling — that's M11+ work if we need it.

const std = @import("std");
const engine = @import("engine");
const adapter_factory = @import("adapter_factory.zig");
const extsync = @import("engine").extsync;

pub const Created = struct {
    external_id: []u8,
    external_url: []u8,
};

pub fn createRemote(
    h: *adapter_factory.Handle,
    allocator: std.mem.Allocator,
    sys: engine.external.system.ExternalSystem,
    payload: []const u8,
) !Created {
    return switch (h.kind) {
        .jira => try createJira(h, allocator, sys, payload),
        .github => try createGithub(h, allocator, sys, payload),
    };
}

pub fn createJira(
    h: *adapter_factory.Handle,
    allocator: std.mem.Allocator,
    sys: engine.external.system.ExternalSystem,
    payload: []const u8,
) !Created {
    const base = sys.base_url orelse return error.InvalidInput;
    const url = try std.fmt.allocPrint(allocator, "{s}/rest/api/3/issue", .{trimSlash(base)});
    defer allocator.free(url);
    const auth = try std.fmt.allocPrint(allocator, "Bearer {s}", .{h.token});
    defer allocator.free(auth);
    const headers = [_]extsync.common.Header{
        .{ .name = "Content-Type", .value = "application/json" },
        .{ .name = "Authorization", .value = auth },
        .{ .name = "Accept", .value = "application/json" },
    };
    const response = try h.transport().send(allocator, .{
        .method = .post,
        .url = url,
        .headers = &headers,
        .body = payload,
    });
    defer response.deinit(allocator);
    if (response.status < 200 or response.status >= 300) return error.UnexpectedStatus;

    var parsed = std.json.parseFromSlice(std.json.Value, allocator, response.body, .{}) catch return error.ParseFailed;
    defer parsed.deinit();
    if (parsed.value != .object) return error.ParseFailed;
    const key_v = parsed.value.object.get("key") orelse return error.ParseFailed;
    if (key_v != .string) return error.ParseFailed;
    const key = key_v.string;
    const url_back = try std.fmt.allocPrint(allocator, "{s}/browse/{s}", .{ trimSlash(base), key });
    return .{
        .external_id = try allocator.dupe(u8, key),
        .external_url = url_back,
    };
}

pub fn createGithub(
    h: *adapter_factory.Handle,
    allocator: std.mem.Allocator,
    sys: engine.external.system.ExternalSystem,
    payload: []const u8,
) !Created {
    const base = sys.base_url orelse "https://api.github.com";
    const project = sys.default_project orelse return error.InvalidInput;
    const url = try std.fmt.allocPrint(allocator, "{s}/repos/{s}/issues", .{ trimSlash(base), project });
    defer allocator.free(url);
    const auth = try std.fmt.allocPrint(allocator, "Bearer {s}", .{h.token});
    defer allocator.free(auth);
    const headers = [_]extsync.common.Header{
        .{ .name = "Content-Type", .value = "application/json" },
        .{ .name = "Authorization", .value = auth },
        .{ .name = "Accept", .value = "application/vnd.github+json" },
    };
    const response = try h.transport().send(allocator, .{
        .method = .post,
        .url = url,
        .headers = &headers,
        .body = payload,
    });
    defer response.deinit(allocator);
    if (response.status < 200 or response.status >= 300) return error.UnexpectedStatus;

    var parsed = std.json.parseFromSlice(std.json.Value, allocator, response.body, .{}) catch return error.ParseFailed;
    defer parsed.deinit();
    if (parsed.value != .object) return error.ParseFailed;
    const num_v = parsed.value.object.get("number") orelse return error.ParseFailed;
    const number: i64 = switch (num_v) {
        .integer => |n| n,
        else => return error.ParseFailed,
    };
    const html_url_v = parsed.value.object.get("html_url");
    const html_url: []const u8 = if (html_url_v) |v|
        (if (v == .string) v.string else "")
    else
        "";
    const external_id = try std.fmt.allocPrint(allocator, "{s}#{d}", .{ project, number });
    return .{
        .external_id = external_id,
        .external_url = try allocator.dupe(u8, html_url),
    };
}

fn trimSlash(s: []const u8) []const u8 {
    if (s.len == 0) return s;
    if (s[s.len - 1] == '/') return s[0 .. s.len - 1];
    return s;
}
