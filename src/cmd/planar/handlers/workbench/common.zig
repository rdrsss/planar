const std = @import("std");
const db = @import("db");
const engine = @import("engine");

pub const ResolvedPlan = struct {
    id: i64,
    slug: []const u8,

    pub fn deinit(self: ResolvedPlan, allocator: std.mem.Allocator) void {
        allocator.free(self.slug);
    }
};

pub const AnchorPathInfo = struct {
    slug: []const u8,
    assoc_slug: []const u8,
    plan_key: []const u8,

    pub fn deinit(self: AnchorPathInfo, allocator: std.mem.Allocator) void {
        allocator.free(self.slug);
        allocator.free(self.assoc_slug);
        allocator.free(self.plan_key);
    }
};

pub fn resolvePlanArg(d: *db.sqlite.Db, allocator: std.mem.Allocator, arg: []const u8) !ResolvedPlan {
    if (std.fmt.parseInt(i64, arg, 10)) |id| {
        return fetchPlanByID(d, allocator, id);
    } else |_| {}
    return fetchPlanBySlug(d, allocator, arg);
}

pub fn resolveFeatureDirForPlan(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    root: []const u8,
    anchor_plan_id: i64,
) ![]const u8 {
    const info = try fetchAnchorPathInfo(d, allocator, anchor_plan_id);
    defer info.deinit(allocator);
    return engine.workbench.feature.featureDir(
        allocator,
        root,
        info.assoc_slug,
        info.plan_key,
        info.slug,
    );
}

pub fn resolveWorkbenchRoot(allocator: std.mem.Allocator) ![]const u8 {
    if (getEnv("PLANAR_WORKBENCH_ROOT")) |root| {
        if (std.mem.startsWith(u8, root, "~/")) {
            const home = getEnv("HOME") orelse return error.HomeNotSet;
            return std.fs.path.join(allocator, &.{ home, root[2..] });
        }
        return allocator.dupe(u8, root);
    }

    const home = getEnv("HOME") orelse return error.HomeNotSet;
    return std.fs.path.join(allocator, &.{ home, ".planar", "workbench" });
}

pub fn resolveAndEnsureWorkbenchRoot(allocator: std.mem.Allocator, io: std.Io) ![]const u8 {
    const root = try resolveWorkbenchRoot(allocator);
    errdefer allocator.free(root);
    try std.Io.Dir.cwd().createDirPath(io, root);
    return root;
}

/// Parse the `--filter-mode {failures,all}` flag value, defaulting to
/// `.failures` when the flag was omitted (null pointer). Returns
/// `error.InvalidInput` for any other string.
pub fn parseFilterMode(raw: ?[]const u8) !engine.workbench.terminal.Mode {
    const s = raw orelse return .failures;
    if (s.len == 0) return .failures;
    return engine.workbench.terminal.Mode.fromString(s) orelse error.InvalidInput;
}

fn fetchPlanByID(d: *db.sqlite.Db, allocator: std.mem.Allocator, plan_id: i64) !ResolvedPlan {
    if (plan_id < 1) return error.InvalidInput;

    var stmt = d.prepare(
        \\select p.id, coalesce(p.slug, '')
        \\from plans p
        \\where p.id = ? and p.parent_plan_id is null
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return error.QueryFailed;

    return switch (stmt.step() catch return error.QueryFailed) {
        .done => error.NotFound,
        .row => .{
            .id = stmt.columnInt(0),
            .slug = try stmt.columnTextAlloc(1, allocator),
        },
    };
}

fn getEnv(key: []const u8) ?[]const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s = std.mem.span(entry);
        if (s.len <= key.len + 1) continue;
        if (s[key.len] != '=') continue;
        if (!std.mem.eql(u8, s[0..key.len], key)) continue;
        const value = s[key.len + 1 ..];
        if (value.len == 0) return null;
        return value;
    }
    return null;
}

fn fetchAnchorPathInfo(d: *db.sqlite.Db, allocator: std.mem.Allocator, plan_id: i64) !AnchorPathInfo {
    var stmt = d.prepare(
        \\select coalesce(p.slug, ''), coalesce(a.slug, '')
        \\from plans p
        \\left join associations a on (p.scope_kind = 'association' and a.id = p.scope_id)
        \\where p.id = ? and p.parent_plan_id is null
        \\limit 1
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return error.QueryFailed;

    return switch (stmt.step() catch return error.QueryFailed) {
        .done => error.NotFound,
        .row => blk: {
            const slug = try stmt.columnTextAlloc(0, allocator);
            errdefer allocator.free(slug);
            const assoc_slug = try stmt.columnTextAlloc(1, allocator);
            errdefer allocator.free(assoc_slug);
            const plan_key = try resolvePlanKey(d, allocator, plan_id);
            break :blk .{
                .slug = slug,
                .assoc_slug = assoc_slug,
                .plan_key = plan_key,
            };
        },
    };
}

fn resolvePlanKey(d: *db.sqlite.Db, allocator: std.mem.Allocator, plan_id: i64) ![]const u8 {
    var stmt = d.prepare(
        \\select coalesce(external_id, '')
        \\from external_links
        \\where entity_kind = 'plan' and entity_id = ?
        \\order by id
        \\limit 1
    ) catch return std.fmt.allocPrint(allocator, "p{d}", .{plan_id});
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return std.fmt.allocPrint(allocator, "p{d}", .{plan_id});

    return switch (stmt.step() catch .done) {
        .done => std.fmt.allocPrint(allocator, "p{d}", .{plan_id}),
        .row => blk: {
            const external_id = try stmt.columnTextAlloc(0, allocator);
            if (external_id.len == 0) {
                allocator.free(external_id);
                break :blk std.fmt.allocPrint(allocator, "p{d}", .{plan_id});
            }
            break :blk external_id;
        },
    };
}

fn fetchPlanBySlug(d: *db.sqlite.Db, allocator: std.mem.Allocator, slug: []const u8) !ResolvedPlan {
    if (slug.len == 0) return error.InvalidInput;

    var stmt = d.prepare(
        \\select p.id, coalesce(p.slug, '')
        \\from plans p
        \\where p.parent_plan_id is null and p.slug = ?
        \\order by p.id
        \\limit 1
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = slug }}) catch return error.QueryFailed;

    return switch (stmt.step() catch return error.QueryFailed) {
        .done => error.NotFound,
        .row => .{
            .id = stmt.columnInt(0),
            .slug = try stmt.columnTextAlloc(1, allocator),
        },
    };
}

pub fn printSyncResult(
    stdout: anytype,
    plan_id: i64,
    plan_slug: []const u8,
    mode: engine.workbench.sync.Mode,
    verb: []const u8,
    result: engine.workbench.sync.Result,
    verbose: bool,
) !void {
    if (verbose) {
        try printSyncResultVerbose(stdout, plan_id, plan_slug, mode, verb, result);
        return;
    }

    try stdout.print(
        "workbench {s}: plan {d} ({s}) - {d} applied, {d} pending, {d} conflict(s)\n",
        .{ verb, plan_id, plan_slug, result.applied, result.pending, result.conflicts },
    );
    try printConflictDetails(stdout, result.entries);
    if (result.conflicts > 0) {
        try stdout.print("  {d} conflict(s) - run 'workbench resolve <event-id> --prefer fs|db'\n", .{result.conflicts});
    }
}

fn printSyncResultVerbose(
    stdout: anytype,
    plan_id: i64,
    plan_slug: []const u8,
    mode: engine.workbench.sync.Mode,
    verb: []const u8,
    result: engine.workbench.sync.Result,
) !void {
    try stdout.print("workbench {s}: plan {d} ({s})\n", .{ verb, plan_id, plan_slug });

    for (result.entries) |entry| {
        switch (entry.class) {
            .no_op => {},
            .fs_to_db => {
                if (mode == .pull or mode == .sync) {
                    try stdout.print("  applied FS->DB: {s}\n", .{entry.file_path});
                } else {
                    try stdout.print("  pending FS->DB: {s}  [run pull to apply]\n", .{entry.file_path});
                }
            },
            .db_to_fs => {
                if (mode == .push or mode == .sync) {
                    try stdout.print("  applied DB->FS: {s}\n", .{entry.file_path});
                } else {
                    try stdout.print("  pending DB->FS: {s}  [run push to apply]\n", .{entry.file_path});
                }
            },
            .conflict => {
                try printConflictLine(stdout, entry);
            },
            .new_on_fs => {
                if (mode == .pull or mode == .sync) {
                    try stdout.print("  new entity: {s} -> {s} {d}\n", .{ entry.file_path, entry.entity_kind, entry.entity_id });
                } else {
                    try stdout.print("  new on FS:  {s}\n", .{entry.file_path});
                }
            },
            .deleted_on_fs => {
                if (mode == .pull or mode == .sync) {
                    try stdout.print("  deleted: {s} -> {s} {d} cancelled\n", .{ entry.file_path, entry.entity_kind, entry.entity_id });
                } else {
                    try stdout.print("  missing: {s}\n", .{entry.file_path});
                }
            },
            .malformed => {
                if (entry.parse_error.len > 0) {
                    try stdout.print("  MALFORMED: {s} ({s})\n", .{ entry.file_path, entry.parse_error });
                } else {
                    try stdout.print("  MALFORMED: {s}\n", .{entry.file_path});
                }
            },
        }
    }

    if (result.conflicts > 0) {
        try stdout.print("  {d} conflict(s) - run 'workbench resolve <event-id> --prefer fs|db'\n", .{result.conflicts});
    }
}

fn printConflictDetails(stdout: anytype, entries: []const engine.workbench.sync.Entry) !void {
    for (entries) |entry| {
        if (entry.class != .conflict) continue;
        try printConflictLine(stdout, entry);
    }
}

fn printConflictLine(stdout: anytype, entry: engine.workbench.sync.Entry) !void {
    if (entry.conflict_id <= 0) return error.InvalidConflictEventID;
    try stdout.print(
        "  CONFLICT [{d}]: {s} ({s} {d})\n",
        .{ entry.conflict_id, entry.file_path, entry.entity_kind, entry.entity_id },
    );
}
