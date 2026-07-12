//! Deterministic privacy boundary for usage-introspection source adapters.
//!
//! `collectPreview` consumes vendor JSONL, recognizes a deliberately small
//! schema allowlist, and returns only bounded aggregate evidence. Source text
//! and parsed JSON never outlive the call and this module has no write handle.

const std = @import("std");

/// A preview cannot carry more distinct evidence buckets than this.
pub const max_evidence_buckets: usize = 1024;

pub const Vendor = enum { claude, codex, copilot, cli_log };
pub const Category = enum { failure, retry, abandonment, gap };
pub const CoverageState = enum { observed, unavailable, disabled };

pub const Signal = struct {
    vendor: Vendor,
    verb_path: []const u8,
    category: Category,
    count: u32,
    first_seen: []const u8,
    last_seen: []const u8,
};

pub const Coverage = struct {
    vendor: Vendor,
    state: CoverageState,
    scanned: u32 = 0,
    malformed: u32 = 0,
    normalized: u32 = 0,
};

/// One discovered source. `jsonl` may contain multiple raw vendor records.
pub const RawSource = struct {
    vendor: Vendor,
    enabled: bool = true,
    available: bool = true,
    jsonl: []const u8 = "",
};

/// Redacted preview input. It owns only normalized keys and timestamps.
pub const Preview = struct {
    signals: []Signal,
    coverage: []Coverage,

    pub fn deinit(self: *Preview, allocator: std.mem.Allocator) void {
        for (self.signals) |signal| {
            allocator.free(signal.verb_path);
            allocator.free(signal.first_seen);
            allocator.free(signal.last_seen);
        }
        allocator.free(self.signals);
        allocator.free(self.coverage);
        self.* = undefined;
    }
};

const Extracted = struct {
    verb_path: []const u8,
    category: Category,
    timestamp: []const u8,
};

/// Collect a no-write preview from raw vendor JSONL.
///
/// Malformed lines include invalid JSON, unknown versions/event kinds, and
/// records whose bounded command or timestamp fields fail validation.
pub fn collectPreview(allocator: std.mem.Allocator, sources: []const RawSource) !Preview {
    var signals: std.ArrayList(Signal) = .empty;
    errdefer {
        for (signals.items) |signal| freeSignal(allocator, signal);
        signals.deinit(allocator);
    }
    var coverage: std.ArrayList(Coverage) = .empty;
    errdefer coverage.deinit(allocator);

    for (sources) |source| {
        var cov = Coverage{ .vendor = source.vendor, .state = .observed };
        if (!source.enabled) {
            cov.state = .disabled;
            try coverage.append(allocator, cov);
            continue;
        }
        if (!source.available) {
            cov.state = .unavailable;
            try coverage.append(allocator, cov);
            continue;
        }

        var lines = std.mem.splitScalar(u8, source.jsonl, '\n');
        while (lines.next()) |line| {
            const trimmed = std.mem.trim(u8, line, " \t\r");
            if (trimmed.len == 0) continue;
            cov.scanned +|= 1;
            var parsed = std.json.parseFromSlice(std.json.Value, allocator, trimmed, .{}) catch {
                cov.malformed +|= 1;
                continue;
            };
            defer parsed.deinit();
            const extracted = extract(source.vendor, parsed.value) orelse {
                cov.malformed +|= 1;
                continue;
            };
            if (try addAggregate(allocator, &signals, source.vendor, extracted)) {
                cov.normalized +|= 1;
            } else {
                cov.malformed +|= 1;
            }
        }
        try coverage.append(allocator, cov);
    }

    removeCliDuplicates(allocator, &signals);
    std.mem.sort(Signal, signals.items, {}, signalLessThan);
    std.mem.sort(Coverage, coverage.items, {}, coverageLessThan);
    return .{ .signals = try signals.toOwnedSlice(allocator), .coverage = try coverage.toOwnedSlice(allocator) };
}

/// Built-ins apply only when the corresponding override is empty. Disabled
/// wins over both, making discovery precedence deterministic.
pub fn discover(enabled: bool, override: []const u8, builtin: []const u8) ?[]const u8 {
    if (!enabled) return null;
    return if (override.len != 0) override else builtin;
}

fn extract(vendor: Vendor, value: std.json.Value) ?Extracted {
    if (value != .object) return null;
    const obj = value.object;
    return switch (vendor) {
        .claude => blk: {
            if (!integerEquals(obj.get("version"), 1) or !stringEquals(obj.get("type"), "tool_result")) break :blk null;
            const tool = objectValue(obj.get("tool")) orelse break :blk null;
            break :blk finish(tool.get("name"), obj.get("timestamp"), categoryFromExit(obj.get("exit_code"), boolValue(obj.get("retry")), false));
        },
        .codex => blk: {
            if (!integerEquals(obj.get("schema_version"), 1) or !stringEquals(obj.get("event"), "command_execution")) break :blk null;
            break :blk finish(obj.get("command_name"), obj.get("timestamp"), categoryFromExit(obj.get("exit_code"), boolValue(obj.get("retry_of_previous")), false));
        },
        .copilot => blk: {
            if (!stringEquals(obj.get("version"), "1") or !stringEquals(obj.get("kind"), "shell_result")) break :blk null;
            const command = objectValue(obj.get("command")) orelse break :blk null;
            const abandoned = stringEquals(obj.get("status"), "abandoned");
            break :blk finish(command.get("name"), obj.get("time"), categoryFromExit(obj.get("exit_code"), boolValue(obj.get("retry")), abandoned));
        },
        .cli_log => blk: {
            if (!integerEquals(obj.get("schema"), 1) or !stringEquals(obj.get("kind"), "cli_invocation")) break :blk null;
            break :blk finish(obj.get("verb_path"), obj.get("recorded_at"), categoryFromExit(obj.get("exit_code"), boolValue(obj.get("retry")), false));
        },
    };
}

fn finish(command_value: ?std.json.Value, timestamp_value: ?std.json.Value, category: ?Category) ?Extracted {
    const command = stringValue(command_value) orelse return null;
    const timestamp = stringValue(timestamp_value) orelse return null;
    return .{
        .verb_path = if (validVerbPath(command)) command else return null,
        .category = category orelse return null,
        .timestamp = if (validTimestamp(timestamp)) timestamp else return null,
    };
}

fn categoryFromExit(exit_value: ?std.json.Value, retry: bool, abandoned: bool) ?Category {
    if (abandoned) return .abandonment;
    if (retry) return .retry;
    const code = integerValue(exit_value) orelse return null;
    return if (code == 0) .gap else .failure;
}

fn addAggregate(allocator: std.mem.Allocator, signals: *std.ArrayList(Signal), vendor: Vendor, item: Extracted) !bool {
    const bucket = timeBucket(item.timestamp);
    for (signals.items) |*signal| {
        if (signal.vendor == vendor and signal.category == item.category and
            std.mem.eql(u8, signal.verb_path, item.verb_path) and
            std.mem.eql(u8, timeBucket(signal.first_seen), bucket))
        {
            signal.count +|= 1;
            if (std.mem.order(u8, item.timestamp, signal.first_seen) == .lt) {
                allocator.free(signal.first_seen);
                signal.first_seen = try allocator.dupe(u8, item.timestamp);
            }
            if (std.mem.order(u8, item.timestamp, signal.last_seen) == .gt) {
                allocator.free(signal.last_seen);
                signal.last_seen = try allocator.dupe(u8, item.timestamp);
            }
            return true;
        }
    }
    if (signals.items.len == max_evidence_buckets) return false;
    try signals.append(allocator, .{
        .vendor = vendor,
        .verb_path = try allocator.dupe(u8, item.verb_path),
        .category = item.category,
        .count = 1,
        .first_seen = try allocator.dupe(u8, item.timestamp),
        .last_seen = try allocator.dupe(u8, item.timestamp),
    });
    return true;
}

fn removeCliDuplicates(allocator: std.mem.Allocator, signals: *std.ArrayList(Signal)) void {
    var i: usize = 0;
    while (i < signals.items.len) {
        const candidate = signals.items[i];
        if (candidate.vendor != .cli_log and hasAuthoritativeCli(signals.items, candidate)) {
            freeSignal(allocator, candidate);
            _ = signals.orderedRemove(i);
        } else i += 1;
    }
}

fn hasAuthoritativeCli(signals: []const Signal, candidate: Signal) bool {
    for (signals) |signal| if (signal.vendor == .cli_log and signal.category == candidate.category and
        std.mem.eql(u8, signal.verb_path, candidate.verb_path) and
        std.mem.eql(u8, timeBucket(signal.first_seen), timeBucket(candidate.first_seen))) return true;
    return false;
}

fn freeSignal(allocator: std.mem.Allocator, signal: Signal) void {
    allocator.free(signal.verb_path);
    allocator.free(signal.first_seen);
    allocator.free(signal.last_seen);
}

fn validVerbPath(path: []const u8) bool {
    if (path.len > 96 or !std.mem.startsWith(u8, path, "planar ")) return false;
    return std.mem.indexOfAny(u8, path, "=\"';&|\\/\n\r\t") == null and std.mem.indexOf(u8, path, "--") == null;
}

fn validTimestamp(ts: []const u8) bool {
    return ts.len >= 20 and ts.len <= 35 and ts[4] == '-' and ts[10] == 'T';
}

fn timeBucket(ts: []const u8) []const u8 {
    return ts[0..@min(ts.len, 13)];
}

fn stringValue(value: ?std.json.Value) ?[]const u8 {
    const v = value orelse return null;
    return if (v == .string) v.string else null;
}
fn integerValue(value: ?std.json.Value) ?i64 {
    const v = value orelse return null;
    return if (v == .integer) v.integer else null;
}
fn boolValue(value: ?std.json.Value) bool {
    const v = value orelse return false;
    return v == .bool and v.bool;
}
fn objectValue(value: ?std.json.Value) ?std.json.ObjectMap {
    const v = value orelse return null;
    return if (v == .object) v.object else null;
}
fn integerEquals(value: ?std.json.Value, expected: i64) bool {
    return (integerValue(value) orelse return false) == expected;
}
fn stringEquals(value: ?std.json.Value, expected: []const u8) bool {
    return std.mem.eql(u8, stringValue(value) orelse return false, expected);
}

fn signalLessThan(_: void, a: Signal, b: Signal) bool {
    const av = @intFromEnum(a.vendor);
    const bv = @intFromEnum(b.vendor);
    if (av != bv) return av < bv;
    const command_order = std.mem.order(u8, a.verb_path, b.verb_path);
    if (command_order != .eq) return command_order == .lt;
    const ac = @intFromEnum(a.category);
    const bc = @intFromEnum(b.category);
    if (ac != bc) return ac < bc;
    return std.mem.order(u8, a.first_seen, b.first_seen) == .lt;
}
fn coverageLessThan(_: void, a: Coverage, b: Coverage) bool {
    return @intFromEnum(a.vendor) < @intFromEnum(b.vendor);
}

test "raw vendor fixtures redact, aggregate, count malformed, and report coverage" {
    const sources = [_]RawSource{
        .{ .vendor = .claude, .jsonl = "{\"version\":1,\"type\":\"tool_result\",\"timestamp\":\"2026-07-12T12:05:00Z\",\"tool\":{\"name\":\"planar task add\",\"input\":{\"body\":\"private prose\",\"token\":\"sk-fixture\"}},\"exit_code\":1,\"retry\":false,\"prompt\":\"secret prompt\"}\n" ++
            "{\"version\":9,\"type\":\"tool_result\",\"prompt\":\"must not leak\"}" },
        .{ .vendor = .codex, .jsonl = "{\"schema_version\":1,\"event\":\"command_execution\",\"timestamp\":\"2026-07-12T13:01:00Z\",\"command_name\":\"planar plan show\",\"arguments\":[\"private-scope\",\"sk-secret\"],\"exit_code\":1,\"retry_of_previous\":true,\"message\":\"prose\"}\n" ++
            "not-json" },
        .{ .vendor = .copilot, .jsonl = "{\"version\":\"1\",\"kind\":\"shell_result\",\"time\":\"2026-07-12T14:01:00Z\",\"command\":{\"name\":\"planar sync pull\",\"args\":[\"--scope\",\"private\"]},\"exit_code\":0,\"retry\":false,\"status\":\"abandoned\",\"transcript\":\"secret prose\"}" },
        .{ .vendor = .cli_log, .available = false },
        .{ .vendor = .cli_log, .enabled = false },
    };
    var preview = try collectPreview(std.testing.allocator, &sources);
    defer preview.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(usize, 3), preview.signals.len);
    try std.testing.expect(preview.signals.len <= max_evidence_buckets);
    try std.testing.expectEqual(@as(u32, 1), preview.coverage[0].malformed);
    try std.testing.expectEqual(@as(u32, 1), preview.coverage[1].malformed);
    try std.testing.expectEqual(CoverageState.unavailable, preview.coverage[3].state);
    try std.testing.expectEqual(CoverageState.disabled, preview.coverage[4].state);
    for (preview.signals) |signal| {
        try std.testing.expect(signal.verb_path.len <= 96);
        try std.testing.expect(std.mem.indexOf(u8, signal.verb_path, "secret") == null);
        try std.testing.expect(std.mem.indexOf(u8, signal.verb_path, "private") == null);
    }
}

test "deduplication is deterministic and cli log is authoritative" {
    const raw = "{\"schema\":1,\"kind\":\"cli_invocation\",\"recorded_at\":\"2026-07-12T12:02:00Z\",\"verb_path\":\"planar task add\",\"exit_code\":1}";
    const transcript = "{\"version\":1,\"type\":\"tool_result\",\"timestamp\":\"2026-07-12T12:05:00Z\",\"tool\":{\"name\":\"planar task add\",\"input\":{\"body\":\"secret\"}},\"exit_code\":1}\n" ++
        "{\"version\":1,\"type\":\"tool_result\",\"timestamp\":\"2026-07-12T12:06:00Z\",\"tool\":{\"name\":\"planar task add\"},\"exit_code\":1}";
    const sources = [_]RawSource{ .{ .vendor = .claude, .jsonl = transcript }, .{ .vendor = .cli_log, .jsonl = raw } };
    var preview = try collectPreview(std.testing.allocator, &sources);
    defer preview.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(usize, 1), preview.signals.len);
    try std.testing.expectEqual(Vendor.cli_log, preview.signals[0].vendor);
    try std.testing.expectEqual(@as(u32, 2), preview.coverage[0].normalized);
}

test "preview collector has no persistence dependency and discovery precedence is explicit" {
    try std.testing.expectEqualStrings("/override", discover(true, "/override", "/builtin").?);
    try std.testing.expectEqualStrings("/builtin", discover(true, "", "/builtin").?);
    try std.testing.expect(discover(false, "/override", "/builtin") == null);
    var preview = try collectPreview(std.testing.allocator, &.{});
    defer preview.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(usize, 0), preview.signals.len);
}
