//! Deterministic semantic validator for authored agent, skill, and doc Markdown.
//! Usage: surface_lint <repo-root> [--json]
//!        surface_lint --command-inventory-json

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
const Options = struct {};
const Result = struct { findings: std.ArrayList(Finding) = .empty, files_scanned: usize = 0 };

pub fn main(init: std.process.Init) !void {
    const arena = init.arena.allocator();
    const io = init.io;
    const args = try init.minimal.args.toSlice(arena);
    if (args.len == 2 and std.mem.eql(u8, args[1], "--command-inventory-json")) {
        var output: std.ArrayList(u8) = .empty;
        try writeCommandInventory(arena, &output);
        try Io.File.stdout().writeStreamingAll(io, output.items);
        return;
    }
    if (args.len < 2) usage(io);
    var root: ?[]const u8 = null;
    var json = false;
    const opts = Options{};
    for (args[1..]) |arg| {
        if (std.mem.eql(u8, arg, "--json")) json = true else if (std.mem.startsWith(u8, arg, "-") or root != null)
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
    Io.File.stderr().writeStreamingAll(io, "usage: surface_lint <repo-root> [--json]\n       surface_lint --command-inventory-json\n") catch {};
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
        const rel_path = relativePath(path, root);
        if (!isAuthoredSurface(rel_path)) continue;
        const content = try Io.Dir.cwd().readFileAlloc(io, path, arena, .unlimited);
        try scanFile(arena, io, root, rel_path, path, content, opts, &result.findings);
        result.files_scanned += 1;
    }
    return result;
}

fn isAuthoredSurface(path: []const u8) bool {
    if (!std.mem.startsWith(u8, path, "agents/")) return true;
    return !std.mem.startsWith(u8, path, "agents/claude/") and
        !std.mem.startsWith(u8, path, "agents/codex/") and
        !std.mem.startsWith(u8, path, "agents/copilot/");
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

fn scanFile(arena: std.mem.Allocator, io: Io, root: []const u8, rel_file: []const u8, abs_file: []const u8, content: []const u8, _: Options, findings: *std.ArrayList(Finding)) !void {
    var lines: std.ArrayList([]const u8) = .empty;
    var split = std.mem.splitScalar(u8, content, '\n');
    while (split.next()) |line| try lines.append(arena, line);
    var suppressions: std.ArrayList(Suppression) = .empty;
    try parseSuppressions(arena, rel_file, lines.items, findings, &suppressions);
    const read_only = std.mem.eql(u8, frontmatterValue(content, "capability") orelse "", "read-only");
    const role = frontmatterValue(content, "role") orelse "";
    var fence: ?Fence = null;
    for (lines.items, 0..) |line, idx| {
        const line_no = idx + 1;
        const trimmed = std.mem.trim(u8, line, " \t\r");
        if (advanceFence(trimmed, &fence)) {
            continue;
        }
        try checkLinks(arena, io, root, rel_file, abs_file, line_no, line, findings, &suppressions);
        try checkLegacy(arena, rel_file, line_no, line, findings, &suppressions);
        try checkArtifactSet(arena, rel_file, line_no, line, findings, &suppressions);
        if (read_only) try checkCapability(arena, rel_file, role, line_no, line, fence != null, findings, &suppressions);
        try checkCommand(arena, rel_file, line_no, line, findings, &suppressions);
        try checkDeferredCommand(arena, rel_file, line_no, line, fence != null, findings, &suppressions);
    }
    if (std.mem.startsWith(u8, rel_file, "skills/src/") and !frontmatterLiteralTrue(content, "internal_only"))
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
    var fence: ?Fence = null;
    for (lines, 0..) |raw, idx| {
        const line = std.mem.trim(u8, raw, " \t\r");
        if (advanceFence(line, &fence)) continue;
        if (fence != null) continue;
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

const Fence = struct {
    marker: u8,
    run_len: usize,
};

/// Update Markdown fenced-code state. An open fence closes only when the
/// marker character matches and the closing run is at least as long as the
/// opener. A different marker or a shorter run is content inside the fence.
fn advanceFence(trimmed: []const u8, state: *?Fence) bool {
    if (trimmed.len < 3 or (trimmed[0] != '`' and trimmed[0] != '~')) return false;
    const marker = trimmed[0];
    var run_len: usize = 1;
    while (run_len < trimmed.len and trimmed[run_len] == marker) : (run_len += 1) {}
    if (run_len < 3) return false;

    if (state.*) |open| {
        if (marker != open.marker or run_len < open.run_len) return false;
        if (std.mem.trim(u8, trimmed[run_len..], " \t\r").len != 0) return false;
        state.* = null;
        return true;
    }

    state.* = .{ .marker = marker, .run_len = run_len };
    return true;
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
        // These two documentation links intentionally name install-time
        // projections. Renderer integration tests resolve them against an
        // out-of-tree render. No directory-substring exemption is allowed.
        if (isProjectionOnlyLink(file, target)) continue;
        const resolved = if (std.fs.path.isAbsolute(target)) try std.fs.path.join(arena, &.{ root, target[1..] }) else try std.fs.path.join(arena, &.{ std.fs.path.dirname(abs_file) orelse root, target });
        Io.Dir.cwd().access(io, resolved, .{}) catch try emit(arena, findings, suppressions, Code.link, file, line_no, try std.fmt.allocPrint(arena, "repository-relative link target does not exist: {s}", .{target}));
    }
}

const projection_links = [_]struct { file: []const u8, target: []const u8 }{
    .{ .file = "docs/cli-reference.md", .target = "../commands/claude/pl-synthesize.md" },
    .{ .file = "docs/workflows.md", .target = "../commands/claude/pl-workspace-scan.md" },
};
fn isProjectionOnlyLink(file: []const u8, target: []const u8) bool {
    for (projection_links) |link| if (std.mem.eql(u8, file, link.file) and std.mem.eql(u8, target, link.target)) return true;
    return false;
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

const Access = enum { read, mutate };
const CommandClass = struct { shape: []const u8, access: Access };
fn r(comptime shape: []const u8) CommandClass {
    return .{ .shape = shape, .access = .read };
}
fn m(comptime shape: []const u8) CommandClass {
    return .{ .shape = shape, .access = .mutate };
}

// Exhaustive leaf classification from the four schema catalogs. A leaf is
// `mutate` when any supported mode writes Planar state, coordination state,
// the filesystem, or a remote system. Reads are explicit so a missing write
// entry cannot silently become an implicit read.
const command_classes = [_]CommandClass{
    m("planar init"),                          r("planar scope show"),                  r("planar scope suggest"),         r("planar scope use"),              r("planar scope pop"),               r("planar scope clear"),
    r("planar assoc list"),                    m("planar assoc create"),                m("planar assoc add"),             m("planar assoc remove"),           r("planar assoc members"),           m("planar assoc detect"),
    m("planar plan create"),                   r("planar plan show"),                   r("planar plan list"),             m("planar plan update"),            m("planar plan edit"),               r("planar plan view"),
    r("planar plan diff"),                     r("planar plan review"),                 m("planar plan link"),             r("planar plan next"),              r("planar plan recommend-strategy"), r("planar plan divergence"),
    m("planar plan recompute-status"),         m("planar plan closeout"),               m("planar plan step add"),         r("planar plan step list"),         m("planar plan step done"),          m("planar plan step skip"),
    m("planar plan step link"),                r("planar plan descendants"),            m("planar task add"),              r("planar task show"),              r("planar task list"),               m("planar task update"),
    m("planar task edit"),                     r("planar task view"),                   r("planar task diff"),             r("planar task review"),            m("planar task done"),               m("planar task cancel"),
    m("planar task block"),                    m("planar task link"),                   m("planar task reopen"),           m("planar task touches add"),       r("planar task touches list"),       m("planar task touches remove"),
    m("planar question add"),                  m("planar question edit"),               r("planar question view"),         r("planar question diff"),          r("planar question review"),         m("planar question answer"),
    m("planar question wontfix"),              r("planar question list"),               r("planar question show"),         m("planar question link"),          m("planar scenario add"),            m("planar scenario edit"),
    r("planar scenario view"),                 r("planar scenario diff"),               r("planar scenario review"),       m("planar scenario verify"),        m("planar scenario retire"),         r("planar scenario list"),
    r("planar scenario show"),                 m("planar scenario link"),               m("planar decision add"),          r("planar decision show"),          r("planar decision list"),           m("planar decision accept"),
    m("planar decision supersede"),            m("planar decision withdraw"),           m("planar decision edit"),         r("planar decision view"),          r("planar decision diff"),           r("planar decision review"),
    m("planar decision link"),                 m("planar artifact add"),                r("planar artifact show"),         r("planar artifact list"),          m("planar artifact update"),         m("planar artifact edit"),
    r("planar artifact view"),                 r("planar artifact diff"),               r("planar artifact review"),       m("planar artifact link"),          m("planar annotate add"),            r("planar annotate show"),
    r("planar annotate list"),                 m("planar annotate update"),             m("planar annotate remove"),       m("planar annotate tag"),           m("planar annotate resolve"),        m("planar annotate dismiss"),
    m("planar annotate archive"),              m("planar annotate bulk-resolve"),       m("planar annotate bulk-dismiss"), m("planar annotate bulk-archive"),  r("planar annotate verify"),         m("planar annotate sweep"),
    m("planar promote"),                       m("planar demote"),                      r("planar workbench lint"),        m("planar workbench pull"),         m("planar workbench push"),          r("planar workbench status"),
    m("planar workbench resolve"),             m("planar workbench sync"),              m("planar workbench archive"),     m("planar workbench restore"),      m("planar workbench gc"),            r("planar workbench list"),
    m("planar workbench publish"),             r("planar workbench extract-questions"), m("planar workbench edit"),        m("planar workspace init"),         m("planar workspace doctor"),        m("planar workspace routing build"),
    r("planar workspace routing show"),        m("planar workspace regenerate"),        m("planar ext register jira"),     m("planar ext register github"),    r("planar ext list"),                r("planar ext test"),
    m("planar ext create"),                    m("planar ext propagate-one"),           m("planar ext propagate"),         m("planar link"),                   m("planar unlink"),                  m("planar links add"),
    r("planar links list"),                    m("planar links remove"),                r("planar links trail"),           m("planar sync pull"),              m("planar sync push"),               r("planar sync status"),
    m("planar sync resolve"),                  r("planar resume validate"),             m("planar handoff create"),        m("planar handoff validate"),       m("planar handoff consume"),         m("planar handoff abandon"),
    r("planar handoff list"),                  r("planar handoff show"),                m("planar capture session"),       m("planar capture commits"),        m("planar capture end"),             m("planar capture note"),
    m("planar capture command"),               m("planar capture file"),                m("planar capture snapshot"),      r("planar audit trail"),            r("planar audit commits"),           r("planar audit session"),
    m("planar audit publish-decision"),        r("planar audit handoff-readiness"),     r("planar health hygiene"),        r("planar models list"),            m("planar models refresh"),          r("planar models routing"),
    m("planar models apply"),                  r("planar models candidates"),           r("planar models evals"),          m("planar models sync-doc"),        r("planar models registry list"),    m("planar models registry add"),
    m("planar models registry update"),        m("planar models registry remove"),      m("planar models registry bind"),  m("planar models registry unbind"), m("planar models registry observe"), r("planar models registry eligibility"),
    m("planar models registry import-legacy"), r("planar models registry export"),      r("planar dashboard"),             m("planar spec ingest"),            r("planar test-spec status"),        r("planar config show"),
    m("planar config edit"),                   r("planar config validate"),             m("planar config init"),           r("planar config path"),            r("planar templates list"),          r("planar templates show"),
    r("planar templates render"),              r("planar templates validate"),          m("planar templates init"),        r("planar templates path"),         r("planar tree"),                    r("planar search"),
    r("planar local list"),                    m("planar local link"),                  m("planar local unlink"),          m("planar local import"),           m("planar local migrate"),
    // `planar skills` is a childless placeholder (plan 918 M5 retired its
    // render/status/repair subcommands); bare invocation only prints help.
              r("planar skills"),
    m("planar import"),                        m("planar synthesize"),                  r("planar version"),               r("planar completion"),             r("planar schema"),                  r("planar report"),
    m("planar bench start"),                   m("planar bench event"),                 m("planar bench touch"),           m("planar bench harvest"),          m("planar bench finish"),            r("planar bench show"),
    m("planar closure compute"),               r("planar closure show"),                m("planar run start"),             m("planar run event"),              m("planar run finish"),              r("planar run show"),
    r("planar groups recommend"),              m("planar explore"),                     r("planar workflow list"),         r("planar workflow show"),          m("planar workflow run"),            r("planar feedback triage list"),
    r("planar feedback triage show"),          m("planar feedback triage set"),         r("planar-agent version"),         m("planar-agent pull"),             r("planar-agent peek"),              m("planar-agent complete"),
    m("planar-agent fail"),                    m("planar-agent release"),               m("planar-agent block"),           m("planar-agent claim"),            m("planar-agent heartbeat"),         m("planar-agent claim-associate"),
    m("planar-agent action start"),            m("planar-agent action end"),            m("planar-agent ingest"),          m("planar-agent reconcile"),        m("planar-agent abort"),             r("planar-agent schema"),
    m("planar-agent run start"),               m("planar-agent run end"),               m("planar-agent context add"),     m("planar-agent context capsule"),  r("planar-agent context list"),      m("planar-agent context resolve"),
    r("planar-watch feed"),                    r("planar-watch ps"),                    r("planar-watch claims"),          r("planar-watch actions"),          r("planar-watch plans"),             r("planar-watch log"),
    r("planar-watch tree"),                    r("planar-watch run list"),              r("planar-watch run show"),        r("planar-watch sync-events"),      r("planar-watch version"),           r("planar-watch completion"),
    r("planar-watch schema"),
};

fn checkCapability(arena: std.mem.Allocator, file: []const u8, role: []const u8, line_no: usize, line: []const u8, in_fence: bool, findings: *std.ArrayList(Finding), suppressions: *std.ArrayList(Suppression)) !void {
    if (in_fence) return checkExecutableCapability(arena, file, role, line_no, line, findings, suppressions);
    var cursor: usize = 0;
    while (std.mem.indexOfScalarPos(u8, line, cursor, '`')) |open| {
        const run_len = markerRunLength(line, open, '`');
        var search = open + run_len;
        var close: ?usize = null;
        while (std.mem.indexOfScalarPos(u8, line, search, '`')) |candidate| {
            const candidate_len = markerRunLength(line, candidate, '`');
            if (candidate_len == run_len) {
                close = candidate;
                break;
            }
            search = candidate + candidate_len;
        }
        const close_at = close orelse break;
        try checkExecutableCapability(arena, file, role, line_no, line[open + run_len .. close_at], findings, suppressions);
        cursor = close_at + run_len;
    }
}

fn markerRunLength(text: []const u8, start: usize, marker: u8) usize {
    var end = start;
    while (end < text.len and text[end] == marker) : (end += 1) {}
    return end - start;
}

fn checkExecutableCapability(arena: std.mem.Allocator, file: []const u8, role: []const u8, line_no: usize, executable: []const u8, findings: *std.ArrayList(Finding), suppressions: *std.ArrayList(Suppression)) !void {
    for (command_classes) |command| if (command.access == .mutate and containsCommandShape(executable, command.shape)) {
        if (isCapabilityExemption(file, role, command.shape)) return;
        try emit(arena, findings, suppressions, Code.capability, file, line_no, try std.fmt.allocPrint(arena, "read-only role contains coordination or entity write: {s}", .{command.shape}));
        return;
    };
}

const capability_exemptions = [_]struct { file: []const u8, role: []const u8, shape: []const u8 }{
    // Introspection is read-only until its explicit apply phase, whose
    // bounded writes may only create the feedback plan and its findings.
    .{ .file = "agents/introspector.md", .role = "introspector", .shape = "planar plan create" },
    .{ .file = "agents/introspector.md", .role = "introspector", .shape = "planar task add" },
    .{ .file = "agents/introspector.md", .role = "introspector", .shape = "planar question add" },
    // Review is entity-read-only but participates in the claim lease ritual;
    // terminal claim mutations remain deliberately absent from this list.
    .{ .file = "agents/reviewer.md", .role = "reviewer", .shape = "planar-agent pull" },
    .{ .file = "agents/reviewer.md", .role = "reviewer", .shape = "planar-agent claim" },
    .{ .file = "agents/reviewer.md", .role = "reviewer", .shape = "planar-agent heartbeat" },
    // Reviewer doctrine names this command only in its explicit do-not-run
    // list; the coder report supplies the already-completed render result.
    .{ .file = "agents/reviewer.md", .role = "reviewer", .shape = "planar skills render" },
};
fn isCapabilityExemption(file: []const u8, role: []const u8, shape: []const u8) bool {
    for (capability_exemptions) |exemption| if (std.mem.eql(u8, file, exemption.file) and
        std.mem.eql(u8, role, exemption.role) and std.mem.eql(u8, shape, exemption.shape)) return true;
    return false;
}

fn containsCommandShape(line: []const u8, shape: []const u8) bool {
    var cursor: usize = 0;
    while (std.mem.indexOfPos(u8, line, cursor, shape)) |at| {
        const before_ok = at == 0 or std.ascii.isWhitespace(line[at - 1]) or std.mem.indexOfScalar(u8, "$(`/;|&{", line[at - 1]) != null;
        const end = at + shape.len;
        const after_ok = end == line.len or std.ascii.isWhitespace(line[end]) or std.mem.indexOfScalar(u8, "<[{(\"'`;|&)$}", line[end]) != null;
        if (before_ok and after_ok) return true;
        cursor = end;
    }
    return false;
}

fn checkCommand(arena: std.mem.Allocator, file: []const u8, line_no: usize, line: []const u8, findings: *std.ArrayList(Finding), suppressions: *std.ArrayList(Suppression)) !void {
    const shape = "planar audit trail";
    var cursor: usize = 0;
    while (std.mem.indexOfPos(u8, line, cursor, shape)) |at| {
        const end = at + shape.len;
        cursor = end;
        if (!containsCommandAt(line, at, shape.len)) continue;
        const args = std.mem.trimStart(u8, line[end..], " \t");
        if (args.len == 0 or args[0] == '`' or args[0] == '.' or args[0] == ',' or args[0] == ')' or
            std.mem.startsWith(u8, args, "[--kind")) continue;
        if (std.mem.startsWith(u8, args, "--kind") or std.mem.startsWith(u8, args, "--link")) {
            if (validAuditSelectorArgs(args)) continue;
            try emit(arena, findings, suppressions, Code.command, file, line_no, "audit trail selector requires `--kind <kind> <entity-id>` or `--link <link-id>`");
            return;
        }
        const token_end = std.mem.indexOfAny(u8, args, " \t`.,)") orelse args.len;
        const token = args[0..token_end];
        if (!isInvalidAuditSelector(token)) continue;
        try emit(arena, findings, suppressions, Code.command, file, line_no, "audit trail entity kind must use `--kind <kind> <entity-id>`");
        return;
    }
}

fn containsCommandAt(line: []const u8, at: usize, shape_len: usize) bool {
    const before_ok = at == 0 or std.ascii.isWhitespace(line[at - 1]) or std.mem.indexOfScalar(u8, "$(`/;|&{", line[at - 1]) != null;
    const end = at + shape_len;
    const after_ok = end == line.len or std.ascii.isWhitespace(line[end]) or std.mem.indexOfScalar(u8, "<[{(\"'`;|&)$}", line[end]) != null;
    return before_ok and after_ok;
}

fn nextCommandToken(args: []const u8, cursor: *usize) ?[]const u8 {
    while (cursor.* < args.len and std.ascii.isWhitespace(args[cursor.*])) cursor.* += 1;
    if (cursor.* >= args.len or std.mem.indexOfScalar(u8, "`;) |&", args[cursor.*]) != null) return null;
    const start = cursor.*;
    while (cursor.* < args.len and !std.ascii.isWhitespace(args[cursor.*]) and
        std.mem.indexOfScalar(u8, "`,;)|&", args[cursor.*]) == null) cursor.* += 1;
    return args[start..cursor.*];
}

fn validAuditSelectorArgs(args: []const u8) bool {
    var cursor: usize = 0;
    const selector = nextCommandToken(args, &cursor) orelse return false;
    const value = nextCommandToken(args, &cursor) orelse return false;
    if (std.mem.eql(u8, selector, "--link")) return value.len != 0 and value[0] != '-';
    if (!std.mem.eql(u8, selector, "--kind") or !isValidAuditKind(value)) return false;
    const entity_id = nextCommandToken(args, &cursor) orelse return false;
    return entity_id.len != 0 and entity_id[0] != '-';
}

fn isValidAuditKind(token: []const u8) bool {
    if (std.mem.eql(u8, token, "<kind>")) return true;
    const kinds = [_][]const u8{ "plan", "task", "question", "scenario", "decision", "artifact" };
    for (kinds) |kind| if (std.mem.eql(u8, token, kind)) return true;
    return false;
}

fn checkDeferredCommand(arena: std.mem.Allocator, file: []const u8, line_no: usize, line: []const u8, in_fence: bool, findings: *std.ArrayList(Finding), suppressions: *std.ArrayList(Suppression)) !void {
    if (!std.mem.startsWith(u8, file, "agents/") and !std.mem.startsWith(u8, file, "skills/src/")) return;
    if (in_fence) return checkDeferredExecutable(arena, file, line_no, line, findings, suppressions);
    var cursor: usize = 0;
    while (std.mem.indexOfScalarPos(u8, line, cursor, '`')) |open| {
        const run_len = markerRunLength(line, open, '`');
        var search = open + run_len;
        var close: ?usize = null;
        while (std.mem.indexOfScalarPos(u8, line, search, '`')) |candidate| {
            const candidate_len = markerRunLength(line, candidate, '`');
            if (candidate_len == run_len) {
                close = candidate;
                break;
            }
            search = candidate + candidate_len;
        }
        const close_at = close orelse break;
        try checkDeferredExecutable(arena, file, line_no, line[open + run_len .. close_at], findings, suppressions);
        cursor = close_at + run_len;
    }
}

fn checkDeferredExecutable(arena: std.mem.Allocator, file: []const u8, line_no: usize, executable: []const u8, findings: *std.ArrayList(Finding), suppressions: *std.ArrayList(Suppression)) !void {
    if (!containsCommandShape(executable, "planar links update")) return;
    try emit(arena, findings, suppressions, Code.command, file, line_no, "deferred `planar links update` cannot be presented as an executable current workflow; use unlink/link recovery");
}

fn isInvalidAuditSelector(token: []const u8) bool {
    if (token.len == 0) return false;
    const kinds = [_][]const u8{ "plan", "task", "question" };
    for (kinds) |kind| {
        if (std.mem.eql(u8, token, kind) or
            (std.mem.startsWith(u8, token, kind) and token.len > kind.len and token[kind.len] == ':')) return true;
        if ((token[0] == '<' or token[0] == '{') and std.mem.indexOf(u8, token, kind) != null) return true;
    }
    return (token[0] == '<' or token[0] == '{') and std.mem.indexOf(u8, token, "kind:") != null;
}

fn checkFeedbackContract(arena: std.mem.Allocator, file: []const u8, content: []const u8, findings: *std.ArrayList(Finding), suppressions: *std.ArrayList(Suppression)) !void {
    const required = [_][]const u8{ "Context", "Intent", "Actions", "Result", "Warnings", "Next actions", "Recovery" };
    var counts = [_]usize{0} ** required.len;
    var malformed_lines = [_]?usize{null} ** required.len;
    var fence: ?Fence = null;
    var lines = std.mem.splitScalar(u8, content, '\n');
    var line_no: usize = 0;
    while (lines.next()) |raw| {
        line_no += 1;
        const line = std.mem.trim(u8, raw, " \t\r");
        if (advanceFence(line, &fence) or fence != null) continue;
        for (required, 0..) |label, idx| {
            const literal = try std.fmt.allocPrint(arena, "## {s}", .{label});
            if (std.mem.eql(u8, line, literal)) {
                counts[idx] += 1;
                if (counts[idx] > 1) try emit(arena, findings, suppressions, Code.contract, file, line_no, try std.fmt.allocPrint(arena, "duplicate required feedback heading `{s}`; keep exactly one literal H2 section", .{literal}));
            } else if (malformed_lines[idx] == null) {
                if (headingLabel(line)) |candidate| {
                    if (std.mem.eql(u8, candidate, label)) malformed_lines[idx] = line_no;
                }
            }
        }
    }
    for (required, 0..) |label, idx| {
        if (counts[idx] != 0) continue;
        const literal = try std.fmt.allocPrint(arena, "## {s}", .{label});
        if (malformed_lines[idx]) |malformed_line| {
            try emit(arena, findings, suppressions, Code.contract, file, malformed_line, try std.fmt.allocPrint(arena, "required feedback section must be the literal H2 heading `{s}`", .{literal}));
        } else {
            try emit(arena, findings, suppressions, Code.contract, file, 1, try std.fmt.allocPrint(arena, "user-invocable skill is missing required feedback heading `{s}`; add that literal H2 section or declare a genuine helper as `internal_only: true` in frontmatter", .{literal}));
        }
    }
}

fn headingLabel(line: []const u8) ?[]const u8 {
    if (line.len < 3 or line[0] != '#') return null;
    var hash_count: usize = 1;
    while (hash_count < line.len and line[hash_count] == '#') : (hash_count += 1) {}
    if (hash_count == line.len or line[hash_count] != ' ') return null;
    return std.mem.trim(u8, line[hash_count + 1 ..], " \t\r");
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
fn frontmatterLiteralTrue(content: []const u8, key: []const u8) bool {
    if (!std.mem.startsWith(u8, content, "---\n")) return false;
    var lines = std.mem.splitScalar(u8, content[4..], '\n');
    while (lines.next()) |line| {
        if (std.mem.eql(u8, std.mem.trim(u8, line, " \t\r"), "---")) break;
        const colon = std.mem.indexOfScalar(u8, line, ':') orelse continue;
        if (std.mem.eql(u8, std.mem.trim(u8, line[0..colon], " \t"), key))
            return std.mem.eql(u8, std.mem.trim(u8, line[colon + 1 ..], " \t\r"), "true");
    }
    return false;
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
fn writeCommandInventory(arena: std.mem.Allocator, out: *std.ArrayList(u8)) !void {
    try out.appendSlice(arena, "{\"version\":1,\"commands\":[");
    for (command_classes, 0..) |command, idx| {
        if (idx != 0) try out.append(arena, ',');
        try out.appendSlice(arena, "{\"command\":");
        try appendJsonString(arena, out, command.shape);
        try out.appendSlice(arena, ",\"access\":");
        try appendJsonString(arena, out, @tagName(command.access));
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
    return scanFixtureAt("agents/fixture.md", content, opts);
}
fn scanSkillFixture(content: []const u8, opts: Options) !FixtureResult {
    return scanFixtureAt("skills/src/fixture.md", content, opts);
}
fn scanFixtureAt(file: []const u8, content: []const u8, opts: Options) !FixtureResult {
    var fixture = FixtureResult{
        .arena = std.heap.ArenaAllocator.init(testing.allocator),
        .result = .{ .files_scanned = 1 },
    };
    errdefer fixture.deinit();
    const arena = fixture.arena.allocator();
    try scanFile(arena, std.testing.io, ".", file, file, content, opts, &fixture.result.findings);
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
        const file = if (std.mem.eql(u8, case.code, Code.contract)) "skills/src/fixture.md" else "agents/fixture.md";
        var fixture = try scanFixtureAt(file, case.content, .{});
        defer fixture.deinit();
        try testing.expect(fixture.result.findings.items.len >= 1);
        try testing.expectEqualStrings(case.code, fixture.result.findings.items[0].code);
        try testing.expectEqual(case.line, fixture.result.findings.items[0].line);
    }
}
test "every pinned retired pattern emits stable legacy evidence" {
    const cases = [_]struct { pattern: []const u8, content: []const u8 }{
        .{ .pattern = "src/internal/", .content = "Header\n\nUse `src/internal/workbench/parse.go`.\n" },
        .{ .pattern = "harness Agent/Task tool", .content = "Header\n\nThe harness Agent/Task tool owns dispatch.\n" },
        .{ .pattern = "harness Agent tool", .content = "Header\n\nThe harness Agent tool owns dispatch.\n" },
        .{ .pattern = "Go side", .content = "Header\n\nThe Go side performs this step.\n" },
        .{ .pattern = "Phase 5.5", .content = "Header\n\nContinue in Phase 5.5.\n" },
    };
    try testing.expectEqual(retired_patterns.len, cases.len);
    for (cases, retired_patterns) |case, pattern| {
        try testing.expectEqualStrings(pattern, case.pattern);
        var fixture = try scanFixture(case.content, .{});
        defer fixture.deinit();
        try testing.expectEqual(@as(usize, 1), fixture.result.findings.items.len);
        const finding = fixture.result.findings.items[0];
        try testing.expectEqualStrings(Code.legacy, finding.code);
        try testing.expectEqual(@as(usize, 3), finding.line);
        const expected = try std.fmt.allocPrint(testing.allocator, "retired authored-surface reference: {s}", .{case.pattern});
        defer testing.allocator.free(expected);
        try testing.expectEqualStrings(expected, finding.message);
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
    var fixture = try scanSkillFixture(content, .{});
    defer fixture.deinit();
    try testing.expectEqual(@as(usize, 0), fixture.result.findings.items.len);
}
test "feedback contract rejects missing result and recovery headings" {
    const cases = [_]struct { omitted: []const u8, expected: []const u8 }{
        .{ .omitted = "## Result\n", .expected = "## Result" },
        .{ .omitted = "## Recovery\n", .expected = "## Recovery" },
    };
    const complete = "## Context\n## Intent\n## Actions\n## Result\n## Warnings\n## Next actions\n## Recovery\n";
    for (cases) |case| {
        const at = std.mem.indexOf(u8, complete, case.omitted).?;
        const content = try std.mem.concat(testing.allocator, u8, &.{
            "---\nslug: fixture\n---\n# Fixture\n\n",
            complete[0..at],
            complete[at + case.omitted.len ..],
        });
        defer testing.allocator.free(content);
        var fixture = try scanSkillFixture(content, .{});
        defer fixture.deinit();
        try testing.expectEqual(@as(usize, 1), fixture.result.findings.items.len);
        try testing.expectEqualStrings(Code.contract, fixture.result.findings.items[0].code);
        try testing.expect(std.mem.indexOf(u8, fixture.result.findings.items[0].message, case.expected) != null);
    }
}
test "feedback contract rejects malformed and duplicate literal H2 headings" {
    const cases = [_]struct { heading: []const u8, line: usize, message: []const u8 }{
        .{ .heading = "### Result\n", .line = 9, .message = "must be the literal H2 heading" },
        .{ .heading = "## Result\n## Result\n", .line = 10, .message = "duplicate required feedback heading" },
    };
    for (cases) |case| {
        const content = try std.mem.concat(testing.allocator, u8, &.{
            "---\nslug: fixture\n---\n# Fixture\n\n## Context\n## Intent\n## Actions\n",
            case.heading,
            "## Warnings\n## Next actions\n## Recovery\n",
        });
        defer testing.allocator.free(content);
        var fixture = try scanSkillFixture(content, .{});
        defer fixture.deinit();
        try testing.expectEqual(@as(usize, 1), fixture.result.findings.items.len);
        try testing.expectEqualStrings(Code.contract, fixture.result.findings.items[0].code);
        try testing.expectEqual(case.line, fixture.result.findings.items[0].line);
        try testing.expect(std.mem.indexOf(u8, fixture.result.findings.items[0].message, case.message) != null);
    }
}
test "suppression syntax shown inside a fence is illustrative" {
    var fixture = try scanFixture("```html\n<!-- surface-lint-ignore surface-legacy-reference: example -->\n```\n", .{});
    defer fixture.deinit();
    try testing.expectEqual(@as(usize, 0), fixture.result.findings.items.len);
}
test "generated vendor agent projections are not authored surfaces" {
    try testing.expect(isAuthoredSurface("agents/coder.md"));
    try testing.expect(!isAuthoredSurface("agents/claude/coder.md"));
    try testing.expect(!isAuthoredSurface("agents/codex/coder.toml"));
    try testing.expect(!isAuthoredSurface("agents/copilot/coder.agent.md"));
    try testing.expect(isAuthoredSurface("skills/src/pl-coder.md"));
    try testing.expect(isAuthoredSurface("docs/architecture.md"));
}
test "narrow introspector write exemption and internal-only contract exemption pass" {
    const agent = "---\nrole: introspector\ncapability: read-only\n---\n```\nplanar task add \"bounded finding\"\n```\n";
    var agent_fixture = try scanFixtureAt("agents/introspector.md", agent, .{});
    defer agent_fixture.deinit();
    try testing.expectEqual(@as(usize, 0), agent_fixture.result.findings.items.len);

    const skill = "---\nslug: fixture\ninternal_only: true\n---\n# Internal helper\n\nCalled only by `pl-orchestrator`, which owns the operator-facing feedback envelope.\n";
    var skill_fixture = try scanSkillFixture(skill, .{});
    defer skill_fixture.deinit();
    try testing.expectEqual(@as(usize, 0), skill_fixture.result.findings.items.len);

    const reviewer = "---\nrole: reviewer\ncapability: read-only\n---\nUse `planar-agent heartbeat --claim token`; never `planar-agent complete --claim token`.\n";
    var reviewer_fixture = try scanFixtureAt("agents/reviewer.md", reviewer, .{});
    defer reviewer_fixture.deinit();
    try testing.expectEqual(@as(usize, 1), reviewer_fixture.result.findings.items.len);
    try testing.expectEqualStrings(Code.capability, reviewer_fixture.result.findings.items[0].code);
}
test "internal-only exemption requires a literal frontmatter boolean" {
    const quoted = "---\nslug: fixture\ninternal_only: \"true\"\n---\n# Not exempt\n";
    var fixture = try scanSkillFixture(quoted, .{});
    defer fixture.deinit();
    try testing.expectEqual(@as(usize, 7), fixture.result.findings.items.len);
    for (fixture.result.findings.items) |finding| try testing.expectEqualStrings(Code.contract, finding.code);
}
test "capability drift scans fenced and inline executable writes without flagging reads or prose" {
    const frontmatter = "---\nrole: fixture\ncapability: read-only\n---\n";
    const writes = [_][]const u8{
        "Run `planar artifact add \"spec\" --kind tech-spec`.\n",
        "Run `planar artifact update 42 --body @spec.md`.\n",
        "Run `planar scenario add \"acceptance\"`.\n",
        "Run `planar links add task:1 plan:2 --relationship derives-from`.\n",
        "```sh\nplanar-agent heartbeat --claim token\n```\n",
    };
    for (writes) |write| {
        const content = try std.mem.concat(testing.allocator, u8, &.{ frontmatter, write });
        defer testing.allocator.free(content);
        var fixture = try scanFixture(content, .{});
        defer fixture.deinit();
        try testing.expectEqual(@as(usize, 1), fixture.result.findings.items.len);
        try testing.expectEqualStrings(Code.capability, fixture.result.findings.items[0].code);
    }

    const clean = frontmatter ++
        "Plain prose names planar artifact add but is not an executable example.\n" ++
        "Run `planar artifact show 42` and `planar links list task:1`.\n";
    var clean_fixture = try scanFixture(clean, .{});
    defer clean_fixture.deinit();
    try testing.expectEqual(@as(usize, 0), clean_fixture.result.findings.items.len);
}
test "capability drift treats interactive explore as a conditional write surface" {
    const content = "---\nrole: fixture\ncapability: read-only\n---\nRun `planar explore`; cockpit actions can mutate state.\n";
    var fixture = try scanFixture(content, .{});
    defer fixture.deinit();
    try testing.expectEqual(@as(usize, 1), fixture.result.findings.items.len);
    try testing.expectEqualStrings(Code.capability, fixture.result.findings.items[0].code);
}
test "shell command boundaries accept separators substitutions and groups but reject partial identifiers" {
    const frontmatter = "---\nrole: fixture\ncapability: read-only\n---\n";
    const writes = [_][]const u8{
        "```sh\nplanar init; planar task show 1\n```\n",
        "Run `value=$(planar task add title)` now.\n",
        "Run `(planar task add title)` or `{ planar task add title; }`.\n",
        "Run `true &&planar task add title|planar task show 1`.\n",
    };
    for (writes) |write| {
        const content = try std.mem.concat(testing.allocator, u8, &.{ frontmatter, write });
        defer testing.allocator.free(content);
        var fixture = try scanFixture(content, .{});
        defer fixture.deinit();
        try testing.expect(fixture.result.findings.items.len >= 1);
        for (fixture.result.findings.items) |finding| try testing.expectEqualStrings(Code.capability, finding.code);
    }
    const clean = frontmatter ++
        "Run `myplanar task add title`, `planar-task add title`, or `planar task additive`.\n";
    var clean_fixture = try scanFixture(clean, .{});
    defer clean_fixture.deinit();
    try testing.expectEqual(@as(usize, 0), clean_fixture.result.findings.items.len);
}
test "schema inventory has an explicit unique classification for every current leaf" {
    // Generated from the three freshly built catalogs with:
    //   for b in planar planar-agent planar-watch; do
    //     $b schema | jq -r '.commands[] | select((.subcommands|length)==0 and (.path|length)>0) | .command'
    //   done
    // The fingerprint pins names and order, while the counts identify which
    // binary drifted when a catalog changes.
    try testing.expectEqual(@as(usize, 259), command_classes.len);
    var planar_count: usize = 0;
    var agent_count: usize = 0;
    var watch_count: usize = 0;
    var inventory: std.ArrayList(u8) = .empty;
    defer inventory.deinit(testing.allocator);
    for (command_classes, 0..) |command, idx| {
        for (command_classes[0..idx]) |prior| try testing.expect(!std.mem.eql(u8, prior.shape, command.shape));
        try inventory.appendSlice(testing.allocator, command.shape);
        try inventory.append(testing.allocator, '\n');
        if (std.mem.startsWith(u8, command.shape, "planar ")) planar_count += 1 else if (std.mem.startsWith(u8, command.shape, "planar-agent ")) agent_count += 1 else if (std.mem.startsWith(u8, command.shape, "planar-watch ")) watch_count += 1 else return error.InvalidCommandClassification;
    }
    try testing.expectEqual(@as(usize, 224), planar_count);
    try testing.expectEqual(@as(usize, 22), agent_count);
    try testing.expectEqual(@as(usize, 13), watch_count);
    var digest: [std.crypto.hash.sha2.Sha256.digest_length]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(inventory.items, &digest, .{});
    const hex = std.fmt.bytesToHex(digest, .lower);
    try testing.expectEqualStrings("51b6a40025548a1239edd5d2883dbe6819a06224ae4eadabe16829ae4285297a", &hex);
}
test "every classified leaf enforces its declared capability" {
    const frontmatter = "---\nrole: fixture\ncapability: read-only\n---\n```sh\n";
    for (command_classes) |command| {
        const content = try std.mem.concat(testing.allocator, u8, &.{ frontmatter, command.shape, " --json\n```\n" });
        defer testing.allocator.free(content);
        var fixture = try scanFixture(content, .{});
        defer fixture.deinit();
        const expected: usize = if (command.access == .mutate) 1 else 0;
        try testing.expectEqual(expected, fixture.result.findings.items.len);
        if (expected != 0) try testing.expectEqualStrings(Code.capability, fixture.result.findings.items[0].code);
    }
}
test "Markdown executable delimiters require matching markers and run lengths" {
    const frontmatter = "---\nrole: fixture\ncapability: read-only\n---\n";
    const cases = [_][]const u8{
        frontmatter ++ "Use ``planar task add `literal` --title x`` now.\n",
        frontmatter ++ "```sh\n~~~\nplanar task add x\n```\n",
        frontmatter ++ "````sh\n```\nplanar task add x\n````\n",
    };
    for (cases) |content| {
        var fixture = try scanFixture(content, .{});
        defer fixture.deinit();
        try testing.expectEqual(@as(usize, 1), fixture.result.findings.items.len);
        try testing.expectEqualStrings(Code.capability, fixture.result.findings.items[0].code);
    }

    const clean = frontmatter ++ "Use ``planar task show `literal` --json`` now.\n";
    var clean_fixture = try scanFixture(clean, .{});
    defer clean_fixture.deinit();
    try testing.expectEqual(@as(usize, 0), clean_fixture.result.findings.items.len);
}
test "audit command drift catches plan and entity placeholder variants" {
    const invalid = [_][]const u8{
        "Run `planar audit trail <plan-id>`.\n",
        "Run `planar audit trail <plan> --grep x`.\n",
        "Run `planar audit trail plan:42`.\n",
        "Run `planar audit trail plan 42`.\n",
        "Run `planar audit trail <kind:id>`.\n",
        "Run `planar audit trail {question-id}`.\n",
        "Run `planar audit trail --kind plan:42`.\n",
        "Run `planar audit trail --kind plan`.\n",
        "Run `planar audit trail --kind bogus 42`.\n",
        "Run `planar audit trail --link`.\n",
    };
    for (invalid) |content| {
        var fixture = try scanFixture(content, .{});
        defer fixture.deinit();
        try testing.expectEqual(@as(usize, 1), fixture.result.findings.items.len);
        try testing.expectEqualStrings(Code.command, fixture.result.findings.items[0].code);
    }
    var valid = try scanFixture("Run `planar audit trail --kind plan <plan-id>`, `planar audit trail --kind <kind> <entity-id>`, or `planar audit trail --link <link-id>`.\n", .{});
    defer valid.deinit();
    try testing.expectEqual(@as(usize, 0), valid.result.findings.items.len);
}
test "deferred links update cannot be an executable current agent or skill workflow" {
    const cases = [_][]const u8{
        "Run `planar links update <link-id> --sync two-way`.\n",
        "```sh\nplanar links update 7 --sync read-only\n```\n",
    };
    for (cases) |content| {
        var fixture = try scanFixture(content, .{});
        defer fixture.deinit();
        try testing.expectEqual(@as(usize, 1), fixture.result.findings.items.len);
        try testing.expectEqualStrings(Code.command, fixture.result.findings.items[0].code);
    }

    var documented_future = try scanFixtureAt("docs/cli-reference.md", "`planar links update <link-id>` is explicitly deferred and not executable current behavior.\n", .{});
    defer documented_future.deinit();
    try testing.expectEqual(@as(usize, 0), documented_future.result.findings.items.len);

    var non_executable = try scanFixtureAt("agents/fixture.md", "The planar links update stub is deferred.\n", .{});
    defer non_executable.deinit();
    try testing.expectEqual(@as(usize, 0), non_executable.result.findings.items.len);
}
test "only exact projection links are exempt from source-tree resolution" {
    var exact = try scanFixtureAt("docs/cli-reference.md", "[projection](../commands/claude/pl-synthesize.md)\n", .{});
    defer exact.deinit();
    try testing.expectEqual(@as(usize, 0), exact.result.findings.items.len);

    var substring = try scanFixtureAt("docs/other.md", "[missing](bogus/commands/claude/pl-synthesize.md)\n", .{});
    defer substring.deinit();
    try testing.expectEqual(@as(usize, 1), substring.result.findings.items.len);
    try testing.expectEqualStrings(Code.link, substring.result.findings.items[0].code);
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
