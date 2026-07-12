//! Deterministic privacy boundary for usage-introspection source adapters.
//!
//! `collectPreview` consumes vendor JSONL, recognizes a deliberately small
//! schema allowlist, and returns only bounded aggregate evidence. Source text
//! and parsed JSON never outlive the call and this module has no write handle.

const std = @import("std");

/// A preview cannot carry more distinct evidence buckets than this.
pub const max_evidence_buckets: usize = 1024;
pub const default_max_files: usize = 128;
pub const default_max_bytes: usize = 4 * 1024 * 1024;
pub const default_max_records: usize = 50_000;

pub const Vendor = enum { claude, codex, copilot, cli_log };
pub const Category = enum { failure, retry, abandonment, gap };
pub const CoverageState = enum { observed, unavailable, disabled };
pub const WarningKind = enum { unavailable, disabled, malformed, file_cap, byte_cap, record_cap, evidence_cap, cli_adapter_failed };

pub const Warning = struct {
    vendor: Vendor,
    kind: WarningKind,
    count: u32 = 1,
};

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
    capped: u32 = 0,
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
    warnings: []Warning,

    pub fn deinit(self: *Preview, allocator: std.mem.Allocator) void {
        for (self.signals) |signal| {
            allocator.free(signal.verb_path);
            allocator.free(signal.first_seen);
            allocator.free(signal.last_seen);
        }
        allocator.free(self.signals);
        allocator.free(self.coverage);
        allocator.free(self.warnings);
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
                cov.capped +|= 1;
            }
        }
        try coverage.append(allocator, cov);
    }

    removeCliDuplicates(allocator, &signals);
    std.mem.sort(Signal, signals.items, {}, signalLessThan);
    std.mem.sort(Coverage, coverage.items, {}, coverageLessThan);
    var warnings: std.ArrayList(Warning) = .empty;
    errdefer warnings.deinit(allocator);
    for (coverage.items) |cov| {
        if (cov.state == .disabled) try warnings.append(allocator, .{ .vendor = cov.vendor, .kind = .disabled });
        if (cov.state == .unavailable) try warnings.append(allocator, .{ .vendor = cov.vendor, .kind = .unavailable });
        if (cov.malformed != 0) try warnings.append(allocator, .{ .vendor = cov.vendor, .kind = .malformed, .count = cov.malformed });
        if (cov.capped != 0) try warnings.append(allocator, .{ .vendor = cov.vendor, .kind = .evidence_cap, .count = cov.capped });
    }
    return .{ .signals = try signals.toOwnedSlice(allocator), .coverage = try coverage.toOwnedSlice(allocator), .warnings = try warnings.toOwnedSlice(allocator) };
}

pub const CollectorLimits = struct {
    max_files: usize = default_max_files,
    max_bytes: usize = default_max_bytes,
    max_records: usize = default_max_records,
};

pub const TranscriptConfig = struct {
    home_dir: []const u8,
    claude_enabled: bool = true,
    claude_path: []const u8 = "",
    codex_enabled: bool = true,
    codex_path: []const u8 = "",
    copilot_enabled: bool = true,
    copilot_path: []const u8 = "",
};

/// Read-only boundary for authoritative CLI rows. The adapter is supplied by
/// the binary that owns the supported local API; this engine module never
/// opens SQLite. Returned JSONL is caller-allocated with `allocator`.
pub const CliLogAdapter = struct {
    context: *anyopaque,
    enabled: bool,
    read: *const fn (*anyopaque, std.mem.Allocator, usize) anyerror!?[]u8,
};

const OwnedSource = struct { raw: RawSource, bytes: ?[]u8 = null };

/// Discover configured/built-in transcript paths and collect a bounded,
/// read-only preview. Overrides accept a file, a directory (recursively), or
/// the documented `/**/*.jsonl` suffix. Paths and records are sorted before
/// normalization so filesystem enumeration order cannot affect results.
pub fn collectPreviewFromPaths(
    allocator: std.mem.Allocator,
    config: TranscriptConfig,
    cli: ?CliLogAdapter,
    limits: CollectorLimits,
) !Preview {
    var owned: std.ArrayList(OwnedSource) = .empty;
    defer {
        for (owned.items) |item| if (item.bytes) |bytes| allocator.free(bytes);
        owned.deinit(allocator);
    }
    var extra_warnings: std.ArrayList(Warning) = .empty;
    defer extra_warnings.deinit(allocator);
    var files_left = limits.max_files;
    var bytes_left = limits.max_bytes;
    var records_left = limits.max_records;

    try collectVendorPath(allocator, &owned, &extra_warnings, .claude, config.claude_enabled, config.claude_path, config.home_dir, ".claude/projects", true, &files_left, &bytes_left, &records_left);
    try collectVendorPath(allocator, &owned, &extra_warnings, .codex, config.codex_enabled, config.codex_path, config.home_dir, ".codex/sessions", true, &files_left, &bytes_left, &records_left);
    try collectVendorPath(allocator, &owned, &extra_warnings, .copilot, config.copilot_enabled, config.copilot_path, config.home_dir, ".copilot/session-state", false, &files_left, &bytes_left, &records_left);

    if (cli) |adapter| {
        if (!adapter.enabled) {
            try owned.append(allocator, .{ .raw = .{ .vendor = .cli_log, .enabled = false } });
        } else {
            var adapter_failed = false;
            const maybe_bytes = adapter.read(adapter.context, allocator, bytes_left) catch blk: {
                adapter_failed = true;
                break :blk null;
            };
            if (maybe_bytes) |bytes| {
                const records = countRecords(bytes);
                if (bytes.len > bytes_left) {
                    allocator.free(bytes);
                    try owned.append(allocator, .{ .raw = .{ .vendor = .cli_log } });
                    try extra_warnings.append(allocator, .{ .vendor = .cli_log, .kind = .byte_cap });
                } else if (records > records_left) {
                    allocator.free(bytes);
                    try owned.append(allocator, .{ .raw = .{ .vendor = .cli_log } });
                    try extra_warnings.append(allocator, .{ .vendor = .cli_log, .kind = .record_cap });
                } else {
                    bytes_left -= bytes.len;
                    records_left -= records;
                    try owned.append(allocator, .{ .raw = .{ .vendor = .cli_log, .jsonl = bytes }, .bytes = bytes });
                }
            } else {
                try owned.append(allocator, .{ .raw = .{ .vendor = .cli_log, .available = false } });
                if (adapter_failed) try extra_warnings.append(allocator, .{ .vendor = .cli_log, .kind = .cli_adapter_failed });
            }
        }
    } else try owned.append(allocator, .{ .raw = .{ .vendor = .cli_log, .available = false } });

    const raws = try allocator.alloc(RawSource, owned.items.len);
    defer allocator.free(raws);
    for (owned.items, 0..) |item, i| raws[i] = item.raw;
    var preview = try collectPreview(allocator, raws);
    errdefer preview.deinit(allocator);
    if (extra_warnings.items.len != 0) {
        const merged = try allocator.alloc(Warning, preview.warnings.len + extra_warnings.items.len);
        @memcpy(merged[0..preview.warnings.len], preview.warnings);
        @memcpy(merged[preview.warnings.len..], extra_warnings.items);
        allocator.free(preview.warnings);
        preview.warnings = merged;
    }
    return preview;
}

/// Configuration-plane bridge. `transcripts` is the resolved
/// `effective.Introspection.transcripts` value; keeping this structural avoids
/// coupling the no-write collector to config parsing or database modules.
pub fn collectConfiguredPreview(allocator: std.mem.Allocator, home_dir: []const u8, transcripts: anytype, cli: ?CliLogAdapter, limits: CollectorLimits) !Preview {
    return collectPreviewFromPaths(allocator, .{
        .home_dir = home_dir,
        .claude_enabled = transcripts.claude_enabled,
        .claude_path = transcripts.claude_path,
        .codex_enabled = transcripts.codex_enabled,
        .codex_path = transcripts.codex_path,
        .copilot_enabled = transcripts.copilot_enabled,
        .copilot_path = transcripts.copilot_path,
    }, cli, limits);
}

fn collectVendorPath(allocator: std.mem.Allocator, owned: *std.ArrayList(OwnedSource), warnings: *std.ArrayList(Warning), vendor: Vendor, enabled: bool, override: []const u8, home: []const u8, builtin_rel: []const u8, jsonl_only: bool, files_left: *usize, bytes_left: *usize, records_left: *usize) !void {
    if (!enabled) return owned.append(allocator, .{ .raw = .{ .vendor = vendor, .enabled = false } });
    const builtin = try std.fs.path.join(allocator, &.{ home, builtin_rel });
    defer allocator.free(builtin);
    var selected = if (override.len == 0) builtin else override;
    if (std.mem.endsWith(u8, selected, "/**/*.jsonl")) selected = selected[0 .. selected.len - "/**/*.jsonl".len];

    var paths: std.ArrayList([]u8) = .empty;
    defer {
        for (paths.items) |path| allocator.free(path);
        paths.deinit(allocator);
    }
    const io = fsIo();
    const stat = std.Io.Dir.cwd().statFile(io, selected, .{}) catch {
        return owned.append(allocator, .{ .raw = .{ .vendor = vendor, .available = false } });
    };
    if (stat.kind == .file) {
        try paths.append(allocator, try allocator.dupe(u8, selected));
    } else if (stat.kind == .directory) {
        var dir = try std.Io.Dir.cwd().openDir(io, selected, .{ .iterate = true });
        defer dir.close(io);
        var walker = try dir.walk(allocator);
        defer walker.deinit();
        while (try walker.next(io)) |entry| if (entry.kind == .file and (!jsonl_only or std.mem.endsWith(u8, entry.path, ".jsonl"))) {
            try paths.append(allocator, try std.fs.path.join(allocator, &.{ selected, entry.path }));
        };
    } else return owned.append(allocator, .{ .raw = .{ .vendor = vendor, .available = false } });
    std.mem.sort([]u8, paths.items, {}, struct {
        fn less(_: void, a: []u8, b: []u8) bool {
            return std.mem.order(u8, a, b) == .lt;
        }
    }.less);

    var combined: std.ArrayList(u8) = .empty;
    defer combined.deinit(allocator);
    var scanned_files: usize = 0;
    for (paths.items) |path| {
        if (files_left.* == 0) {
            try warnings.append(allocator, .{ .vendor = vendor, .kind = .file_cap });
            break;
        }
        const file_stat = std.Io.Dir.cwd().statFile(io, path, .{}) catch continue;
        const size = file_stat.size;
        if (size > bytes_left.*) {
            try warnings.append(allocator, .{ .vendor = vendor, .kind = .byte_cap });
            break;
        }
        const bytes = try std.Io.Dir.cwd().readFileAlloc(io, path, allocator, std.Io.Limit.limited(bytes_left.*));
        defer allocator.free(bytes);
        const record_count = countRecords(bytes);
        if (record_count > records_left.*) {
            try warnings.append(allocator, .{ .vendor = vendor, .kind = .record_cap });
            break;
        }
        try combined.appendSlice(allocator, bytes);
        try combined.append(allocator, '\n');
        files_left.* -= 1;
        bytes_left.* -= bytes.len;
        records_left.* -= record_count;
        scanned_files += 1;
    }
    if (scanned_files == 0 and paths.items.len != 0 and combined.items.len == 0) {
        try owned.append(allocator, .{ .raw = .{ .vendor = vendor } });
    } else {
        const bytes = try combined.toOwnedSlice(allocator);
        try owned.append(allocator, .{ .raw = .{ .vendor = vendor, .jsonl = bytes }, .bytes = bytes });
    }
}

fn countRecords(bytes: []const u8) usize {
    var count: usize = 0;
    var lines = std.mem.splitScalar(u8, bytes, '\n');
    while (lines.next()) |line| if (std.mem.trim(u8, line, " \t\r").len != 0) {
        count += 1;
    };
    return count;
}

fn fsIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
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

fn fixtureCliRead(context: *anyopaque, allocator: std.mem.Allocator, max_bytes: usize) anyerror!?[]u8 {
    const jsonl: *const []const u8 = @ptrCast(@alignCast(context));
    if (jsonl.*.len > max_bytes) return error.StreamTooLong;
    return @as(?[]u8, try allocator.dupe(u8, jsonl.*));
}

test "path collector honors override precedence, recursive deterministic discovery, redaction, and CLI authority" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root_rel = try std.fs.path.join(std.testing.allocator, &.{ ".zig-cache/tmp", &tmp.sub_path });
    defer std.testing.allocator.free(root_rel);
    const root = try std.fs.path.resolve(std.testing.allocator, &.{root_rel});
    defer std.testing.allocator.free(root);
    try tmp.dir.createDirPath(std.testing.io, "home/.claude/projects/builtin");
    try tmp.dir.createDirPath(std.testing.io, "override/nested");
    try tmp.dir.createDirPath(std.testing.io, "home/.codex/sessions");
    try tmp.dir.writeFile(std.testing.io, .{ .sub_path = "home/.claude/projects/builtin/ignored.jsonl", .data = "{\"version\":1,\"type\":\"tool_result\",\"timestamp\":\"2026-07-12T09:00:00Z\",\"tool\":{\"name\":\"planar ignored\"},\"exit_code\":1}" });
    // Names are intentionally reverse chronological: lexical path order must
    // not leak filesystem enumeration order into the sorted preview.
    try tmp.dir.writeFile(std.testing.io, .{ .sub_path = "override/z.jsonl", .data = "{\"version\":1,\"type\":\"tool_result\",\"timestamp\":\"2026-07-12T12:05:00Z\",\"tool\":{\"name\":\"planar task add\",\"input\":{\"token\":\"sk-secret\"}},\"exit_code\":1}\nmalformed" });
    try tmp.dir.writeFile(std.testing.io, .{ .sub_path = "override/nested/a.jsonl", .data = "{\"version\":1,\"type\":\"tool_result\",\"timestamp\":\"2026-07-12T11:05:00Z\",\"tool\":{\"name\":\"planar plan show\"},\"exit_code\":1}" });
    try tmp.dir.writeFile(std.testing.io, .{ .sub_path = "home/.codex/sessions/a.jsonl", .data = "{\"schema_version\":1,\"event\":\"command_execution\",\"timestamp\":\"2026-07-12T12:06:00Z\",\"command_name\":\"planar task add\",\"arguments\":[\"private\"],\"exit_code\":1}" });

    const override_path = try std.fs.path.join(std.testing.allocator, &.{ root, "override/**/*.jsonl" });
    defer std.testing.allocator.free(override_path);
    const home = try std.fs.path.join(std.testing.allocator, &.{ root, "home" });
    defer std.testing.allocator.free(home);
    const cli_jsonl: []const u8 = "{\"schema\":1,\"kind\":\"cli_invocation\",\"recorded_at\":\"2026-07-12T12:02:00Z\",\"verb_path\":\"planar task add\",\"exit_code\":1}";
    var preview = try collectPreviewFromPaths(std.testing.allocator, .{
        .home_dir = home,
        .claude_path = override_path,
        .copilot_enabled = false,
    }, .{ .context = @ptrCast(@constCast(&cli_jsonl)), .enabled = true, .read = fixtureCliRead }, .{});
    defer preview.deinit(std.testing.allocator);

    try std.testing.expectEqual(@as(usize, 2), preview.signals.len);
    try std.testing.expectEqualStrings("planar plan show", preview.signals[0].verb_path);
    try std.testing.expectEqual(Vendor.cli_log, preview.signals[1].vendor);
    for (preview.signals) |signal| {
        try std.testing.expect(std.mem.indexOf(u8, signal.verb_path, "secret") == null);
        try std.testing.expect(std.mem.indexOf(u8, signal.verb_path, "private") == null);
        try std.testing.expect(std.mem.indexOf(u8, signal.verb_path, "ignored") == null);
    }
    try std.testing.expectEqual(@as(u32, 1), preview.coverage[0].malformed);
    try std.testing.expectEqual(CoverageState.disabled, preview.coverage[2].state);
}

test "path collector reports unavailable and intake caps without writing" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root_rel = try std.fs.path.join(std.testing.allocator, &.{ ".zig-cache/tmp", &tmp.sub_path });
    defer std.testing.allocator.free(root_rel);
    const root = try std.fs.path.resolve(std.testing.allocator, &.{root_rel});
    defer std.testing.allocator.free(root);
    try tmp.dir.createDirPath(std.testing.io, "claude");
    try tmp.dir.writeFile(std.testing.io, .{ .sub_path = "claude/a.jsonl", .data = "{\"version\":1,\"type\":\"tool_result\",\"timestamp\":\"2026-07-12T12:05:00Z\",\"tool\":{\"name\":\"planar task add\"},\"exit_code\":1}" });
    try tmp.dir.writeFile(std.testing.io, .{ .sub_path = "claude/b.jsonl", .data = "{\"version\":1,\"type\":\"tool_result\",\"timestamp\":\"2026-07-12T13:05:00Z\",\"tool\":{\"name\":\"planar task add\"},\"exit_code\":1}" });
    const claude = try std.fs.path.join(std.testing.allocator, &.{ root, "claude" });
    defer std.testing.allocator.free(claude);
    var preview = try collectPreviewFromPaths(std.testing.allocator, .{
        .home_dir = root,
        .claude_path = claude,
        .codex_path = "/definitely/missing/codex",
        .copilot_enabled = false,
    }, null, .{ .max_files = 1, .max_bytes = 4096, .max_records = 10 });
    defer preview.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(usize, 1), preview.signals.len);
    try std.testing.expectEqual(CoverageState.unavailable, preview.coverage[1].state);
    var saw_file_cap = false;
    var saw_missing = false;
    var saw_cli_unavailable = false;
    for (preview.warnings) |warning| {
        if (warning.vendor == .claude and warning.kind == .file_cap) saw_file_cap = true;
        if (warning.vendor == .codex and warning.kind == .unavailable) saw_missing = true;
        if (warning.vendor == .cli_log and warning.kind == .unavailable) saw_cli_unavailable = true;
    }
    try std.testing.expect(saw_file_cap and saw_missing and saw_cli_unavailable);
    // Collector opens only read handles: the fixture tree is unchanged.
    try std.testing.expect((try tmp.dir.statFile(std.testing.io, "claude/a.jsonl", .{})).kind == .file);
    try std.testing.expect((try tmp.dir.statFile(std.testing.io, "claude/b.jsonl", .{})).kind == .file);
}

test "path collector accounts byte and record caps" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root_rel = try std.fs.path.join(std.testing.allocator, &.{ ".zig-cache/tmp", &tmp.sub_path });
    defer std.testing.allocator.free(root_rel);
    const root = try std.fs.path.resolve(std.testing.allocator, &.{root_rel});
    defer std.testing.allocator.free(root);
    try tmp.dir.writeFile(std.testing.io, .{ .sub_path = "records.jsonl", .data = "{\"version\":1,\"type\":\"tool_result\",\"timestamp\":\"2026-07-12T12:05:00Z\",\"tool\":{\"name\":\"planar task add\"},\"exit_code\":1}\n" ++
        "{\"version\":1,\"type\":\"tool_result\",\"timestamp\":\"2026-07-12T13:05:00Z\",\"tool\":{\"name\":\"planar task add\"},\"exit_code\":1}" });
    const path = try std.fs.path.join(std.testing.allocator, &.{ root, "records.jsonl" });
    defer std.testing.allocator.free(path);
    var byte_limited = try collectPreviewFromPaths(std.testing.allocator, .{ .home_dir = root, .claude_path = path, .codex_enabled = false, .copilot_enabled = false }, null, .{ .max_bytes = 8 });
    defer byte_limited.deinit(std.testing.allocator);
    var record_limited = try collectPreviewFromPaths(std.testing.allocator, .{ .home_dir = root, .claude_path = path, .codex_enabled = false, .copilot_enabled = false }, null, .{ .max_bytes = 4096, .max_records = 1 });
    defer record_limited.deinit(std.testing.allocator);
    try std.testing.expect(hasWarning(byte_limited.warnings, .claude, .byte_cap));
    try std.testing.expect(hasWarning(record_limited.warnings, .claude, .record_cap));
    try std.testing.expectEqual(@as(usize, 0), byte_limited.signals.len);
    try std.testing.expectEqual(@as(usize, 0), record_limited.signals.len);
}

test "distinct evidence cap is explicit and counted" {
    var jsonl: std.ArrayList(u8) = .empty;
    defer jsonl.deinit(std.testing.allocator);
    for (0..max_evidence_buckets + 1) |i| {
        const line = try std.fmt.allocPrint(
            std.testing.allocator,
            "{{\"schema_version\":1,\"event\":\"command_execution\",\"timestamp\":\"2026-07-12T12:00:00Z\",\"command_name\":\"planar verb {d}\",\"exit_code\":1}}\n",
            .{i},
        );
        defer std.testing.allocator.free(line);
        try jsonl.appendSlice(std.testing.allocator, line);
    }
    var preview = try collectPreview(std.testing.allocator, &.{.{ .vendor = .codex, .jsonl = jsonl.items }});
    defer preview.deinit(std.testing.allocator);
    try std.testing.expectEqual(max_evidence_buckets, preview.signals.len);
    try std.testing.expectEqual(@as(u32, 1), preview.coverage[0].capped);
    try std.testing.expect(hasWarning(preview.warnings, .codex, .evidence_cap));
}

fn hasWarning(warnings: []const Warning, vendor: Vendor, kind: WarningKind) bool {
    for (warnings) |warning| if (warning.vendor == vendor and warning.kind == kind) return true;
    return false;
}
