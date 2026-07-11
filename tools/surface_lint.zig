//! Deterministic semantic validator for authored agent, skill, and doc Markdown.
//! Usage: surface_lint <repo-root> [--json] [--require-feedback-contract]

const std = @import("std");
const Io = std.Io;

const scan_dirs = [_][]const u8{ "agents", "skills/src", "docs" };
const Code = struct {
    const link = "surface-link-missing";
    const legacy = "surface-legacy-reference";
    const artifacts = "surface-artifact-set-drift";
    const capability = "surface-capability-drift";
    const command = "surface-command-drift";
    const contract = "surface-contract-missing";
    const suppression_invalid = "surface-suppression-invalid";
    const suppression_unused = "surface-suppression-unused";
};
const suppressible_codes = [_][]const u8{ Code.link, Code.legacy, Code.artifacts, Code.capability, Code.command, Code.contract };

const Finding = struct { code: []const u8, file: []const u8, line: usize, message: []const u8 };
const Suppression = struct { code: []const u8, declaration_line: usize, target_line: usize, used: bool = false };
const Options = struct { require_feedback_contract: bool = false };
const Result = struct { findings: std.ArrayList(Finding) = .empty, files_scanned: usize = 0 };

pub fn main(init: std.process.Init) !void {
    const arena = init.arena.allocator();
    const io = init.io;
    const args = try init.minimal.args.toSlice(arena);
    if (args.len < 2) usage(io);
    var root: ?[]const u8 = null;
    var json = false;
    var opts = Options{};
    for (args[1..]) |arg| {
        if (std.mem.eql(u8, arg, "--json")) json = true else if (std.mem.eql(u8, arg, "--require-feedback-contract"))
            opts.require_feedback_contract = true
        else if (std.mem.startsWith(u8, arg, "-") or root != null)
            usage(io)
        else
            root = arg;
    }
    var result = scanRepository(arena, io, root orelse usage(io), opts) catch |err| {
        const msg = try std.fmt.allocPrint(arena, "surface-lint: internal error: {s}\n", .{@errorName(err)});
        Io.File.stderr().writeStreamingAll(io, msg) catch {};
        std.process.exit(2);
    };
    std.mem.sort(Finding, result.findings.items, {}, findingLessThan);
    var output: std.ArrayList(u8) = .empty;
    if (json) try writeJson(arena, &output, &result) else try writeText(arena, &output, &result);
    try Io.File.stdout().writeStreamingAll(io, output.items);
    if (result.findings.items.len != 0) std.process.exit(1);
}

fn usage(io: Io) noreturn {
    Io.File.stderr().writeStreamingAll(io, "usage: surface_lint <repo-root> [--json] [--require-feedback-contract]\n") catch {};
    std.process.exit(2);
}

fn scanRepository(arena: std.mem.Allocator, io: Io, root: []const u8, opts: Options) !Result {
    var result = Result{};
    var paths: std.ArrayList([]const u8) = .empty;
    for (scan_dirs) |rel| {
        const dir = try std.fs.path.join(arena, &.{ root, rel });
        collectMarkdown(arena, io, dir, &paths) catch |err| switch (err) {
            error.FileNotFound => {},
            else => return err,
        };
    }
    std.mem.sort([]const u8, paths.items, {}, stringLessThan);
    for (paths.items) |path| {
        const content = try Io.Dir.cwd().readFileAlloc(io, path, arena, .unlimited);
        try scanFile(arena, io, root, relativePath(path, root), path, content, opts, &result.findings);
        result.files_scanned += 1;
    }
    return result;
}

fn collectMarkdown(arena: std.mem.Allocator, io: Io, path: []const u8, paths: *std.ArrayList([]const u8)) !void {
    var dir = try Io.Dir.cwd().openDir(io, path, .{ .iterate = true });
    defer dir.close(io);
    var it = dir.iterate();
    while (try it.next(io)) |entry| {
        const child = try std.fs.path.join(arena, &.{ path, entry.name });
        switch (entry.kind) {
            .directory => try collectMarkdown(arena, io, child, paths),
            .file => if (std.mem.endsWith(u8, entry.name, ".md")) try paths.append(arena, child),
            else => {},
        }
    }
}

fn scanFile(arena: std.mem.Allocator, io: Io, root: []const u8, rel_file: []const u8, abs_file: []const u8, content: []const u8, opts: Options, findings: *std.ArrayList(Finding)) !void {
    var lines: std.ArrayList([]const u8) = .empty;
    var split = std.mem.splitScalar(u8, content, '\n');
    while (split.next()) |line| try lines.append(arena, line);
    var suppressions: std.ArrayList(Suppression) = .empty;
    try parseSuppressions(arena, rel_file, lines.items, findings, &suppressions);
    const read_only = std.mem.eql(u8, frontmatterValue(content, "capability") orelse "", "read-only");
    const role = frontmatterValue(content, "role") orelse "";
    var in_fence = false;
    for (lines.items, 0..) |line, idx| {
        const line_no = idx + 1;
        const trimmed = std.mem.trim(u8, line, " \t\r");
        if (std.mem.startsWith(u8, trimmed, "```") or std.mem.startsWith(u8, trimmed, "~~~")) {
            in_fence = !in_fence;
            continue;
        }
        try checkLinks(arena, io, root, rel_file, abs_file, line_no, line, findings, &suppressions);
        try checkLegacy(arena, rel_file, line_no, line, findings, &suppressions);
        try checkArtifactSet(arena, rel_file, line_no, line, findings, &suppressions);
        if (in_fence and read_only) try checkCapability(arena, rel_file, role, line_no, line, findings, &suppressions);
        try checkCommand(arena, rel_file, line_no, line, findings, &suppressions);
    }
    if (opts.require_feedback_contract and std.mem.startsWith(u8, rel_file, "skills/src/") and
        !std.mem.eql(u8, frontmatterValue(content, "internal_only") orelse "", "true"))
        try checkFeedbackContract(arena, rel_file, content, findings, &suppressions);
    for (suppressions.items) |s| if (!s.used) try findings.append(arena, .{
        .code = Code.suppression_unused,
        .file = rel_file,
        .line = s.declaration_line,
        .message = try std.fmt.allocPrint(arena, "suppression for {s} does not match a finding on the next non-blank line", .{s.code}),
    });
}

fn parseSuppressions(arena: std.mem.Allocator, file: []const u8, lines: []const []const u8, findings: *std.ArrayList(Finding), suppressions: *std.ArrayList(Suppression)) !void {
    const prefix = "<!-- surface-lint-ignore ";
    var in_fence = false;
    for (lines, 0..) |raw, idx| {
        const line = std.mem.trim(u8, raw, " \t\r");
        if (std.mem.startsWith(u8, line, "```") or std.mem.startsWith(u8, line, "~~~")) {
            in_fence = !in_fence;
            continue;
        }
        if (in_fence) continue;
        if (std.mem.indexOf(u8, line, "surface-lint-ignore-file") != null) {
            try invalidSuppression(arena, findings, file, idx + 1, "file-wide suppressions are not allowed");
            continue;
        }
        if (std.mem.indexOf(u8, line, "surface-lint-ignore") == null) continue;
        if (!std.mem.startsWith(u8, line, prefix) or !std.mem.endsWith(u8, line, "-->")) {
            try invalidSuppression(arena, findings, file, idx + 1, "expected `<!-- surface-lint-ignore <code>: <rationale> -->`");
            continue;
        }
        const body = std.mem.trim(u8, line[prefix.len .. line.len - 3], " \t");
        const colon = std.mem.indexOfScalar(u8, body, ':') orelse {
            try invalidSuppression(arena, findings, file, idx + 1, "suppression rationale is required after `:`");
            continue;
        };
        const code = std.mem.trim(u8, body[0..colon], " \t");
        const rationale = std.mem.trim(u8, body[colon + 1 ..], " \t");
        if (!isSuppressible(code)) {
            try invalidSuppression(arena, findings, file, idx + 1, "suppression names an unknown finding code");
            continue;
        }
        if (rationale.len == 0) {
            try invalidSuppression(arena, findings, file, idx + 1, "suppression rationale must be non-empty");
            continue;
        }
        var target = idx + 1;
        while (target < lines.len and std.mem.trim(u8, lines[target], " \t\r").len == 0) : (target += 1) {}
        if (target == lines.len) {
            try invalidSuppression(arena, findings, file, idx + 1, "suppression has no following non-blank line");
            continue;
        }
        try suppressions.append(arena, .{ .code = try arena.dupe(u8, code), .declaration_line = idx + 1, .target_line = target + 1 });
    }
}

fn invalidSuppression(arena: std.mem.Allocator, findings: *std.ArrayList(Finding), file: []const u8, line: usize, message: []const u8) !void {
    try findings.append(arena, .{ .code = Code.suppression_invalid, .file = file, .line = line, .message = message });
}

fn checkLinks(arena: std.mem.Allocator, io: Io, root: []const u8, file: []const u8, abs_file: []const u8, line_no: usize, line: []const u8, findings: *std.ArrayList(Finding), suppressions: *std.ArrayList(Suppression)) !void {
    var cursor: usize = 0;
    while (std.mem.indexOfPos(u8, line, cursor, "](")) |open| {
        const start = open + 2;
        const close = std.mem.indexOfScalarPos(u8, line, start, ')') orelse break;
        cursor = close + 1;
        var target = std.mem.trim(u8, line[start..close], " \t");
        if (target.len == 0 or target[0] == '#' or std.mem.indexOf(u8, target, "://") != null or std.mem.startsWith(u8, target, "mailto:") or std.mem.indexOfAny(u8, target, "{}") != null) continue;
        if (std.mem.indexOfAny(u8, target, " \t")) |space| target = target[0..space];
        if (std.mem.indexOfScalar(u8, target, '#')) |hash| target = target[0..hash];
        if (std.mem.indexOfScalar(u8, target, '?')) |query| target = target[0..query];
        if (target.len == 0) continue;
        // Generated vendor projections are absent from the source tree and
        // are validated by renderer fixtures instead.
        if (std.mem.indexOf(u8, target, "commands/claude/") != null or
            std.mem.indexOf(u8, target, "skills/codex/") != null or
            std.mem.indexOf(u8, target, "skills/copilot/") != null) continue;
        const resolved = if (std.fs.path.isAbsolute(target)) try std.fs.path.join(arena, &.{ root, target[1..] }) else try std.fs.path.join(arena, &.{ std.fs.path.dirname(abs_file) orelse root, target });
        Io.Dir.cwd().access(io, resolved, .{}) catch try emit(arena, findings, suppressions, Code.link, file, line_no, try std.fmt.allocPrint(arena, "repository-relative link target does not exist: {s}", .{target}));
    }
}

const retired_patterns = [_][]const u8{ "src/internal/", "harness Agent/Task tool", "harness Agent tool", "Go side", "Phase 5.5" };
fn checkLegacy(arena: std.mem.Allocator, file: []const u8, line_no: usize, line: []const u8, findings: *std.ArrayList(Finding), suppressions: *std.ArrayList(Suppression)) !void {
    for (retired_patterns) |pattern| if (std.mem.indexOf(u8, line, pattern) != null and
        (!std.mem.eql(u8, pattern, "src/internal/") or std.mem.indexOf(u8, line, ".go") != null))
    {
        try emit(arena, findings, suppressions, Code.legacy, file, line_no, try std.fmt.allocPrint(arena, "retired authored-surface reference: {s}", .{pattern}));
        return;
    };
}

fn checkArtifactSet(arena: std.mem.Allocator, file: []const u8, line_no: usize, line: []const u8, findings: *std.ArrayList(Finding), suppressions: *std.ArrayList(Suppression)) !void {
    if (std.mem.indexOf(u8, line, ".md") == null) return;
    const contract_statement = std.mem.indexOf(u8, line, "Create ") != null or
        std.mem.indexOf(u8, line, "Creates ") != null or
        std.mem.indexOf(u8, line, "creates ") != null or
        std.mem.indexOf(u8, line, "registers ") != null or
        std.mem.indexOf(u8, line, "produces ") != null or
        std.mem.indexOf(u8, line, "For each of ") != null or
        std.mem.indexOf(u8, line, "Repeat for each ") != null;
    if (!contract_statement) return;
    const names = [_][]const u8{ "product-spec", "tech-spec", "roadmap", "test-spec" };
    var present: usize = 0;
    for (names) |name| if (std.mem.indexOf(u8, line, name) != null) {
        present += 1;
    };
    if (present >= 3 and present != names.len) try emit(arena, findings, suppressions, Code.artifacts, file, line_no, "planning artifact set must contain product-spec, tech-spec, roadmap, and test-spec");
}

const write_shapes = [_][]const u8{ "planar plan create", "planar task add", "planar question add", "planar decision add", "planar artifact create", "planar annotation add", "planar links add", "planar-agent complete", "planar-agent fail", "planar-agent release", "planar-agent block" };
fn checkCapability(arena: std.mem.Allocator, file: []const u8, role: []const u8, line_no: usize, line: []const u8, findings: *std.ArrayList(Finding), suppressions: *std.ArrayList(Suppression)) !void {
    for (write_shapes) |shape| if (std.mem.indexOf(u8, line, shape) != null) {
        if (std.mem.eql(u8, role, "introspector") and (std.mem.eql(u8, shape, "planar plan create") or std.mem.eql(u8, shape, "planar task add") or std.mem.eql(u8, shape, "planar question add"))) return;
        try emit(arena, findings, suppressions, Code.capability, file, line_no, try std.fmt.allocPrint(arena, "read-only role contains coordination or entity write: {s}", .{shape}));
        return;
    };
}

const invalid_commands = [_][]const u8{
    "planar audit trail task:",
    "planar audit trail question:",
    "planar audit trail plan:",
    "planar audit trail task ",
    "planar audit trail question ",
    "planar audit trail plan ",
    "planar audit trail <kind:id>",
};
fn checkCommand(arena: std.mem.Allocator, file: []const u8, line_no: usize, line: []const u8, findings: *std.ArrayList(Finding), suppressions: *std.ArrayList(Suppression)) !void {
    for (invalid_commands) |shape| if (std.mem.indexOf(u8, line, shape) != null) {
        try emit(arena, findings, suppressions, Code.command, file, line_no, "audit trail entity kind must use `--kind <kind> <entity-id>`");
        return;
    };
}

fn checkFeedbackContract(arena: std.mem.Allocator, file: []const u8, content: []const u8, findings: *std.ArrayList(Finding), suppressions: *std.ArrayList(Suppression)) !void {
    const required = [_][]const u8{ "## Context", "## Intent", "## Actions", "## Result", "## Warnings", "## Next actions", "## Recovery" };
    for (required) |heading| if (std.mem.indexOf(u8, content, heading) == null) try emit(arena, findings, suppressions, Code.contract, file, 1, try std.fmt.allocPrint(arena, "user-invocable skill is missing required feedback heading: {s}", .{heading}));
}

fn emit(arena: std.mem.Allocator, findings: *std.ArrayList(Finding), suppressions: *std.ArrayList(Suppression), code: []const u8, file: []const u8, line: usize, message: []const u8) !void {
    for (suppressions.items) |*s| if (!s.used and s.target_line == line and std.mem.eql(u8, s.code, code)) {
        s.used = true;
        return;
    };
    try findings.append(arena, .{ .code = code, .file = file, .line = line, .message = message });
}

fn frontmatterValue(content: []const u8, key: []const u8) ?[]const u8 {
    if (!std.mem.startsWith(u8, content, "---\n")) return null;
    var lines = std.mem.splitScalar(u8, content[4..], '\n');
    while (lines.next()) |line| {
        if (std.mem.eql(u8, std.mem.trim(u8, line, " \t\r"), "---")) break;
        const colon = std.mem.indexOfScalar(u8, line, ':') orelse continue;
        if (std.mem.eql(u8, std.mem.trim(u8, line[0..colon], " \t"), key)) return std.mem.trim(u8, line[colon + 1 ..], " \t\r\"");
    }
    return null;
}
fn isSuppressible(code: []const u8) bool {
    for (suppressible_codes) |known| if (std.mem.eql(u8, code, known)) return true;
    return false;
}
fn relativePath(path: []const u8, root: []const u8) []const u8 {
    if (std.mem.startsWith(u8, path, root)) return std.mem.trimStart(u8, path[root.len..], "/");
    return path;
}
fn stringLessThan(_: void, a: []const u8, b: []const u8) bool {
    return std.mem.lessThan(u8, a, b);
}
fn findingLessThan(_: void, a: Finding, b: Finding) bool {
    const f = std.mem.order(u8, a.file, b.file);
    if (f != .eq) return f == .lt;
    if (a.line != b.line) return a.line < b.line;
    const c = std.mem.order(u8, a.code, b.code);
    if (c != .eq) return c == .lt;
    return std.mem.lessThan(u8, a.message, b.message);
}

fn writeText(arena: std.mem.Allocator, out: *std.ArrayList(u8), result: *const Result) !void {
    for (result.findings.items) |f| try out.print(arena, "{s}:{d}: {s}: {s}\n", .{ f.file, f.line, f.code, f.message });
    if (result.findings.items.len == 0) try out.print(arena, "surface-lint: clean ({d} files)\n", .{result.files_scanned}) else try out.print(arena, "surface-lint: {d} finding(s) across {d} files\n", .{ result.findings.items.len, result.files_scanned });
}
fn writeJson(arena: std.mem.Allocator, out: *std.ArrayList(u8), result: *const Result) !void {
    try out.print(arena, "{{\"version\":1,\"ok\":{s},\"files_scanned\":{d},\"findings\":[", .{ if (result.findings.items.len == 0) "true" else "false", result.files_scanned });
    for (result.findings.items, 0..) |f, idx| {
        if (idx != 0) try out.append(arena, ',');
        try out.appendSlice(arena, "{\"code\":");
        try appendJsonString(arena, out, f.code);
        try out.appendSlice(arena, ",\"file\":");
        try appendJsonString(arena, out, f.file);
        try out.print(arena, ",\"line\":{d},\"message\":", .{f.line});
        try appendJsonString(arena, out, f.message);
        try out.append(arena, '}');
    }
    try out.appendSlice(arena, "]}\n");
}
fn appendJsonString(arena: std.mem.Allocator, out: *std.ArrayList(u8), value: []const u8) !void {
    try out.append(arena, '"');
    for (value) |c| switch (c) {
        '"' => try out.appendSlice(arena, "\\\""),
        '\\' => try out.appendSlice(arena, "\\\\"),
        '\n' => try out.appendSlice(arena, "\\n"),
        '\r' => try out.appendSlice(arena, "\\r"),
        '\t' => try out.appendSlice(arena, "\\t"),
        else => if (c < 0x20) try out.print(arena, "\\u00{x:0>2}", .{c}) else try out.append(arena, c),
    };
    try out.append(arena, '"');
}

const testing = std.testing;
const FixtureResult = struct {
    arena: std.heap.ArenaAllocator,
    result: Result,

    fn deinit(self: *FixtureResult) void {
        self.arena.deinit();
    }
};
fn scanFixture(content: []const u8, opts: Options) !FixtureResult {
    var fixture = FixtureResult{
        .arena = std.heap.ArenaAllocator.init(testing.allocator),
        .result = .{ .files_scanned = 1 },
    };
    errdefer fixture.deinit();
    const arena = fixture.arena.allocator();
    try scanFile(arena, std.testing.io, ".", "skills/src/fixture.md", "skills/src/fixture.md", content, opts, &fixture.result.findings);
    std.mem.sort(Finding, fixture.result.findings.items, {}, findingLessThan);
    return fixture;
}

test "semantic drift fixtures emit stable codes and line evidence" {
    const cases = [_]struct { content: []const u8, code: []const u8, line: usize }{
        .{ .content = "[gone](missing-target.md)\n", .code = Code.link, .line = 1 },
        .{ .content = "Use `src/internal/workbench/parse.go`.\n", .code = Code.legacy, .line = 1 },
        .{ .content = "Create product-spec.md, tech-spec.md, and roadmap.md.\n", .code = Code.artifacts, .line = 1 },
        .{ .content = "---\nrole: fixture\ncapability: read-only\n---\n```\nplanar task add \"x\"\n```\n", .code = Code.capability, .line = 6 },
        .{ .content = "Run `planar audit trail task:42`.\n", .code = Code.command, .line = 1 },
        .{ .content = "---\nslug: fixture\n---\n# Fixture\n", .code = Code.contract, .line = 1 },
    };
    for (cases) |case| {
        var fixture = try scanFixture(case.content, .{ .require_feedback_contract = std.mem.eql(u8, case.code, Code.contract) });
        defer fixture.deinit();
        try testing.expect(fixture.result.findings.items.len >= 1);
        try testing.expectEqualStrings(case.code, fixture.result.findings.items[0].code);
        try testing.expectEqual(case.line, fixture.result.findings.items[0].line);
    }
}
test "clean fixture and valid next-line suppression pass" {
    var fixture = try scanFixture("<!-- surface-lint-ignore surface-legacy-reference: historical comparison required -->\n\nHistorical `src/internal/foo.go` reference.\n", .{});
    defer fixture.deinit();
    try testing.expectEqual(@as(usize, 0), fixture.result.findings.items.len);
}
test "clean authored contract passes every semantic rule" {
    const content =
        "---\nslug: fixture\n---\n" ++
        "# Fixture\n\n" ++
        "Create product-spec.md, tech-spec.md, roadmap.md, and test-spec.md.\n" ++
        "Run `planar audit trail --kind task 42`.\n\n" ++
        "## Context\n## Intent\n## Actions\n## Result\n## Warnings\n## Next actions\n## Recovery\n";
    var fixture = try scanFixture(content, .{ .require_feedback_contract = true });
    defer fixture.deinit();
    try testing.expectEqual(@as(usize, 0), fixture.result.findings.items.len);
}
test "suppression syntax shown inside a fence is illustrative" {
    var fixture = try scanFixture("```html\n<!-- surface-lint-ignore surface-legacy-reference: example -->\n```\n", .{});
    defer fixture.deinit();
    try testing.expectEqual(@as(usize, 0), fixture.result.findings.items.len);
}
test "narrow introspector write exemption and internal-only contract exemption pass" {
    const agent = "---\nrole: introspector\ncapability: read-only\n---\n```\nplanar task add \"bounded finding\"\n```\n";
    var agent_fixture = try scanFixture(agent, .{});
    defer agent_fixture.deinit();
    try testing.expectEqual(@as(usize, 0), agent_fixture.result.findings.items.len);

    const skill = "---\nslug: fixture\ninternal_only: true\n---\n# Internal\n";
    var skill_fixture = try scanFixture(skill, .{ .require_feedback_contract = true });
    defer skill_fixture.deinit();
    try testing.expectEqual(@as(usize, 0), skill_fixture.result.findings.items.len);
}
test "suppression edge cases fail" {
    const cases = [_][]const u8{
        "<!-- surface-lint-ignore surface-command-drift: wrong code -->\n`src/internal/foo.go`\n",
        "<!-- surface-lint-ignore surface-legacy-reference: -->\n`src/internal/foo.go`\n",
        "<!-- surface-lint-ignore surface-not-real: reason -->\ntext\n",
        "<!-- surface-lint-ignore-file surface-legacy-reference: reason -->\n",
        "<!-- surface-lint-ignore surface-legacy-reference: unused -->\nclean line\n",
    };
    for (cases) |content| {
        var fixture = try scanFixture(content, .{});
        defer fixture.deinit();
        try testing.expect(fixture.result.findings.items.len != 0);
    }
}
test "JSON output shape and finding order are stable" {
    var result = Result{ .files_scanned = 2 };
    defer result.findings.deinit(testing.allocator);
    try result.findings.append(testing.allocator, .{ .code = Code.command, .file = "z.md", .line = 8, .message = "z" });
    try result.findings.append(testing.allocator, .{ .code = Code.link, .file = "a.md", .line = 2, .message = "a" });
    std.mem.sort(Finding, result.findings.items, {}, findingLessThan);
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(testing.allocator);
    try writeJson(testing.allocator, &out, &result);
    try testing.expectEqualStrings("{\"version\":1,\"ok\":false,\"files_scanned\":2,\"findings\":[{\"code\":\"surface-link-missing\",\"file\":\"a.md\",\"line\":2,\"message\":\"a\"},{\"code\":\"surface-command-drift\",\"file\":\"z.md\",\"line\":8,\"message\":\"z\"}]}\n", out.items);
}
