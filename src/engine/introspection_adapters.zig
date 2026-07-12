//! Privacy boundary shared by usage-introspection source adapters.
//!
//! Vendor parsers discard prose and values into `SourceRecord`; only this
//! module's bounded, normalized output may cross into classification.

const std = @import("std");

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

pub const SourceRecord = struct {
    schema: u8,
    verb_path: []const u8,
    category: Category,
    timestamp: []const u8,
    // Deliberately accepted only so adapters have an explicit discard point.
    raw_prompt: ?[]const u8 = null,
    argument_values: ?[]const u8 = null,
    entity_title: ?[]const u8 = null,
};

pub fn normalize(vendor: Vendor, record: SourceRecord) ?Signal {
    if (record.schema != 1 or !validVerbPath(record.verb_path) or !validTimestamp(record.timestamp)) return null;
    return .{
        .vendor = vendor,
        .verb_path = record.verb_path,
        .category = record.category,
        .count = 1,
        .first_seen = record.timestamp,
        .last_seen = record.timestamp,
    };
}

/// Built-ins apply only when the corresponding override is empty. Disabled
/// wins over both, making discovery precedence deterministic.
pub fn discover(enabled: bool, override: []const u8, builtin: []const u8) ?[]const u8 {
    if (!enabled) return null;
    return if (override.len != 0) override else builtin;
}

/// CLI-log rows are authoritative for matching time buckets. Transcript rows
/// remain only when they contribute vendor-only categories or uncovered time.
pub fn cliLogAuthoritative(cli: Signal, transcript: Signal) bool {
    return std.mem.eql(u8, cli.verb_path, transcript.verb_path) and
        cli.category == transcript.category and
        std.mem.eql(u8, timeBucket(cli.first_seen), timeBucket(transcript.first_seen));
}

fn validVerbPath(path: []const u8) bool {
    if (!std.mem.startsWith(u8, path, "planar ")) return false;
    // Values, quoting, shell operators, and paths cannot enter normalized keys.
    return std.mem.indexOfAny(u8, path, "=\"';&|\\/") == null;
}

fn validTimestamp(ts: []const u8) bool {
    return ts.len >= 20 and ts[4] == '-' and ts[10] == 'T';
}

fn timeBucket(ts: []const u8) []const u8 {
    return ts[0..@min(ts.len, 13)];
}

test "all vendors normalize to one bounded redacted shape" {
    const secret = "PROMPT secret-token=sk-fixture entity=Private Roadmap";
    inline for (.{ Vendor.claude, .codex, .copilot, .cli_log }) |vendor| {
        const signal = normalize(vendor, .{
            .schema = 1,
            .verb_path = "planar task add",
            .category = .failure,
            .timestamp = "2026-07-12T12:00:00Z",
            .raw_prompt = secret,
            .argument_values = "--body private prose --token sk-fixture",
            .entity_title = "Private Roadmap",
        }).?;
        const rendered = try std.fmt.allocPrint(std.testing.allocator, "{s}|{s}|{s}", .{ @tagName(signal.vendor), signal.verb_path, signal.first_seen });
        defer std.testing.allocator.free(rendered);
        try std.testing.expect(std.mem.indexOf(u8, rendered, "secret-token") == null);
        try std.testing.expect(std.mem.indexOf(u8, rendered, "private prose") == null);
        try std.testing.expect(std.mem.indexOf(u8, rendered, "Private Roadmap") == null);
        try std.testing.expect(rendered.len < 128);
    }
}

test "discovery precedence and malformed records are deterministic" {
    try std.testing.expectEqualStrings("/override", discover(true, "/override", "/builtin").?);
    try std.testing.expectEqualStrings("/builtin", discover(true, "", "/builtin").?);
    try std.testing.expect(discover(false, "/override", "/builtin") == null);
    try std.testing.expect(normalize(.claude, .{ .schema = 2, .verb_path = "planar task add", .category = .failure, .timestamp = "2026-07-12T12:00:00Z" }) == null);
    try std.testing.expect(normalize(.codex, .{ .schema = 1, .verb_path = "planar task add --body=secret", .category = .failure, .timestamp = "2026-07-12T12:00:00Z" }) == null);
}

test "cli log matching bucket is authoritative" {
    const cli = normalize(.cli_log, .{ .schema = 1, .verb_path = "planar task add", .category = .failure, .timestamp = "2026-07-12T12:01:00Z" }).?;
    const transcript = normalize(.claude, .{ .schema = 1, .verb_path = "planar task add", .category = .failure, .timestamp = "2026-07-12T12:59:00Z" }).?;
    try std.testing.expect(cliLogAuthoritative(cli, transcript));
}
