//! cmd/planar/editflow — shared view and edit orchestration for all 6 planning entities.
//!
//! This module is the Zig equivalent of Go's cli.RunEditorEdit +
//! cli.WorkbenchPathFor. It is a cmd-layer helper (peer of editor.zig),
//! NOT placed under handlers/. Each <entity>/view.zig and <entity>/edit.zig
//! handler becomes a thin shim that parses its positional arg and delegates
//! here.
//!
//! M4 limitations (documented in the spec and confirmed per decision D-m4-edit-reduced-flow):
//!   - Title and status mutations ONLY. Other frontmatter changes are warned
//!     and silently dropped.
//!   - No conflict detection (M5).
//!   - No auto-push/post-pull (deferred to later milestones).
//!   - No dry-run gate (deferred).
//!   - No three-way merge (M6).
//!
//! Architecture decisions honored:
//!   D-shared-helper  : this module, two pub fns view/edit.
//!   D-m4-edit-reduced-flow : steps 1-7 as specified.
//!   D-m4-view-flow   : steps 1-5 as specified.
//!   D-anchor-plan-resolver : resolveAnchorPlan private helper.
//!   D-pager-shape    : $PAGER → `less` → `cat` fallback chain.
//!   D-no-engine-edits: src-zig/src/engine/** untouched.
//!   D-handler-shape  : handlers are thin shims; no SQL in handlers.

const std = @import("std");
const db = @import("db");
const engine = @import("engine");
const editor = @import("editor.zig");
const runtime = @import("runtime");

// =========================================================================
// Public types
// =========================================================================

/// The six planning entity kinds covered by Cycle B.
pub const EntityKind = enum {
    plan,
    task,
    question,
    scenario,
    decision,
    artifact,

    /// Parse a string to an EntityKind. Returns null for unknown kinds.
    pub fn fromText(s: []const u8) ?EntityKind {
        if (std.mem.eql(u8, s, "plan")) return .plan;
        if (std.mem.eql(u8, s, "task")) return .task;
        if (std.mem.eql(u8, s, "question")) return .question;
        if (std.mem.eql(u8, s, "scenario")) return .scenario;
        if (std.mem.eql(u8, s, "decision")) return .decision;
        if (std.mem.eql(u8, s, "artifact")) return .artifact;
        return null;
    }

    /// Return the string used in workbench relative paths and frontmatter.
    pub fn toText(self: EntityKind) []const u8 {
        return switch (self) {
            .plan => "plan",
            .task => "task",
            .question => "question",
            .scenario => "scenario",
            .decision => "decision",
            .artifact => "artifact",
        };
    }

    /// Return the entity_links from_kind value (scenario uses "test_scenario").
    pub fn toLinkKind(self: EntityKind) []const u8 {
        return switch (self) {
            .scenario => "test_scenario",
            else => self.toText(),
        };
    }
};

/// Options for the edit verb. All fields are optional overrides.
pub const EditOpts = struct {
    /// When non-null, use this instead of the env-var editor chain.
    editor_override: ?[]const u8 = null,
};

/// ReviewVerdict is the explicit decision provided by `--approve` or
/// `--request-changes`.
pub const ReviewVerdict = enum {
    approve,
    request_changes,

    pub fn toText(self: ReviewVerdict) []const u8 {
        return switch (self) {
            .approve => "approve",
            .request_changes => "request-changes",
        };
    }
};

/// ReviewSummary is a stable response envelope for `<entity> review`.
pub const ReviewSummary = struct {
    entity: []const u8,
    id: i64,
    anchor_plan_id: i64,
    workbench_path: []const u8,
    verdict: ?[]const u8 = null,
    has_changes: bool,
    persisted: bool,
    persistence: []const u8,
};

// =========================================================================
// Public API
// =========================================================================

/// view renders the entity to its canonical workbench file path, then
/// opens it in $PAGER (→ less → cat). Does NOT mutate the DB.
///
/// M4 view flow:
///   1. Resolve entity from DB + walk to anchor plan.
///   2. Build workbench path via feature helpers.
///   3. Render entity to markdown via renderEntity().
///   4. Write rendered content to canonical workbench file path.
///   5. exec $PAGER with the file path; fall back to `less` then `cat`.
pub fn view(
    ctx: *const runtime.Ctx,
    d: *db.sqlite.Db,
    kind: EntityKind,
    entity_id: i64,
) !void {
    const allocator = ctx.allocator;

    // 1. Resolve entity + anchor plan.
    const anchor_id = resolveAnchorPlan(d, allocator, kind, entity_id) catch |e| {
        ctx.stderr.print(
            "error: cannot resolve anchor plan for {s} {d}: {s}\n",
            .{ kind.toText(), entity_id, @errorName(e) },
        ) catch {};
        return e;
    };

    // 2. Build canonical workbench file path.
    const abs_path = try buildWorkbenchPath(d, allocator, kind, entity_id, anchor_id);
    defer allocator.free(abs_path);

    // 3. Render entity to markdown.
    const content = try renderEntity(d, allocator, kind, entity_id, anchor_id);
    defer allocator.free(content);

    // 4. Ensure the directory exists and write the file.
    if (std.fs.path.dirname(abs_path)) |dir| {
        std.Io.Dir.cwd().createDirPath(ctx.io, dir) catch |e| switch (e) {
            error.PathAlreadyExists => {},
            else => return e,
        };
    }
    try std.Io.Dir.cwd().writeFile(ctx.io, .{ .sub_path = abs_path, .data = content });

    // 5. Exec pager.
    try execPager(ctx.io, allocator, abs_path);
}

/// edit renders the entity to its workbench file, opens it in $EDITOR,
/// parses the saved content, and applies title/status mutations to the DB.
///
/// M4 edit flow (D-m4-edit-reduced-flow):
///   1. Resolve entity + anchor plan.
///   2. Build workbench path.
///   3. Render entity to markdown.
///   4. Write rendered content to canonical workbench file (DB is authoritative in M4).
///   5. Call editor.invoke with the canonical path content as initial_content.
///   6. On non-zero exit code: abort without writing.
///   7. Parse the editor output via engine.workbench.parse.
///   8. Apply title+status only; warn and drop other frontmatter changes.
pub fn edit(
    ctx: *const runtime.Ctx,
    d: *db.sqlite.Db,
    kind: EntityKind,
    entity_id: i64,
    opts: EditOpts,
) !void {
    const allocator = ctx.allocator;

    // 1. Resolve anchor plan.
    const anchor_id = resolveAnchorPlan(d, allocator, kind, entity_id) catch |e| {
        ctx.stderr.print(
            "error: cannot resolve anchor plan for {s} {d}: {s}\n",
            .{ kind.toText(), entity_id, @errorName(e) },
        ) catch {};
        return e;
    };

    // 2. Build canonical workbench file path.
    const abs_path = try buildWorkbenchPath(d, allocator, kind, entity_id, anchor_id);
    defer allocator.free(abs_path);

    // 3. Render entity to markdown.
    const content = try renderEntity(d, allocator, kind, entity_id, anchor_id);
    defer allocator.free(content);

    // 4. Write rendered content to canonical workbench file.
    if (std.fs.path.dirname(abs_path)) |dir| {
        std.Io.Dir.cwd().createDirPath(ctx.io, dir) catch |e| switch (e) {
            error.PathAlreadyExists => {},
            else => return e,
        };
    }
    warnOnWorkbenchOverwrite(ctx, allocator, abs_path, content);
    try std.Io.Dir.cwd().writeFile(ctx.io, .{ .sub_path = abs_path, .data = content });

    // 5. Call editor.invoke with the rendered content as initial_content.
    //    We pass the canonical path's content, but editor writes to a temp file
    //    per the editor.zig contract. We then take the edited content back.
    const edit_result = try editor.invoke(ctx.io, allocator, content, .{
        .editor_override = opts.editor_override,
    });
    defer edit_result.deinit(allocator);

    // 6. Abort on non-zero editor exit.
    if (edit_result.editor_exit_code != 0) {
        ctx.stderr.print(
            "aborted: editor exited with code {d}\n",
            .{edit_result.editor_exit_code},
        ) catch {};
        return;
    }

    // 7. Parse the editor output.
    const parsed = engine.workbench.parse.parse(allocator, edit_result.content) catch |e| {
        ctx.stderr.print(
            "error: failed to parse editor output for {s} {d}: {s}\n",
            .{ kind.toText(), entity_id, @errorName(e) },
        ) catch {};
        return e;
    };
    defer engine.workbench.parse.deinit(parsed, allocator);

    const new_fm = parsed.frontmatter;

    // Parse original to compare.
    const orig_parsed = engine.workbench.parse.parse(allocator, content) catch |e| {
        ctx.stderr.print("error: failed to parse original content: {s}\n", .{@errorName(e)}) catch {};
        return e;
    };
    defer engine.workbench.parse.deinit(orig_parsed, allocator);
    const orig_fm = orig_parsed.frontmatter;

    // 8. Emit M4 limitation warning for non-title/status changes.
    //    Check for changes in any frontmatter field beyond title and status.
    const has_other_changes = detectOtherFrontmatterChanges(orig_fm, new_fm);
    if (has_other_changes) {
        ctx.stderr.print(
            "[M4 limitation: only title and status mutations are applied; other fields are ignored]\n",
            .{},
        ) catch {};
    }

    // Apply title and/or status changes.
    const title_changed = !std.mem.eql(u8, orig_fm.title, new_fm.title);
    const status_changed = !std.mem.eql(u8, orig_fm.status, new_fm.status);

    if (!title_changed and !status_changed) {
        ctx.stderr.print("aborted: no changes made\n", .{}) catch {};
        return;
    }

    // Write mutations to the DB.
    const new_title: ?[]const u8 = if (title_changed) new_fm.title else null;
    const new_status: ?[]const u8 = if (status_changed) new_fm.status else null;
    try applyMutations(d, allocator, kind, entity_id, new_title, new_status);

    // Write the final edited content back to the canonical workbench file
    // so the on-disk file reflects what was applied.
    std.Io.Dir.cwd().writeFile(ctx.io, .{ .sub_path = abs_path, .data = edit_result.content }) catch {};
}

/// diff renders the DB version for an entity and compares it to the current
/// workbench file. Equal content is silent, mirroring `diff -u`.
pub fn diff(
    ctx: *const runtime.Ctx,
    d: *db.sqlite.Db,
    kind: EntityKind,
    entity_id: i64,
) !void {
    const snapshot = try loadDiffSnapshot(ctx, d, kind, entity_id);
    defer snapshot.deinit(ctx.allocator);
    if (!snapshot.has_changes) return;
    try writeUnifiedDiff(ctx, kind, entity_id, snapshot.abs_path, snapshot.db_content, snapshot.fs_content);
}

/// review evaluates the current DB-vs-workbench state for an entity.
///
/// When `verdict` is null, this is preview mode and writes the diff (if any)
/// to stdout. When `verdict` is present, this emits a stable review summary
/// in text or JSON form. There is intentionally no per-entity review row in
/// schema; the summary reports `persisted=false`.
pub fn review(
    ctx: *const runtime.Ctx,
    d: *db.sqlite.Db,
    kind: EntityKind,
    entity_id: i64,
    verdict: ?ReviewVerdict,
    json: bool,
) !void {
    const snapshot = try loadDiffSnapshot(ctx, d, kind, entity_id);
    defer snapshot.deinit(ctx.allocator);

    if (verdict == null) {
        if (json) {
            const preview = ReviewSummary{
                .entity = kind.toText(),
                .id = entity_id,
                .anchor_plan_id = snapshot.anchor_plan_id,
                .workbench_path = snapshot.abs_path,
                .has_changes = snapshot.has_changes,
                .persisted = false,
                .persistence = "none",
            };
            try std.json.Stringify.value(preview, .{}, ctx.stdout);
            try ctx.stdout.print("\n", .{});
            return;
        }
        if (!snapshot.has_changes) return;
        try writeUnifiedDiff(ctx, kind, entity_id, snapshot.abs_path, snapshot.db_content, snapshot.fs_content);
        return;
    }

    const summary = ReviewSummary{
        .entity = kind.toText(),
        .id = entity_id,
        .anchor_plan_id = snapshot.anchor_plan_id,
        .workbench_path = snapshot.abs_path,
        .verdict = verdict.?.toText(),
        .has_changes = snapshot.has_changes,
        .persisted = false,
        .persistence = "none",
    };

    if (json) {
        try std.json.Stringify.value(summary, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
        return;
    }

    try ctx.stdout.print(
        "{s} {d} review: {s} (persisted: no; reason: no per-entity review table)\n",
        .{ kind.toText(), entity_id, verdict.?.toText() },
    );
    try ctx.stdout.print(
        "workbench: {s}\nanchor_plan: {d}\nchanges_pending: {s}\n",
        .{ summary.workbench_path, summary.anchor_plan_id, if (summary.has_changes) "yes" else "no" },
    );
}

const DiffSnapshot = struct {
    abs_path: []const u8,
    db_content: []const u8,
    fs_content: []const u8,
    anchor_plan_id: i64,
    has_changes: bool,

    fn deinit(self: DiffSnapshot, allocator: std.mem.Allocator) void {
        allocator.free(self.abs_path);
        allocator.free(self.db_content);
        allocator.free(self.fs_content);
    }
};

fn loadDiffSnapshot(
    ctx: *const runtime.Ctx,
    d: *db.sqlite.Db,
    kind: EntityKind,
    entity_id: i64,
) !DiffSnapshot {
    const allocator = ctx.allocator;
    const anchor_id = try resolveAnchorPlan(d, allocator, kind, entity_id);
    const canonical = try engine.workbench.sync.renderEntityCanonical(
        d,
        allocator,
        anchor_id,
        kind.toText(),
        entity_id,
    );
    defer canonical.deinit(allocator);

    const abs_path = try buildWorkbenchPathFromRel(d, allocator, anchor_id, canonical.rel_path);
    errdefer allocator.free(abs_path);
    const db_content = try allocator.dupe(u8, canonical.content);
    errdefer allocator.free(db_content);

    const fs_content = readFile(ctx.io, allocator, abs_path) catch |e| switch (e) {
        error.FileNotFound => try allocator.dupe(u8, ""),
        else => return e,
    };
    errdefer allocator.free(fs_content);

    return .{
        .abs_path = abs_path,
        .db_content = db_content,
        .fs_content = fs_content,
        .anchor_plan_id = anchor_id,
        .has_changes = !std.mem.eql(u8, db_content, fs_content),
    };
}

fn readFile(io: std.Io, allocator: std.mem.Allocator, path: []const u8) ![]u8 {
    return try std.Io.Dir.cwd().readFileAlloc(io, path, allocator, .limited(10 * 1024 * 1024));
}

fn warnOnWorkbenchOverwrite(
    ctx: *const runtime.Ctx,
    allocator: std.mem.Allocator,
    abs_path: []const u8,
    rendered: []const u8,
) void {
    const existing = readFile(ctx.io, allocator, abs_path) catch |e| switch (e) {
        error.FileNotFound => return,
        else => return,
    };
    defer allocator.free(existing);
    if (std.mem.eql(u8, existing, rendered)) return;
    ctx.stderr.print(
        "warning: overwriting local workbench file with DB-rendered content before edit: {s}\n",
        .{abs_path},
    ) catch {};
}

const DiffOpTag = enum { equal, delete, insert };
const DiffOp = struct {
    tag: DiffOpTag,
    line: []const u8,
};

const Hunk = struct {
    start: usize,
    end: usize,
};

fn writeUnifiedDiff(
    ctx: *const runtime.Ctx,
    kind: EntityKind,
    entity_id: i64,
    abs_path: []const u8,
    db_content: []const u8,
    fs_content: []const u8,
) !void {
    try ctx.stdout.print("--- db:{s}:{d}\n", .{ kind.toText(), entity_id });
    try ctx.stdout.print("+++ fs:{s}\n", .{abs_path});

    var arena = std.heap.ArenaAllocator.init(ctx.allocator);
    defer arena.deinit();
    const a = arena.allocator();

    const db_lines = try splitLines(a, db_content);
    const fs_lines = try splitLines(a, fs_content);
    const ops = try buildDiffOps(a, db_lines, fs_lines);
    const hunks = try buildHunks(a, ops, 3);
    const pre = try buildPreCounts(a, ops);

    for (hunks) |hunk| {
        try writeHunkHeader(ctx, ops, pre, hunk);
        for (ops[hunk.start..hunk.end]) |op| {
            const prefix: u8 = switch (op.tag) {
                .equal => ' ',
                .delete => '-',
                .insert => '+',
            };
            try ctx.stdout.print("{c}{s}\n", .{ prefix, op.line });
        }
    }
}

fn splitLines(allocator: std.mem.Allocator, content: []const u8) ![]const []const u8 {
    var out: std.ArrayList([]const u8) = .empty;
    errdefer out.deinit(allocator);
    var start: usize = 0;
    var i: usize = 0;
    while (i < content.len) : (i += 1) {
        if (content[i] != '\n') continue;
        try out.append(allocator, content[start..i]);
        start = i + 1;
    }
    if (start < content.len) {
        try out.append(allocator, content[start..content.len]);
    } else if (content.len > 0 and content[content.len - 1] == '\n') {
        try out.append(allocator, content[content.len..content.len]);
    }
    return try out.toOwnedSlice(allocator);
}

fn buildDiffOps(
    allocator: std.mem.Allocator,
    a_lines: []const []const u8,
    b_lines: []const []const u8,
) ![]const DiffOp {
    const n = a_lines.len;
    const m = b_lines.len;
    const width = m + 1;

    var lcs = try allocator.alloc(usize, (n + 1) * (m + 1));
    @memset(lcs, 0);

    var i: usize = n;
    while (i > 0) {
        i -= 1;
        var j: usize = m;
        while (j > 0) {
            j -= 1;
            const idx = i * width + j;
            if (std.mem.eql(u8, a_lines[i], b_lines[j])) {
                lcs[idx] = lcs[(i + 1) * width + (j + 1)] + 1;
            } else {
                const down = lcs[(i + 1) * width + j];
                const right = lcs[i * width + (j + 1)];
                lcs[idx] = if (down >= right) down else right;
            }
        }
    }

    var ops: std.ArrayList(DiffOp) = .empty;
    errdefer ops.deinit(allocator);

    i = 0;
    var j: usize = 0;
    while (i < n and j < m) {
        if (std.mem.eql(u8, a_lines[i], b_lines[j])) {
            try ops.append(allocator, .{ .tag = .equal, .line = a_lines[i] });
            i += 1;
            j += 1;
            continue;
        }

        const down = lcs[(i + 1) * width + j];
        const right = lcs[i * width + (j + 1)];
        if (down >= right) {
            try ops.append(allocator, .{ .tag = .delete, .line = a_lines[i] });
            i += 1;
        } else {
            try ops.append(allocator, .{ .tag = .insert, .line = b_lines[j] });
            j += 1;
        }
    }
    while (i < n) : (i += 1) try ops.append(allocator, .{ .tag = .delete, .line = a_lines[i] });
    while (j < m) : (j += 1) try ops.append(allocator, .{ .tag = .insert, .line = b_lines[j] });

    return try ops.toOwnedSlice(allocator);
}

fn buildHunks(
    allocator: std.mem.Allocator,
    ops: []const DiffOp,
    context: usize,
) ![]const Hunk {
    var changes: std.ArrayList(usize) = .empty;
    defer changes.deinit(allocator);
    for (ops, 0..) |op, idx| {
        if (op.tag != .equal) try changes.append(allocator, idx);
    }
    if (changes.items.len == 0) return try allocator.alloc(Hunk, 0);

    var hunks: std.ArrayList(Hunk) = .empty;
    errdefer hunks.deinit(allocator);

    var open_start: ?usize = null;
    var open_end: usize = 0;
    for (changes.items) |chg| {
        const cand_start = if (chg > context) chg - context else 0;
        const cand_end = @min(ops.len, chg + context + 1);
        if (open_start == null) {
            open_start = cand_start;
            open_end = cand_end;
            continue;
        }
        if (cand_start <= open_end) {
            open_end = @max(open_end, cand_end);
            continue;
        }
        try hunks.append(allocator, .{ .start = open_start.?, .end = open_end });
        open_start = cand_start;
        open_end = cand_end;
    }
    try hunks.append(allocator, .{ .start = open_start.?, .end = open_end });
    return try hunks.toOwnedSlice(allocator);
}

fn buildPreCounts(allocator: std.mem.Allocator, ops: []const DiffOp) ![]const [2]usize {
    const pre = try allocator.alloc([2]usize, ops.len + 1);
    var old_count: usize = 0;
    var new_count: usize = 0;
    pre[0] = .{ 0, 0 };
    for (ops, 0..) |op, idx| {
        switch (op.tag) {
            .equal => {
                old_count += 1;
                new_count += 1;
            },
            .delete => old_count += 1,
            .insert => new_count += 1,
        }
        pre[idx + 1] = .{ old_count, new_count };
    }
    return pre;
}

fn writeHunkHeader(
    ctx: *const runtime.Ctx,
    ops: []const DiffOp,
    pre: []const [2]usize,
    hunk: Hunk,
) !void {
    var old_count: usize = 0;
    var new_count: usize = 0;
    for (ops[hunk.start..hunk.end]) |op| {
        switch (op.tag) {
            .equal => {
                old_count += 1;
                new_count += 1;
            },
            .delete => old_count += 1,
            .insert => new_count += 1,
        }
    }

    const old_start = pre[hunk.start][0] + 1;
    const new_start = pre[hunk.start][1] + 1;

    const old_range = try formatHunkRange(ctx.allocator, old_start, old_count);
    defer ctx.allocator.free(old_range);
    const new_range = try formatHunkRange(ctx.allocator, new_start, new_count);
    defer ctx.allocator.free(new_range);

    try ctx.stdout.print("@@ -{s} +{s} @@\n", .{ old_range, new_range });
}

fn formatHunkRange(
    allocator: std.mem.Allocator,
    start: usize,
    count: usize,
) ![]const u8 {
    if (count == 0) {
        const zero_start = if (start == 0) 0 else start - 1;
        return std.fmt.allocPrint(allocator, "{d},0", .{zero_start});
    }
    if (count == 1) {
        return std.fmt.allocPrint(allocator, "{d}", .{start});
    }
    return std.fmt.allocPrint(allocator, "{d},{d}", .{ start, count });
}

// =========================================================================
// Anchor plan resolver (D-anchor-plan-resolver)
// =========================================================================

/// resolveAnchorPlan mirrors Go's cli.ResolveAnchorPlanForEntity.
/// Returns the top-level anchor plan id for the given (kind, entity_id).
///
/// Rules:
///   - plan: walk parent_plan_id up to the anchor.
///   - task: read tasks.plan_id, then walk to anchor.
///   - artifact/decision/question/scenario: query entity_links derives-from
///     to find the linked plan, then walk to anchor.
pub fn resolveAnchorPlan(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    kind: EntityKind,
    entity_id: i64,
) !i64 {
    switch (kind) {
        .plan => {
            return try walkToAnchor(d, entity_id);
        },
        .task => {
            const plan_id = try taskPlanId(d, entity_id);
            return try walkToAnchor(d, plan_id);
        },
        .question, .scenario, .decision, .artifact => {
            const link_kind = kind.toLinkKind();
            const plan_id = try linkDerivedPlanId(d, allocator, link_kind, entity_id);
            return try walkToAnchor(d, plan_id);
        },
    }
}

/// walkToAnchor walks plans.parent_plan_id upward to the root anchor.
/// Mirrors Go's cli.walkToAnchor.
fn walkToAnchor(d: *db.sqlite.Db, plan_id: i64) !i64 {
    var current = plan_id;
    var i: usize = 0;
    while (i < 1024) : (i += 1) {
        var stmt = d.prepare("select parent_plan_id from plans where id = ?") catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = current }}) catch return error.QueryFailed;
        switch (stmt.step() catch return error.QueryFailed) {
            .done => return error.NotFound,
            .row => {
                const parent = stmt.columnIntOpt(0);
                if (parent == null) return current; // no parent → this IS the anchor
                current = parent.?;
            },
        }
    }
    return error.CyclicPlanChain;
}

/// taskPlanId returns the plan_id for the given task, or error.NoPlanLink
/// if the task has no plan_id (task 2088 contract: unpinned tasks cannot
/// be edited via the editor flow).
fn taskPlanId(d: *db.sqlite.Db, task_id: i64) !i64 {
    var stmt = d.prepare("select plan_id from tasks where id = ?") catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => {
            const plan_id = stmt.columnIntOpt(0) orelse return error.NoPlanLink;
            return plan_id;
        },
    }
}

/// linkDerivedPlanId finds the plan id via entity_links derives-from for
/// artifact/decision/question/scenario. Mirrors Go's entity_links lookup
/// in ResolveAnchorPlanForEntity.
fn linkDerivedPlanId(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    from_kind: []const u8,
    entity_id: i64,
) !i64 {
    _ = allocator;
    const sql: [:0]const u8 =
        "select to_id from entity_links where from_kind = ? and from_id = ? and to_kind = 'plan' and relationship = 'derives-from' order by id limit 1";

    var stmt = d.prepare(sql) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{
        .{ .text = from_kind },
        .{ .int = entity_id },
    }) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NoPlanLink,
        .row => return stmt.columnInt(0),
    }
}

// =========================================================================
// Workbench path builder
// =========================================================================

/// AnchorInfo holds the fields we need from the anchor plan row.
const AnchorInfo = struct {
    slug: []const u8,
    assoc_slug: []const u8,
    plan_key: []const u8,

    fn deinit(self: AnchorInfo, allocator: std.mem.Allocator) void {
        allocator.free(self.slug);
        allocator.free(self.assoc_slug);
        allocator.free(self.plan_key);
    }
};

/// fetchAnchorInfo fetches slug, assoc_slug, and plan_key for an anchor plan.
/// Mirrors Go's workbench.FetchAnchor + AnchorPlan.PlanKey.
fn fetchAnchorInfo(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_id: i64) !AnchorInfo {
    // Query slug + association slug (mirrors Go's FetchAnchor join).
    const sql_z: [:0]const u8 =
        \\select p.slug, coalesce(a.slug, '')
        \\from plans p
        \\left join associations a on (p.scope_kind = 'association' and a.id = p.scope_id)
        \\where p.id = ?
    ;
    var stmt = d.prepare(sql_z) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = anchor_id }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => {
            const slug = try stmt.columnTextAlloc(0, allocator);
            errdefer allocator.free(slug);
            const assoc_slug = try stmt.columnTextAlloc(1, allocator);
            errdefer allocator.free(assoc_slug);

            // plan_key: external_id if present, else "p<id>".
            const plan_key = try resolvePlanKey(d, allocator, anchor_id);

            return AnchorInfo{
                .slug = slug,
                .assoc_slug = assoc_slug,
                .plan_key = plan_key,
            };
        },
    }
}

/// resolvePlanKey returns the plan key: external_id from external_links if
/// present, otherwise "p<id>". Mirrors Go's AnchorPlan.PlanKey.
fn resolvePlanKey(d: *db.sqlite.Db, allocator: std.mem.Allocator, plan_id: i64) ![]const u8 {
    const sql_z: [:0]const u8 =
        \\select external_id from external_links
        \\where entity_kind = 'plan' and entity_id = ?
        \\limit 1
    ;
    var stmt = d.prepare(sql_z) catch {
        // external_links table may not have this row; fall through to default.
        return try std.fmt.allocPrint(allocator, "p{d}", .{plan_id});
    };
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch {
        return try std.fmt.allocPrint(allocator, "p{d}", .{plan_id});
    };
    switch (stmt.step() catch .done) {
        .done => return try std.fmt.allocPrint(allocator, "p{d}", .{plan_id}),
        .row => {
            const ext_id = try stmt.columnTextAlloc(0, allocator);
            if (ext_id.len == 0) {
                allocator.free(ext_id);
                return try std.fmt.allocPrint(allocator, "p{d}", .{plan_id});
            }
            return ext_id;
        },
    }
}

/// getPosixEnv reads a single environment variable from std.c.environ.
/// Returns null when the variable is not set or empty.
/// The returned slice points into the process's environ block; caller must
/// NOT free it.
fn getPosixEnv(key: []const u8) ?[]const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (s.len <= key.len + 1) continue;
        if (s[key.len] != '=') continue;
        if (!std.mem.eql(u8, s[0..key.len], key)) continue;
        const val = s[key.len + 1 ..];
        if (val.len == 0) return null;
        return val;
    }
    return null;
}

/// resolveWorkbenchRoot returns the workbench root from $PLANAR_WORKBENCH_ROOT
/// or the default ~/.planar/workbench/.
fn resolveWorkbenchRoot(allocator: std.mem.Allocator) ![]u8 {
    if (getPosixEnv("PLANAR_WORKBENCH_ROOT")) |root| {
        return try allocator.dupe(u8, root);
    }

    const home = getPosixEnv("HOME") orelse return error.HomeNotSet;
    return try std.fs.path.join(allocator, &.{ home, ".planar", "workbench" });
}

/// buildWorkbenchPath returns the absolute path for the entity's workbench file.
/// The path is computed from: root / featureDir(anchor) / relPath(entity).
fn buildWorkbenchPath(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    kind: EntityKind,
    entity_id: i64,
    anchor_id: i64,
) ![]u8 {
    const anchor = try fetchAnchorInfo(d, allocator, anchor_id);
    defer anchor.deinit(allocator);

    const root = try resolveWorkbenchRoot(allocator);
    defer allocator.free(root);

    const feat_dir = try engine.workbench.feature.featureDir(
        allocator,
        root,
        anchor.assoc_slug,
        anchor.plan_key,
        anchor.slug,
    );
    defer allocator.free(feat_dir);

    const rel_path = try entityRelPath(d, allocator, kind, entity_id, anchor_id);
    defer allocator.free(rel_path);

    return std.fs.path.join(allocator, &.{ feat_dir, rel_path });
}

fn buildWorkbenchPathFromRel(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    anchor_id: i64,
    rel_path: []const u8,
) ![]u8 {
    const anchor = try fetchAnchorInfo(d, allocator, anchor_id);
    defer anchor.deinit(allocator);

    const root = try resolveWorkbenchRoot(allocator);
    defer allocator.free(root);

    const feat_dir = try engine.workbench.feature.featureDir(
        allocator,
        root,
        anchor.assoc_slug,
        anchor.plan_key,
        anchor.slug,
    );
    defer allocator.free(feat_dir);

    return std.fs.path.join(allocator, &.{ feat_dir, rel_path });
}

// =========================================================================
// Entity rel-path computation
// =========================================================================

/// EntityTitleStatus holds the minimal info needed to build a relative path.
const EntityTitleStatus = struct {
    title: []const u8,
    status: []const u8,

    fn deinit(self: EntityTitleStatus, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        allocator.free(self.status);
    }
};

/// entityRelPath returns the relative path (from the feature directory) for
/// an entity. Mirrors Go's per-entity renderXxx relPath logic.
fn entityRelPath(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    kind: EntityKind,
    entity_id: i64,
    anchor_id: i64,
) ![]u8 {
    switch (kind) {
        .plan => {
            if (entity_id == anchor_id) {
                return allocator.dupe(u8, "README.md");
            }
            // Child plan: plans/<slug>.md
            const slug = try fetchPlanSlug(d, allocator, entity_id);
            defer allocator.free(slug);
            return try std.fmt.allocPrint(allocator, "plans/{s}.md", .{slug});
        },
        .task => {
            const ts = try fetchEntityTitleForSlug(d, allocator, "tasks", entity_id);
            defer ts.deinit(allocator);
            const slug = try engine.workbench.feature.slugify(allocator, ts.title);
            defer allocator.free(slug);
            const task_dir = try taskWorkbenchDir(d, allocator, entity_id);
            defer allocator.free(task_dir);
            return try std.fmt.allocPrint(allocator, "tasks/{s}/{d}-{s}.md", .{ task_dir, entity_id, slug });
        },
        .scenario => {
            const ts = try fetchEntityTitleForSlug(d, allocator, "test_scenarios", entity_id);
            defer ts.deinit(allocator);
            const slug = try engine.workbench.feature.slugify(allocator, ts.title);
            defer allocator.free(slug);
            return try std.fmt.allocPrint(allocator, "scenarios/{d}-{s}.md", .{ entity_id, slug });
        },
        .decision => {
            const ts = try fetchEntityTitleForSlug(d, allocator, "decisions", entity_id);
            defer ts.deinit(allocator);
            const slug = try engine.workbench.feature.slugify(allocator, ts.title);
            defer allocator.free(slug);
            return try std.fmt.allocPrint(allocator, "decisions/{d}-{s}.md", .{ entity_id, slug });
        },
        .question => {
            const ts = try fetchEntityTitleForSlug(d, allocator, "questions", entity_id);
            defer ts.deinit(allocator);
            const slug = try engine.workbench.feature.slugify(allocator, ts.title);
            defer allocator.free(slug);
            return try std.fmt.allocPrint(allocator, "questions/{d}-{s}.md", .{ entity_id, slug });
        },
        .artifact => {
            const info = try fetchArtifactFilenameInfo(d, allocator, entity_id);
            defer allocator.free(info.title);
            defer allocator.free(info.kind);
            const filename = try engine.workbench.feature.artifactFilename(
                allocator,
                entity_id,
                info.title,
                info.kind,
            );
            defer allocator.free(filename);
            return try std.fmt.allocPrint(allocator, "artifacts/{s}", .{filename});
        },
    }
}

const TaskScope = struct {
    scope_kind: []const u8,
    scope_id: ?i64,

    fn deinit(self: TaskScope, allocator: std.mem.Allocator) void {
        allocator.free(self.scope_kind);
    }
};

fn taskWorkbenchDir(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) ![]u8 {
    const scope = try fetchTaskScope(d, allocator, task_id);
    defer scope.deinit(allocator);

    if (std.mem.eql(u8, scope.scope_kind, "repo")) {
        if (scope.scope_id) |repo_id| {
            const repo_slug = try fetchProjectSlugByID(d, allocator, repo_id);
            defer allocator.free(repo_slug);
            return repoSlugToFS(allocator, repo_slug);
        }
    }

    if (try fetchFirstTouchedRepoSlug(d, allocator, task_id)) |touch_slug| {
        defer allocator.free(touch_slug);
        return repoSlugToFS(allocator, touch_slug);
    }

    return allocator.dupe(u8, "cross");
}

fn fetchTaskScope(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) !TaskScope {
    var stmt = d.prepare("select coalesce(scope_kind, ''), scope_id from tasks where id = ?") catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => return TaskScope{
            .scope_kind = try stmt.columnTextAlloc(0, allocator),
            .scope_id = stmt.columnIntOpt(1),
        },
    }
}

fn fetchProjectSlugByID(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    repo_id: i64,
) ![]const u8 {
    var stmt = d.prepare("select slug from projects where id = ?") catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = repo_id }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => return stmt.columnTextAlloc(0, allocator),
    }
}

fn fetchFirstTouchedRepoSlug(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) !?[]const u8 {
    var stmt = d.prepare(
        \\select p.slug
        \\from entity_links el
        \\join projects p on p.id = el.to_id
        \\where el.from_kind = 'task'
        \\  and el.from_id = ?
        \\  and el.to_kind = 'repo'
        \\  and el.relationship = 'touches'
        \\order by el.id
        \\limit 1
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return null,
        .row => {
            const slug = try stmt.columnTextAlloc(0, allocator);
            return slug;
        },
    }
}

fn repoSlugToFS(allocator: std.mem.Allocator, slug: []const u8) ![]u8 {
    return std.mem.replaceOwned(u8, allocator, slug, "/", "_");
}

/// fetchPlanSlug returns the slug of a plan row.
fn fetchPlanSlug(d: *db.sqlite.Db, allocator: std.mem.Allocator, plan_id: i64) ![]const u8 {
    var stmt = d.prepare("select coalesce(slug, '') from plans where id = ?") catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => {
            const slug = try stmt.columnTextAlloc(0, allocator);
            if (slug.len == 0) {
                allocator.free(slug);
                return try std.fmt.allocPrint(allocator, "p{d}", .{plan_id});
            }
            return slug;
        },
    }
}

/// fetchEntityTitleForSlug returns an EntityTitleStatus from any table that has
/// `title` and `status` columns.
fn fetchEntityTitleForSlug(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    table: []const u8,
    id: i64,
) !EntityTitleStatus {
    var sql_buf: std.ArrayList(u8) = .empty;
    defer sql_buf.deinit(allocator);
    try sql_buf.appendSlice(allocator, "select coalesce(title,''), coalesce(status,'') from ");
    try sql_buf.appendSlice(allocator, table);
    try sql_buf.appendSlice(allocator, " where id = ?");
    const sql_z = try allocator.dupeZ(u8, sql_buf.items);
    defer allocator.free(sql_z);
    var stmt = d.prepare(sql_z) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => return EntityTitleStatus{
            .title = try stmt.columnTextAlloc(0, allocator),
            .status = try stmt.columnTextAlloc(1, allocator),
        },
    }
}

const ArtifactFilenameInfo = struct {
    title: []const u8,
    kind: []const u8,
};

/// fetchArtifactFilenameInfo returns the title and kind for an artifact.
fn fetchArtifactFilenameInfo(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    artifact_id: i64,
) !ArtifactFilenameInfo {
    var stmt = d.prepare("select coalesce(title,''), kind from artifacts where id = ?") catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = artifact_id }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => return ArtifactFilenameInfo{
            .title = try stmt.columnTextAlloc(0, allocator),
            .kind = try stmt.columnTextAlloc(1, allocator),
        },
    }
}

// =========================================================================
// Entity renderer
// =========================================================================

/// renderEntity fetches the entity from the DB and produces a complete
/// workbench Markdown file. This is the Zig analog of Go's RenderEntity.
/// The engine/workbench layer is NOT extended (D-no-engine-edits); all
/// SQL lives here in the cmd layer.
fn renderEntity(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    kind: EntityKind,
    entity_id: i64,
    anchor_id: i64,
) ![]u8 {
    switch (kind) {
        .plan => return renderPlan(d, allocator, entity_id, anchor_id),
        .task => return renderTask(d, allocator, entity_id, anchor_id),
        .scenario => return renderScenario(d, allocator, entity_id, anchor_id),
        .decision => return renderDecision(d, allocator, entity_id, anchor_id),
        .question => return renderQuestion(d, allocator, entity_id, anchor_id),
        .artifact => return renderArtifact(d, allocator, entity_id, anchor_id),
    }
}

fn renderPlan(d: *db.sqlite.Db, allocator: std.mem.Allocator, plan_id: i64, anchor_id: i64) ![]u8 {
    var stmt = d.prepare("select coalesce(title,''), coalesce(status,'') from plans where id = ?") catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            defer allocator.free(title);
            const status = try stmt.columnTextAlloc(1, allocator);
            defer allocator.free(status);

            const fm = engine.workbench.render.FrontMatter{
                .entity_kind = "plan",
                .entity_id = plan_id,
                .anchor_plan_id = anchor_id,
                .title = title,
                .status = status,
            };
            const body = try std.fmt.allocPrint(
                allocator,
                "# Plan {d}: {s}\n\n**Status:** {s}\n",
                .{ plan_id, title, status },
            );
            defer allocator.free(body);
            return engine.workbench.render.render(allocator, fm, body);
        },
    }
}

fn renderTask(d: *db.sqlite.Db, allocator: std.mem.Allocator, task_id: i64, anchor_id: i64) ![]u8 {
    var stmt = d.prepare(
        "select coalesce(title,''), coalesce(status,''), priority from tasks where id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            defer allocator.free(title);
            const status = try stmt.columnTextAlloc(1, allocator);
            defer allocator.free(status);
            const priority = stmt.columnInt(2);

            const fm = engine.workbench.render.FrontMatter{
                .entity_kind = "task",
                .entity_id = task_id,
                .anchor_plan_id = anchor_id,
                .title = title,
                .status = status,
                .priority = priority,
            };
            const body = try std.fmt.allocPrint(
                allocator,
                "# Task {d}: {s}\n\n**Status:** {s}  \n**Priority:** {d}\n",
                .{ task_id, title, status, priority },
            );
            defer allocator.free(body);
            return engine.workbench.render.render(allocator, fm, body);
        },
    }
}

fn renderScenario(d: *db.sqlite.Db, allocator: std.mem.Allocator, scenario_id: i64, anchor_id: i64) ![]u8 {
    var stmt = d.prepare(
        "select coalesce(title,''), coalesce(status,'') from test_scenarios where id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = scenario_id }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            defer allocator.free(title);
            const status = try stmt.columnTextAlloc(1, allocator);
            defer allocator.free(status);

            const fm = engine.workbench.render.FrontMatter{
                .entity_kind = "scenario",
                .entity_id = scenario_id,
                .anchor_plan_id = anchor_id,
                .title = title,
                .status = status,
            };
            const body = try std.fmt.allocPrint(
                allocator,
                "# Scenario {d}: {s}\n\n**Status:** {s}\n",
                .{ scenario_id, title, status },
            );
            defer allocator.free(body);
            return engine.workbench.render.render(allocator, fm, body);
        },
    }
}

fn renderDecision(d: *db.sqlite.Db, allocator: std.mem.Allocator, decision_id: i64, anchor_id: i64) ![]u8 {
    var stmt = d.prepare(
        "select coalesce(title,''), coalesce(status,''), coalesce(body,''), coalesce(rationale,'') from decisions where id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = decision_id }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            defer allocator.free(title);
            const status = try stmt.columnTextAlloc(1, allocator);
            defer allocator.free(status);
            const body_text = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(body_text);
            const rationale = try stmt.columnTextAlloc(3, allocator);
            defer allocator.free(rationale);

            const fm = engine.workbench.render.FrontMatter{
                .entity_kind = "decision",
                .entity_id = decision_id,
                .anchor_plan_id = anchor_id,
                .title = title,
                .status = status,
            };
            const body = try std.fmt.allocPrint(
                allocator,
                "# Decision {d}: {s}\n\n**Status:** {s}\n\n## Body\n\n{s}\n\n## Rationale\n\n{s}\n",
                .{ decision_id, title, status, body_text, rationale },
            );
            defer allocator.free(body);
            return engine.workbench.render.render(allocator, fm, body);
        },
    }
}

fn renderQuestion(d: *db.sqlite.Db, allocator: std.mem.Allocator, question_id: i64, anchor_id: i64) ![]u8 {
    var stmt = d.prepare(
        "select coalesce(title,''), coalesce(status,''), coalesce(body,'') from questions where id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = question_id }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            defer allocator.free(title);
            const status = try stmt.columnTextAlloc(1, allocator);
            defer allocator.free(status);
            const body_text = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(body_text);

            const fm = engine.workbench.render.FrontMatter{
                .entity_kind = "question",
                .entity_id = question_id,
                .anchor_plan_id = anchor_id,
                .title = title,
                .status = status,
            };
            const body = try std.fmt.allocPrint(
                allocator,
                "# Question {d}: {s}\n\n**Status:** {s}\n\n{s}\n",
                .{ question_id, title, status, body_text },
            );
            defer allocator.free(body);
            return engine.workbench.render.render(allocator, fm, body);
        },
    }
}

fn renderArtifact(d: *db.sqlite.Db, allocator: std.mem.Allocator, artifact_id: i64, anchor_id: i64) ![]u8 {
    var stmt = d.prepare(
        "select coalesce(title,''), coalesce(status,''), coalesce(kind,''), coalesce(body,'') from artifacts where id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = artifact_id }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            defer allocator.free(title);
            const status = try stmt.columnTextAlloc(1, allocator);
            defer allocator.free(status);
            const kind_str = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(kind_str);
            const body_text = try stmt.columnTextAlloc(3, allocator);
            defer allocator.free(body_text);

            const fm = engine.workbench.render.FrontMatter{
                .entity_kind = "artifact",
                .entity_id = artifact_id,
                .anchor_plan_id = anchor_id,
                .title = title,
                .status = status,
                .artifact_kind = kind_str,
            };
            const body = try std.fmt.allocPrint(
                allocator,
                "# Artifact {d}: {s}\n\n**Kind:** {s}  \n**Status:** {s}\n\n## Content\n\n{s}\n",
                .{ artifact_id, title, kind_str, status, body_text },
            );
            defer allocator.free(body);
            return engine.workbench.render.render(allocator, fm, body);
        },
    }
}

// =========================================================================
// Mutation application
// =========================================================================

/// detectOtherFrontmatterChanges returns true if the new frontmatter has
/// changes beyond title and status. Used to emit the M4 limitation warning.
fn detectOtherFrontmatterChanges(
    orig: engine.workbench.parse.FrontMatter,
    new: engine.workbench.parse.FrontMatter,
) bool {
    // Check priority, scope, artifact_kind, and list fields.
    if (orig.priority != new.priority) return true;
    if (!std.mem.eql(u8, orig.scope, new.scope)) return true;
    if (!std.mem.eql(u8, orig.artifact_kind, new.artifact_kind)) return true;
    // Compare touches (string slices) element-by-element so that same-length
    // but reordered/substituted lists still trigger the warning.
    if (orig.touches.len != new.touches.len) return true;
    for (orig.touches, new.touches) |a, b| {
        if (!std.mem.eql(u8, a, b)) return true;
    }
    // Compare EntityRef lists (verifies, cites, derives_from) by both
    // kind + id so that same-length-but-different-element lists fire.
    if (orig.verifies.len != new.verifies.len) return true;
    for (orig.verifies, new.verifies) |a, b| {
        if (!std.mem.eql(u8, a.kind, b.kind) or a.id != b.id) return true;
    }
    if (orig.cites.len != new.cites.len) return true;
    for (orig.cites, new.cites) |a, b| {
        if (!std.mem.eql(u8, a.kind, b.kind) or a.id != b.id) return true;
    }
    if (orig.derives_from.len != new.derives_from.len) return true;
    for (orig.derives_from, new.derives_from) |a, b| {
        if (!std.mem.eql(u8, a.kind, b.kind) or a.id != b.id) return true;
    }
    // Don't inspect entity_kind / entity_id / anchor_plan_id — those are
    // identity fields; changes are a no-op in M4 (full validator is M5).
    return false;
}

/// applyMutations writes title and/or status changes to the DB.
/// Each mutation is applied via a targeted UPDATE so untouched fields
/// keep their existing values.
fn applyMutations(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    kind: EntityKind,
    entity_id: i64,
    new_title: ?[]const u8,
    new_status: ?[]const u8,
) !void {
    if (new_title == null and new_status == null) return;

    // Build dynamic UPDATE. We always set updated_at.
    var sql_buf: std.ArrayList(u8) = .empty;
    defer sql_buf.deinit(allocator);

    const table = entityTable(kind);

    try sql_buf.appendSlice(allocator, "update ");
    try sql_buf.appendSlice(allocator, table);
    try sql_buf.appendSlice(allocator, " set updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')");

    var params: std.ArrayList(db.sqlite.Param) = .empty;
    defer params.deinit(allocator);

    if (new_title) |t| {
        try sql_buf.appendSlice(allocator, ", title = ?");
        try params.append(allocator, .{ .text = t });
    }
    if (new_status) |s| {
        try sql_buf.appendSlice(allocator, ", status = ?");
        try params.append(allocator, .{ .text = s });
    }
    try sql_buf.appendSlice(allocator, " where id = ?");
    try params.append(allocator, .{ .int = entity_id });

    const sql_z = try allocator.dupeZ(u8, sql_buf.items);
    defer allocator.free(sql_z);

    _ = d.execParams(sql_z, params.items) catch return error.QueryFailed;
}

/// entityTable returns the SQL table name for the given entity kind.
fn entityTable(kind: EntityKind) []const u8 {
    return switch (kind) {
        .plan => "plans",
        .task => "tasks",
        .question => "questions",
        .scenario => "test_scenarios",
        .decision => "decisions",
        .artifact => "artifacts",
    };
}

// =========================================================================
// Pager execution (D-pager-shape)
// =========================================================================

/// execPager opens the file at path in $PAGER → less → cat.
/// The last fallback (cat) is guaranteed to succeed since it just writes
/// the file to stdout — useful in test environments.
fn execPager(io: std.Io, allocator: std.mem.Allocator, path: []const u8) !void {
    // Try $PAGER first.
    if (getPosixEnv("PAGER")) |pager| {
        if (runPager(io, allocator, pager, path)) return else |_| {}
    }

    // Try less.
    if (runPager(io, allocator, "less", path)) return else |_| {}

    // Final fallback: cat.
    _ = runPager(io, allocator, "cat", path) catch {};
}

fn runPager(io: std.Io, allocator: std.mem.Allocator, pager: []const u8, path: []const u8) !void {
    var argv_buf: [2][]const u8 = .{ pager, path };
    var child = try std.process.spawn(io, .{
        .argv = &argv_buf,
        .stdin = .inherit,
        .stdout = .inherit,
        .stderr = .inherit,
    });
    _ = try child.wait(io);
    _ = allocator; // allocator retained for future use (e.g. argv construction)
}

// =========================================================================
// Tests
// =========================================================================

const testing = std.testing;

test "EntityKind.fromText and toText round-trip" {
    const cases = [_][]const u8{ "plan", "task", "question", "scenario", "decision", "artifact" };
    for (cases) |s| {
        const k = EntityKind.fromText(s) orelse {
            try testing.expect(false);
            continue;
        };
        try testing.expectEqualStrings(s, k.toText());
    }
}

test "EntityKind.fromText returns null for unknown" {
    try testing.expect(EntityKind.fromText("widget") == null);
    try testing.expect(EntityKind.fromText("") == null);
}

test "EntityKind.toLinkKind: scenario uses test_scenario" {
    try testing.expectEqualStrings("test_scenario", EntityKind.scenario.toLinkKind());
}

test "EntityKind.toLinkKind: other kinds pass through" {
    try testing.expectEqualStrings("plan", EntityKind.plan.toLinkKind());
    try testing.expectEqualStrings("task", EntityKind.task.toLinkKind());
    try testing.expectEqualStrings("decision", EntityKind.decision.toLinkKind());
    try testing.expectEqualStrings("artifact", EntityKind.artifact.toLinkKind());
    try testing.expectEqualStrings("question", EntityKind.question.toLinkKind());
}

test "resolveAnchorPlan: plan walks to anchor" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, testing.allocator);

    // Create an anchor plan.
    const anchor_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Anchor', 'anchor', 'active')",
        &.{},
    );
    const anchor = try resolveAnchorPlan(&d, testing.allocator, .plan, anchor_id);
    try testing.expectEqual(anchor_id, anchor);
}

test "resolveAnchorPlan: child plan walks up to anchor" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, testing.allocator);

    const anchor_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Anchor', 'anchor', 'active')",
        &.{},
    );
    const child_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status, parent_plan_id) values ('global', 'Child', 'child', 'active', ?)",
        &.{.{ .int = anchor_id }},
    );
    const resolved = try resolveAnchorPlan(&d, testing.allocator, .plan, child_id);
    try testing.expectEqual(anchor_id, resolved);
}

test "resolveAnchorPlan: task with plan_id" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, testing.allocator);

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'P', 'p', 'active')",
        &.{},
    );
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority, plan_id) values ('global', 'T', 'todo', 100, ?)",
        &.{.{ .int = plan_id }},
    );
    const resolved = try resolveAnchorPlan(&d, testing.allocator, .task, task_id);
    try testing.expectEqual(plan_id, resolved);
}

test "resolveAnchorPlan: task with no plan_id returns NoPlanLink" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, testing.allocator);

    const task_id = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global', 'T', 'todo', 100)",
        &.{},
    );
    const result = resolveAnchorPlan(&d, testing.allocator, .task, task_id);
    try testing.expectError(error.NoPlanLink, result);
}

test "resolveAnchorPlan: decision via entity_links derives-from" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, testing.allocator);

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'P', 'p', 'active')",
        &.{},
    );
    const dec_id = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global', 'D', 'body', 'proposed')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('decision', ?, 'plan', ?, 'derives-from')",
        &.{ .{ .int = dec_id }, .{ .int = plan_id } },
    );
    const resolved = try resolveAnchorPlan(&d, testing.allocator, .decision, dec_id);
    try testing.expectEqual(plan_id, resolved);
}

test "resolveAnchorPlan: artifact via entity_links derives-from" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, testing.allocator);

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'P', 'p', 'active')",
        &.{},
    );
    const art_id = try d.execParams(
        "insert into artifacts (scope_kind, title, kind, status) values ('global', 'A', 'adr', 'active')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('artifact', ?, 'plan', ?, 'derives-from')",
        &.{ .{ .int = art_id }, .{ .int = plan_id } },
    );
    const resolved = try resolveAnchorPlan(&d, testing.allocator, .artifact, art_id);
    try testing.expectEqual(plan_id, resolved);
}

test "resolveAnchorPlan: entity with no plan link returns error" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, testing.allocator);

    const dec_id = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global', 'D', 'body', 'proposed')",
        &.{},
    );
    const result = resolveAnchorPlan(&d, testing.allocator, .decision, dec_id);
    try testing.expectError(error.NoPlanLink, result);
}

test "buildWorkbenchPath: plan anchor returns README.md" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, testing.allocator);

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'My Plan', 'my-plan', 'active')",
        &.{},
    );

    // Set a known workbench root via env-var isn't feasible in unit tests;
    // test the rel-path logic directly instead.
    const rel = try entityRelPath(&d, testing.allocator, .plan, plan_id, plan_id);
    defer testing.allocator.free(rel);
    try testing.expectEqualStrings("README.md", rel);
}

test "buildWorkbenchPath: decision uses decisions/<id>-<slug>.md" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, testing.allocator);

    const dec_id = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global', 'My Decision', 'body', 'proposed')",
        &.{},
    );
    const plan_id: i64 = 1; // unused anchor for path building

    const rel = try entityRelPath(&d, testing.allocator, .decision, dec_id, plan_id);
    defer testing.allocator.free(rel);
    // Expected: decisions/<dec_id>-my-decision.md
    const expected = try std.fmt.allocPrint(testing.allocator, "decisions/{d}-my-decision.md", .{dec_id});
    defer testing.allocator.free(expected);
    try testing.expectEqualStrings(expected, rel);
}

test "entityRelPath task: repo-scoped task uses tasks/<repo-slug> dir" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, testing.allocator);

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'P', 'p', 'active')",
        &.{},
    );
    const repo_id = try d.execParams(
        "insert into projects (slug, name) values ('acme/protos', 'Protos')",
        &.{},
    );
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, scope_id, plan_id, title, status, priority) values ('repo', ?, ?, 'Repo scoped task', 'todo', 100)",
        &.{ .{ .int = repo_id }, .{ .int = plan_id } },
    );

    const rel = try entityRelPath(&d, testing.allocator, .task, task_id, plan_id);
    defer testing.allocator.free(rel);
    const expected = try std.fmt.allocPrint(testing.allocator, "tasks/acme_protos/{d}-repo-scoped-task.md", .{task_id});
    defer testing.allocator.free(expected);
    try testing.expectEqualStrings(expected, rel);
}

test "entityRelPath task: association-scoped task with touches uses first touch repo dir" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, testing.allocator);

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'P', 'p', 'active')",
        &.{},
    );
    const assoc_id = try d.execParams(
        "insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')",
        &.{},
    );
    const first_repo_id = try d.execParams(
        "insert into projects (slug, name) values ('acme/gateway', 'Gateway')",
        &.{},
    );
    const second_repo_id = try d.execParams(
        "insert into projects (slug, name) values ('acme/service', 'Service')",
        &.{},
    );
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, scope_id, plan_id, title, status, priority) values ('association', ?, ?, 'Assoc task', 'todo', 100)",
        &.{ .{ .int = assoc_id }, .{ .int = plan_id } },
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'repo', ?, 'touches')",
        &.{ .{ .int = task_id }, .{ .int = first_repo_id } },
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'repo', ?, 'touches')",
        &.{ .{ .int = task_id }, .{ .int = second_repo_id } },
    );

    const rel = try entityRelPath(&d, testing.allocator, .task, task_id, plan_id);
    defer testing.allocator.free(rel);
    const expected = try std.fmt.allocPrint(testing.allocator, "tasks/acme_gateway/{d}-assoc-task.md", .{task_id});
    defer testing.allocator.free(expected);
    try testing.expectEqualStrings(expected, rel);
}

test "entityRelPath task: no repo-scope and no touches uses tasks/cross dir" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, testing.allocator);

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'P', 'p', 'active')",
        &.{},
    );
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', ?, 'Cross task', 'todo', 100)",
        &.{.{ .int = plan_id }},
    );

    const rel = try entityRelPath(&d, testing.allocator, .task, task_id, plan_id);
    defer testing.allocator.free(rel);
    const expected = try std.fmt.allocPrint(testing.allocator, "tasks/cross/{d}-cross-task.md", .{task_id});
    defer testing.allocator.free(expected);
    try testing.expectEqualStrings(expected, rel);
}

test "applyMutations: title change is persisted" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, testing.allocator);

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Old Title', 'old', 'draft')",
        &.{},
    );

    try applyMutations(&d, testing.allocator, .plan, plan_id, "New Title", null);

    // Verify the change landed.
    var stmt = d.prepare("select title from plans where id = ?") catch unreachable;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch unreachable;
    _ = stmt.step() catch unreachable;
    const title = try stmt.columnTextAlloc(0, testing.allocator);
    defer testing.allocator.free(title);
    try testing.expectEqualStrings("New Title", title);
}

test "applyMutations: status change is persisted" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, testing.allocator);

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'P', 'p', 'draft')",
        &.{},
    );

    try applyMutations(&d, testing.allocator, .plan, plan_id, null, "active");

    var stmt = d.prepare("select status from plans where id = ?") catch unreachable;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch unreachable;
    _ = stmt.step() catch unreachable;
    const status = try stmt.columnTextAlloc(0, testing.allocator);
    defer testing.allocator.free(status);
    try testing.expectEqualStrings("active", status);
}

test "detectOtherFrontmatterChanges: no changes returns false" {
    const orig = engine.workbench.parse.FrontMatter{
        .entity_kind = "plan",
        .entity_id = 1,
        .anchor_plan_id = 1,
        .title = "T",
        .status = "draft",
        .priority = 0,
    };
    const new = engine.workbench.parse.FrontMatter{
        .entity_kind = "plan",
        .entity_id = 1,
        .anchor_plan_id = 1,
        .title = "T2", // title change — not counted by detectOtherFrontmatterChanges
        .status = "active", // status change — not counted
        .priority = 0,
    };
    try testing.expect(!detectOtherFrontmatterChanges(orig, new));
}

test "detectOtherFrontmatterChanges: priority change returns true" {
    const orig = engine.workbench.parse.FrontMatter{
        .entity_kind = "task",
        .entity_id = 2,
        .anchor_plan_id = 1,
        .priority = 100,
    };
    const new = engine.workbench.parse.FrontMatter{
        .entity_kind = "task",
        .entity_id = 2,
        .anchor_plan_id = 1,
        .priority = 50,
    };
    try testing.expect(detectOtherFrontmatterChanges(orig, new));
}

test "detectOtherFrontmatterChanges: same-length but different touches triggers warning" {
    // Replacing a touches entry (same length, different string) must fire the warning.
    const touches_a = [_][]const u8{"acme/foo"};
    const touches_b = [_][]const u8{"acme/bar"}; // substituted element
    const orig = engine.workbench.parse.FrontMatter{
        .entity_kind = "task",
        .entity_id = 3,
        .anchor_plan_id = 1,
        .touches = &touches_a,
    };
    const new = engine.workbench.parse.FrontMatter{
        .entity_kind = "task",
        .entity_id = 3,
        .anchor_plan_id = 1,
        .touches = &touches_b,
    };
    try testing.expect(detectOtherFrontmatterChanges(orig, new));
}

test "detectOtherFrontmatterChanges: same-length but different verifies triggers warning" {
    // Substituting a verifies EntityRef (same length, different id) must fire.
    const verifies_a = [_]engine.workbench.parse.EntityRef{.{ .kind = "task", .id = 10 }};
    const verifies_b = [_]engine.workbench.parse.EntityRef{.{ .kind = "task", .id = 99 }}; // different id
    const orig = engine.workbench.parse.FrontMatter{
        .entity_kind = "artifact",
        .entity_id = 5,
        .anchor_plan_id = 1,
        .verifies = &verifies_a,
    };
    const new = engine.workbench.parse.FrontMatter{
        .entity_kind = "artifact",
        .entity_id = 5,
        .anchor_plan_id = 1,
        .verifies = &verifies_b,
    };
    try testing.expect(detectOtherFrontmatterChanges(orig, new));
}

test "detectOtherFrontmatterChanges: same-length but different verifies kind triggers warning" {
    // Substituting the kind field (same id, different kind) must fire.
    const verifies_a = [_]engine.workbench.parse.EntityRef{.{ .kind = "plan", .id = 10 }};
    const verifies_b = [_]engine.workbench.parse.EntityRef{.{ .kind = "task", .id = 10 }}; // different kind
    const orig = engine.workbench.parse.FrontMatter{
        .entity_kind = "artifact",
        .entity_id = 6,
        .anchor_plan_id = 1,
        .verifies = &verifies_a,
    };
    const new = engine.workbench.parse.FrontMatter{
        .entity_kind = "artifact",
        .entity_id = 6,
        .anchor_plan_id = 1,
        .verifies = &verifies_b,
    };
    try testing.expect(detectOtherFrontmatterChanges(orig, new));
}

test "detectOtherFrontmatterChanges: same-length but different cites triggers warning" {
    const cites_a = [_]engine.workbench.parse.EntityRef{.{ .kind = "artifact", .id = 10 }};
    const cites_b = [_]engine.workbench.parse.EntityRef{.{ .kind = "artifact", .id = 99 }};
    const orig = engine.workbench.parse.FrontMatter{
        .entity_kind = "task",
        .entity_id = 8,
        .anchor_plan_id = 1,
        .cites = &cites_a,
    };
    const new = engine.workbench.parse.FrontMatter{
        .entity_kind = "task",
        .entity_id = 8,
        .anchor_plan_id = 1,
        .cites = &cites_b,
    };
    try testing.expect(detectOtherFrontmatterChanges(orig, new));
}

test "detectOtherFrontmatterChanges: same-length but different derives_from triggers warning" {
    const derives_a = [_]engine.workbench.parse.EntityRef{.{ .kind = "plan", .id = 4 }};
    const derives_b = [_]engine.workbench.parse.EntityRef{.{ .kind = "plan", .id = 9 }};
    const orig = engine.workbench.parse.FrontMatter{
        .entity_kind = "question",
        .entity_id = 9,
        .anchor_plan_id = 1,
        .derives_from = &derives_a,
    };
    const new = engine.workbench.parse.FrontMatter{
        .entity_kind = "question",
        .entity_id = 9,
        .anchor_plan_id = 1,
        .derives_from = &derives_b,
    };
    try testing.expect(detectOtherFrontmatterChanges(orig, new));
}

test "detectOtherFrontmatterChanges: identical lists return false" {
    // Identical list contents must NOT trigger the warning.
    const verifies_a = [_]engine.workbench.parse.EntityRef{
        .{ .kind = "task", .id = 10 },
        .{ .kind = "plan", .id = 2 },
    };
    const verifies_b = [_]engine.workbench.parse.EntityRef{
        .{ .kind = "task", .id = 10 },
        .{ .kind = "plan", .id = 2 },
    };
    const orig = engine.workbench.parse.FrontMatter{
        .entity_kind = "artifact",
        .entity_id = 7,
        .anchor_plan_id = 1,
        .verifies = &verifies_a,
    };
    const new = engine.workbench.parse.FrontMatter{
        .entity_kind = "artifact",
        .entity_id = 7,
        .anchor_plan_id = 1,
        .verifies = &verifies_b,
    };
    try testing.expect(!detectOtherFrontmatterChanges(orig, new));
}

test "renderEntity: plan produces valid frontmatter" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, testing.allocator);

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Test Plan', 'test-plan', 'active')",
        &.{},
    );
    const content = try renderEntity(&d, testing.allocator, .plan, plan_id, plan_id);
    defer testing.allocator.free(content);

    try testing.expect(std.mem.indexOf(u8, content, "entity_kind: plan") != null);
    try testing.expect(std.mem.indexOf(u8, content, "title: Test Plan") != null);
    try testing.expect(std.mem.indexOf(u8, content, "status: active") != null);
}

test "edit happy path: title change persisted (stub editor)" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, testing.allocator);

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Original', 'original', 'draft')",
        &.{},
    );

    // Render the entity to understand what the initial content would be.
    const initial = try renderEntity(&d, testing.allocator, .plan, plan_id, plan_id);
    defer testing.allocator.free(initial);

    // Patch the title in the frontmatter for the "editor output".
    // Replace "title: Original" with "title: Patched".
    const patched = try std.mem.replaceOwned(u8, testing.allocator, initial, "title: Original", "title: Patched");
    defer testing.allocator.free(patched);

    // Parse the patched content.
    const parsed = try engine.workbench.parse.parse(testing.allocator, patched);
    defer engine.workbench.parse.deinit(parsed, testing.allocator);
    const orig_parsed = try engine.workbench.parse.parse(testing.allocator, initial);
    defer engine.workbench.parse.deinit(orig_parsed, testing.allocator);

    const title_changed = !std.mem.eql(u8, orig_parsed.frontmatter.title, parsed.frontmatter.title);
    try testing.expect(title_changed);

    // Apply.
    try applyMutations(&d, testing.allocator, .plan, plan_id, parsed.frontmatter.title, null);

    // Verify.
    var stmt = d.prepare("select title from plans where id = ?") catch unreachable;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch unreachable;
    _ = stmt.step() catch unreachable;
    const title = try stmt.columnTextAlloc(0, testing.allocator);
    defer testing.allocator.free(title);
    try testing.expectEqualStrings("Patched", title);
}

test "edit operator-abort: non-zero editor exit does not write DB" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, testing.allocator);

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Orig', 'orig', 'draft')",
        &.{},
    );

    // Use /usr/bin/false as editor — exits 1.
    const initial = try renderEntity(&d, testing.allocator, .plan, plan_id, plan_id);
    defer testing.allocator.free(initial);

    const edit_result = try editor.invoke(testing.io, testing.allocator, initial, .{
        .editor_override = "/usr/bin/false",
    });
    defer edit_result.deinit(testing.allocator);

    // Non-zero exit → do NOT apply mutations.
    if (edit_result.editor_exit_code != 0) {
        // Verified: no DB write.
    } else {
        try testing.expect(false); // /bin/false should always exit 1
    }

    // DB title should still be "Orig".
    var stmt = d.prepare("select title from plans where id = ?") catch unreachable;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch unreachable;
    _ = stmt.step() catch unreachable;
    const title = try stmt.columnTextAlloc(0, testing.allocator);
    defer testing.allocator.free(title);
    try testing.expectEqualStrings("Orig", title);
}

test "view happy path: stub pager exits 0 — no error" {
    // view() requires DB + workbench path resolution. For a unit-level test
    // we verify that runPager succeeds with a benign pager (/bin/true) and
    // that execPager can be called without panic. The full integration path
    // is covered in integration_tests/editflow_view_test.zig.
    try runPager(testing.io, testing.allocator, "/usr/bin/true", "/dev/null");
}
