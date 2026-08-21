//! Integrity checks for sanitized, current-shape vendor transcript fixtures.
//!
//! The integrity tests lock the fixture union independently. One focused CLI
//! test also proves the complete fixture union reaches the normalizer without
//! exposing its privacy sentinels or source paths.

const std = @import("std");
const harness = @import("harness");

const claude_fixture = @embedFile("fixtures/introspection_transcripts/claude.jsonl");
const codex_fixture = @embedFile("fixtures/introspection_transcripts/codex.jsonl");
const copilot_fixture = @embedFile("fixtures/introspection_transcripts/copilot.jsonl");

const privacy_sentinels = [_][]const u8{
    "PRIVATE_TRANSCRIPT_PROSE_SENTINEL",
    "PRIVATE_ARGUMENT_VALUE_SENTINEL",
    "PRIVATE_ENTITY_TEXT_SENTINEL",
    "/private/raw/path/sentinel",
};

fn expectSanitizedJsonLines(fixture: []const u8, expected_records: usize) !void {
    var records: usize = 0;
    var lines = std.mem.splitScalar(u8, fixture, '\n');
    while (lines.next()) |line| {
        if (line.len == 0) continue;
        var parsed = try std.json.parseFromSlice(std.json.Value, std.testing.allocator, line, .{});
        defer parsed.deinit();
        try std.testing.expect(parsed.value == .object);
        records += 1;
    }
    try std.testing.expectEqual(expected_records, records);

    for (privacy_sentinels) |sentinel| {
        try std.testing.expect(std.mem.indexOf(u8, fixture, sentinel) != null);
    }
    try std.testing.expect(std.mem.indexOf(u8, fixture, "/Users/") == null);
    try std.testing.expect(std.mem.indexOf(u8, fixture, "C:\\Users\\") == null);
    try std.testing.expect(std.mem.indexOf(u8, fixture, "ghp_") == null);
    try std.testing.expect(std.mem.indexOf(u8, fixture, "sk-") == null);
}

test "Claude transcript fixture preserves current nested envelopes and boundary cases" {
    try expectSanitizedJsonLines(claude_fixture, 8);
    try std.testing.expect(std.mem.indexOf(u8, claude_fixture, "\"type\":\"assistant\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude_fixture, "\"type\":\"user\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude_fixture, "\"type\":\"tool_use\",\"id\":\"fixture-claude-tool-1\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude_fixture, "\"tool_use_id\":\"fixture-claude-tool-1\",\"type\":\"tool_result\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude_fixture, "\"type\":\"tool_use\",\"id\":\"fixture-claude-read-1\",\"name\":\"Read\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude_fixture, "\"tool_use_id\":\"fixture-claude-read-1\",\"type\":\"tool_result\",\"content\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude_fixture, "\"tool_use_id\":\"fixture-claude-read-1\",\"type\":\"tool_result\",\"content\":\"PRIVATE_TRANSCRIPT_PROSE_SENTINEL") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude_fixture, "\"tool_use_id\":\"fixture-claude-read-1\",\"type\":\"tool_result\",\"content\":\"PRIVATE_TRANSCRIPT_PROSE_SENTINEL PRIVATE_ARGUMENT_VALUE_SENTINEL PRIVATE_ENTITY_TEXT_SENTINEL /private/raw/path/sentinel\",\"is_error\"") == null);
    try std.testing.expect(std.mem.indexOf(u8, claude_fixture, "\"type\":\"future_record\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude_fixture, "\"content\":{\"type\":\"tool_use\"") != null);
}

test "Codex transcript fixture preserves current rollout envelopes and boundary cases" {
    try expectSanitizedJsonLines(codex_fixture, 7);
    try std.testing.expect(std.mem.indexOf(u8, codex_fixture, "\"type\":\"session_meta\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, codex_fixture, "\"type\":\"function_call\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, codex_fixture, "\"type\":\"function_call_output\",\"call_id\":\"fixture-codex-call-1\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, codex_fixture, "\"type\":\"user_message\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, codex_fixture, "\"type\":\"agent_message\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, codex_fixture, "\"type\":\"future_rollout_item\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, codex_fixture, "\"name\":7") != null);
}

test "Copilot transcript fixture preserves current event envelopes and boundary cases" {
    try expectSanitizedJsonLines(copilot_fixture, 7);
    try std.testing.expect(std.mem.indexOf(u8, copilot_fixture, "\"type\":\"tool.execution_start\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, copilot_fixture, "\"toolCallId\":\"fixture-copilot-tool-1\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, copilot_fixture, "\"type\":\"tool.execution_complete\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, copilot_fixture, "\"type\":\"user.message\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, copilot_fixture, "\"type\":\"assistant.message\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, copilot_fixture, "\"type\":\"future.event\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, copilot_fixture, "\"success\":\"PRIVATE_TRANSCRIPT_PROSE_SENTINEL\"") != null);
}

test "current vendor fixture union stays bounded and private in JSON and text reports" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    const init_out = suite.mustRun(&.{ "init", "--allow-no-repo" });
    defer gpa.free(init_out);

    const claude_path = try std.fs.path.resolve(gpa, &.{"zig/integration_tests/fixtures/introspection_transcripts/claude.jsonl"});
    defer gpa.free(claude_path);
    const codex_path = try std.fs.path.resolve(gpa, &.{"zig/integration_tests/fixtures/introspection_transcripts/codex.jsonl"});
    defer gpa.free(codex_path);
    const copilot_path = try std.fs.path.resolve(gpa, &.{"zig/integration_tests/fixtures/introspection_transcripts/copilot.jsonl"});
    defer gpa.free(copilot_path);
    const config_dir = std.fs.path.dirname(suite.db_path) orelse ".";
    const config_path = try std.fs.path.join(gpa, &.{ config_dir, "introspection-fixture-config.toml" });
    defer gpa.free(config_path);
    const config = try std.fmt.allocPrint(
        gpa,
        "[introspection]\ncli_log = false\n[introspection.transcripts]\nclaude_path = \"{s}\"\ncodex_path = \"{s}\"\ncopilot_path = \"{s}\"\n",
        .{ claude_path, codex_path, copilot_path },
    );
    defer gpa.free(config);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = config_path, .data = config });

    const extra = [_]harness.Suite.ExtraEnvEntry{.{ .key = "PLANAR_CONFIG_PATH", .value = config_path }};
    const json_output = suite.mustRunWith(&.{ "report", "--json" }, &extra);
    defer gpa.free(json_output);
    for (privacy_sentinels) |sentinel| {
        try std.testing.expect(std.mem.indexOf(u8, json_output, sentinel) == null);
    }
    for ([_][]const u8{ claude_path, codex_path, copilot_path }) |path| {
        try std.testing.expect(std.mem.indexOf(u8, json_output, path) == null);
    }

    var parsed = try std.json.parseFromSlice(std.json.Value, gpa, json_output, .{});
    defer parsed.deinit();
    const preview = parsed.value.object.get("introspection_preview").?.object;
    const signals = preview.get("signals").?.array;
    try std.testing.expectEqual(@as(usize, 1), signals.items.len);
    const signal = signals.items[0].object;
    try std.testing.expectEqualStrings("claude", signal.get("vendor").?.string);
    try std.testing.expectEqualStrings("planar task show", signal.get("verb_path").?.string);
    try std.testing.expectEqualStrings("failure", signal.get("category").?.string);

    const coverage = preview.get("coverage").?.array.items;
    try std.testing.expectEqual(@as(usize, 4), coverage.len);
    try expectCoverage(coverage[0].object, "claude", 8, 1, 6, 1);
    try expectCoverage(coverage[1].object, "codex", 7, 0, 6, 1);
    try expectCoverage(coverage[2].object, "copilot", 7, 0, 6, 1);

    const text_output = suite.mustRunWith(&.{"report"}, &extra);
    defer gpa.free(text_output);
    for (privacy_sentinels) |sentinel| {
        try std.testing.expect(std.mem.indexOf(u8, text_output, sentinel) == null);
    }
    for ([_][]const u8{ claude_path, codex_path, copilot_path }) |path| {
        try std.testing.expect(std.mem.indexOf(u8, text_output, path) == null);
    }
    try std.testing.expect(std.mem.indexOf(u8, text_output, "signal claude/planar task show/failure: count=1") != null);
    try std.testing.expect(std.mem.indexOf(u8, text_output, "claude: state=observed scanned=8 normalized=1 ignored=6 malformed=1 capped=0") != null);
    try std.testing.expect(std.mem.indexOf(u8, text_output, "codex: state=observed scanned=7 normalized=0 ignored=6 malformed=1 capped=0") != null);
    try std.testing.expect(std.mem.indexOf(u8, text_output, "copilot: state=observed scanned=7 normalized=0 ignored=6 malformed=1 capped=0") != null);
}

fn expectCoverage(
    coverage: std.json.ObjectMap,
    vendor: []const u8,
    scanned: i64,
    normalized: i64,
    ignored: i64,
    malformed: i64,
) !void {
    try std.testing.expectEqualStrings(vendor, coverage.get("vendor").?.string);
    try std.testing.expectEqual(scanned, coverage.get("scanned").?.integer);
    try std.testing.expectEqual(normalized, coverage.get("normalized").?.integer);
    try std.testing.expectEqual(ignored, coverage.get("ignored").?.integer);
    try std.testing.expectEqual(malformed, coverage.get("malformed").?.integer);
    try std.testing.expectEqual(@as(i64, 0), coverage.get("capped").?.integer);
}
