//! engine/identity/association — Association entity + project membership.
//!
//! Associations are the scope namespaces operators tag work with. Each
//! `planar plan|task` row that isn't global belongs to exactly one
//! association via (scope_kind='association', scope_id=<assoc.id>).
//! Many-to-many bridge `project_associations` links projects to the
//! associations they participate in.
//!
//! This module owns:
//!   - associations table CRUD (create, list, showBySlug, showById)
//!   - the project_associations join helpers (addMember, removeMember,
//!     members) that take an association slug + a repo path. The repo
//!     path is resolved to a project row, creating one on the fly when
//!     the path hasn't been registered yet.
//!
//! `detect` (auto-derive associations from cwd via git remote / parent
//! path / language ecosystem) is a separate concern that lives at the
//! CLI layer for now; this module exposes the primitives it needs.

const std = @import("std");
const db = @import("db");
const policy = @import("../policy.zig");

// =========================================================================
// Types
// =========================================================================

pub const Kind = enum {
    org,
    project,
    client,
    personal,
    @"ad-hoc",
    host,
    path,
    lang,

    pub fn fromText(s: []const u8) ?Kind {
        if (std.mem.eql(u8, s, "org")) return .org;
        if (std.mem.eql(u8, s, "project")) return .project;
        if (std.mem.eql(u8, s, "client")) return .client;
        if (std.mem.eql(u8, s, "personal")) return .personal;
        if (std.mem.eql(u8, s, "ad-hoc")) return .@"ad-hoc";
        if (std.mem.eql(u8, s, "host")) return .host;
        if (std.mem.eql(u8, s, "path")) return .path;
        if (std.mem.eql(u8, s, "lang")) return .lang;
        return null;
    }
};

/// One row from the `associations` table. All string fields owned by
/// the allocator passed to the producing function.
pub const Association = struct {
    id: i64,
    slug: []const u8,
    name: []const u8,
    kind: Kind,
    auto_detected: bool,
    config_json: ?[]const u8,
    created_at: []const u8,
    updated_at: []const u8,
};

pub fn deinit(a: Association, allocator: std.mem.Allocator) void {
    allocator.free(a.slug);
    allocator.free(a.name);
    if (a.config_json) |s| allocator.free(s);
    allocator.free(a.created_at);
    allocator.free(a.updated_at);
}

pub fn deinitMany(items: []const Association, allocator: std.mem.Allocator) void {
    for (items) |a| deinit(a, allocator);
    allocator.free(items);
}

/// Minimal projection of the `projects` table — enough for the
/// member-listing case. Full Project + project CRUD will land in
/// `engine/identity/project.zig` when first-class init-by-CLI exists.
pub const Project = struct {
    id: i64,
    slug: []const u8,
    name: []const u8,
    root_path: ?[]const u8,
};

pub fn deinitProject(p: Project, allocator: std.mem.Allocator) void {
    allocator.free(p.slug);
    allocator.free(p.name);
    if (p.root_path) |s| allocator.free(s);
}

pub fn deinitProjects(items: []const Project, allocator: std.mem.Allocator) void {
    for (items) |p| deinitProject(p, allocator);
    allocator.free(items);
}

pub const CreateArgs = struct {
    slug: []const u8,
    /// Defaults to slug if null.
    name: ?[]const u8 = null,
    /// Defaults to `.@"ad-hoc"` if null — matches the Go side's default
    /// for `planar association create` with no --kind.
    kind: ?Kind = null,
};

pub const AddMemberSource = enum {
    user,
    @"auto:git-remote",
    @"auto:path",
    @"auto:lang",

    pub fn toText(self: AddMemberSource) []const u8 {
        return switch (self) {
            .user => "user",
            .@"auto:git-remote" => "auto:git-remote",
            .@"auto:path" => "auto:path",
            .@"auto:lang" => "auto:lang",
        };
    }
};

pub const Error =
    error{
        NotFound,
        SlugConflict,
        UnknownKind,
        AlreadyMember,
        NotAMember,
        QueryFailed,
    } ||
    std.mem.Allocator.Error ||
    policy.scope_guard.Error ||
    policy.audit.Error;

// =========================================================================
// CRUD
// =========================================================================

pub fn create(d: *db.sqlite.Db, allocator: std.mem.Allocator, args: CreateArgs) Error!Association {
    const name = args.name orelse args.slug;
    const kind = args.kind orelse .@"ad-hoc";

    const id = d.execParams(
        \\insert into associations (slug, name, kind) values (?, ?, ?)
    , &.{
        .{ .text = args.slug },
        .{ .text = name },
        .{ .text = @tagName(kind) },
    }) catch |e| {
        if (d.lastWasUniqueViolation()) return Error.SlugConflict;
        std.log.err("association.create exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };

    const summary = try std.fmt.allocPrint(allocator, "create association '{s}'", .{args.slug});
    defer allocator.free(summary);
    try policy.audit.record(d, .{
        .verb = .create,
        .entity = .{ .kind = "association", .id = id },
        .summary = summary,
    });

    return try showById(d, allocator, id);
}

pub fn showById(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!Association {
    var stmt = d.prepare(select_columns_sql ++ " where id = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => try readRow(&stmt, allocator),
    };
}

pub fn showBySlug(d: *db.sqlite.Db, allocator: std.mem.Allocator, slug: []const u8) Error!Association {
    var stmt = d.prepare(select_columns_sql ++ " where slug = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = slug }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => try readRow(&stmt, allocator),
    };
}

pub const ListFilter = struct {
    kind: ?Kind = null,
};

pub fn list(d: *db.sqlite.Db, allocator: std.mem.Allocator, filter: ListFilter) Error![]Association {
    var sql_buf: std.ArrayList(u8) = .empty;
    defer sql_buf.deinit(allocator);
    try sql_buf.appendSlice(allocator, select_columns_sql ++ " where 1 = 1");

    var params: std.ArrayList(db.sqlite.Param) = .empty;
    defer params.deinit(allocator);
    if (filter.kind) |k| {
        try sql_buf.appendSlice(allocator, " and kind = ?");
        try params.append(allocator, .{ .text = @tagName(k) });
    }
    try sql_buf.appendSlice(allocator, " order by slug");

    const sql_z = try allocator.dupeZ(u8, sql_buf.items);
    defer allocator.free(sql_z);

    var stmt = d.prepare(sql_z) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(params.items) catch return Error.QueryFailed;

    var out: std.ArrayList(Association) = .empty;
    errdefer {
        for (out.items) |a| deinit(a, allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try readRow(&stmt, allocator)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

// =========================================================================
// Membership (project_associations join)
// =========================================================================

/// Add a project (looked up or created from `repo_path`) to the
/// association named by `slug`. Source defaults to `.user` — `detect`
/// uses one of the `auto:*` variants when registering auto-derived
/// memberships.
pub fn addMember(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    assoc_slug: []const u8,
    repo_path: []const u8,
    source: AddMemberSource,
) Error!void {
    const assoc = try showBySlug(d, allocator, assoc_slug);
    defer deinit(assoc, allocator);
    const project = try findOrCreateProjectByPath(d, allocator, repo_path);
    defer deinitProject(project, allocator);

    _ = d.execParams(
        \\insert into project_associations (project_id, association_id, source)
        \\values (?, ?, ?)
    , &.{
        .{ .int = project.id },
        .{ .int = assoc.id },
        .{ .text = source.toText() },
    }) catch |e| {
        if (d.lastWasUniqueViolation()) return Error.AlreadyMember;
        std.log.err("association.addMember exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };

    const summary = try std.fmt.allocPrint(
        allocator,
        "add project '{s}' to association '{s}'",
        .{ project.slug, assoc.slug },
    );
    defer allocator.free(summary);
    try policy.audit.record(d, .{
        .verb = .link,
        .entity = .{ .kind = "association", .id = assoc.id },
        .summary = summary,
    });
}

pub fn removeMember(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    assoc_slug: []const u8,
    repo_path: []const u8,
) Error!void {
    const assoc = try showBySlug(d, allocator, assoc_slug);
    defer deinit(assoc, allocator);
    const project = projectByPath(d, allocator, repo_path) catch |e| switch (e) {
        Error.NotFound => return Error.NotAMember,
        else => return e,
    };
    defer deinitProject(project, allocator);

    _ = d.execParams(
        \\delete from project_associations where project_id = ? and association_id = ?
    , &.{
        .{ .int = project.id },
        .{ .int = assoc.id },
    }) catch return Error.QueryFailed;

    const summary = try std.fmt.allocPrint(
        allocator,
        "remove project '{s}' from association '{s}'",
        .{ project.slug, assoc.slug },
    );
    defer allocator.free(summary);
    try policy.audit.record(d, .{
        .verb = .unlink,
        .entity = .{ .kind = "association", .id = assoc.id },
        .summary = summary,
    });
}

pub fn members(d: *db.sqlite.Db, allocator: std.mem.Allocator, assoc_slug: []const u8) Error![]Project {
    const assoc = try showBySlug(d, allocator, assoc_slug);
    defer deinit(assoc, allocator);

    var stmt = d.prepare(
        \\select p.id, p.slug, p.name, p.root_path
        \\from projects p
        \\join project_associations pa on pa.project_id = p.id
        \\where pa.association_id = ?
        \\order by p.slug
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = assoc.id }}) catch return Error.QueryFailed;

    var out: std.ArrayList(Project) = .empty;
    errdefer {
        for (out.items) |p| deinitProject(p, allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, .{
                .id = stmt.columnInt(0),
                .slug = try stmt.columnTextAlloc(1, allocator),
                .name = try stmt.columnTextAlloc(2, allocator),
                .root_path = try stmt.columnTextOpt(3, allocator),
            }),
        }
    }
    return try out.toOwnedSlice(allocator);
}

// =========================================================================
// Output rendering
// =========================================================================

pub fn renderText(a: Association, writer: *std.Io.Writer) std.Io.Writer.Error!void {
    try writer.print("id:        {d}\n", .{@as(u64, @intCast(a.id))});
    try writer.print("slug:      {s}\n", .{a.slug});
    try writer.print("name:      {s}\n", .{a.name});
    try writer.print("kind:      {s}\n", .{@tagName(a.kind)});
    try writer.print("auto:      {s}\n", .{if (a.auto_detected) "yes" else "no"});
    if (a.config_json) |c| try writer.print("config:    {s}\n", .{c});
    try writer.print("created:   {s}\n", .{a.created_at});
    try writer.print("updated:   {s}\n", .{a.updated_at});
}

pub fn renderListText(items: []const Association, writer: *std.Io.Writer) std.Io.Writer.Error!void {
    if (items.len == 0) {
        try writer.print("(no associations)\n", .{});
        return;
    }
    for (items) |a| {
        try writer.print("{s:<20}  {s:<12}  {s}\n", .{ a.slug, @tagName(a.kind), a.name });
    }
}

pub fn renderProjectListText(items: []const Project, writer: *std.Io.Writer) std.Io.Writer.Error!void {
    if (items.len == 0) {
        try writer.print("(no members)\n", .{});
        return;
    }
    for (items) |p| {
        const path = p.root_path orelse "(no root)";
        try writer.print("{s:<20}  {s}\n", .{ p.slug, path });
    }
}

// =========================================================================
// Internals
// =========================================================================

const select_columns_sql: [:0]const u8 =
    "select id, slug, name, kind, auto_detected, config_json, created_at, updated_at from associations";

fn readRow(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) Error!Association {
    const kind_text = try stmt.columnTextAlloc(3, allocator);
    defer allocator.free(kind_text);
    const kind = Kind.fromText(kind_text) orelse return Error.UnknownKind;
    return .{
        .id = stmt.columnInt(0),
        .slug = try stmt.columnTextAlloc(1, allocator),
        .name = try stmt.columnTextAlloc(2, allocator),
        .kind = kind,
        .auto_detected = stmt.columnInt(4) != 0,
        .config_json = try stmt.columnTextOpt(5, allocator),
        .created_at = try stmt.columnTextAlloc(6, allocator),
        .updated_at = try stmt.columnTextAlloc(7, allocator),
    };
}

fn projectByPath(d: *db.sqlite.Db, allocator: std.mem.Allocator, root_path: []const u8) Error!Project {
    var stmt = d.prepare(
        "select id, slug, name, root_path from projects where root_path = ?",
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = root_path }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => .{
            .id = stmt.columnInt(0),
            .slug = try stmt.columnTextAlloc(1, allocator),
            .name = try stmt.columnTextAlloc(2, allocator),
            .root_path = try stmt.columnTextOpt(3, allocator),
        },
    };
}

fn findOrCreateProjectByPath(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    root_path: []const u8,
) Error!Project {
    if (projectByPath(d, allocator, root_path)) |p| {
        return p;
    } else |e| {
        if (e != Error.NotFound) return e;
    }

    // Create a project with slug + name derived from the path basename.
    const basename = std.fs.path.basename(root_path);
    const slug = try slugifyPathSegment(allocator, basename);
    defer allocator.free(slug);

    const id = d.execParams(
        "insert into projects (slug, name, root_path) values (?, ?, ?)",
        &.{ .{ .text = slug }, .{ .text = basename }, .{ .text = root_path } },
    ) catch |e| {
        // Slug collision on auto-derived name — retry with a numeric
        // suffix until we find one that sticks. Keeps `association add`
        // working when two repos share a basename.
        if (d.lastWasUniqueViolation()) {
            return try findOrCreateProjectByPathWithSuffix(d, allocator, basename, root_path);
        }
        std.log.err("project insert failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };
    return try projectById(d, allocator, id);
}

fn findOrCreateProjectByPathWithSuffix(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    basename: []const u8,
    root_path: []const u8,
) Error!Project {
    var suffix: u32 = 2;
    while (suffix < 10000) : (suffix += 1) {
        const base_slug = try slugifyPathSegment(allocator, basename);
        defer allocator.free(base_slug);
        const candidate = try std.fmt.allocPrint(allocator, "{s}-{d}", .{ base_slug, suffix });
        defer allocator.free(candidate);
        const id = d.execParams(
            "insert into projects (slug, name, root_path) values (?, ?, ?)",
            &.{ .{ .text = candidate }, .{ .text = basename }, .{ .text = root_path } },
        ) catch |e| {
            if (d.lastWasUniqueViolation()) continue;
            std.log.err("project insert (suffix) failed: {s}", .{@errorName(e)});
            return Error.QueryFailed;
        };
        return try projectById(d, allocator, id);
    }
    return Error.QueryFailed; // gave up after 10k attempts
}

fn projectById(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!Project {
    var stmt = d.prepare("select id, slug, name, root_path from projects where id = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => .{
            .id = stmt.columnInt(0),
            .slug = try stmt.columnTextAlloc(1, allocator),
            .name = try stmt.columnTextAlloc(2, allocator),
            .root_path = try stmt.columnTextOpt(3, allocator),
        },
    };
}

fn slugifyPathSegment(allocator: std.mem.Allocator, s: []const u8) std.mem.Allocator.Error![]const u8 {
    var out: std.ArrayList(u8) = .empty;
    errdefer out.deinit(allocator);
    var last_dash = true;
    for (s) |ch| {
        const lower = std.ascii.toLower(ch);
        if (std.ascii.isAlphanumeric(lower)) {
            try out.append(allocator, lower);
            last_dash = false;
        } else if (!last_dash) {
            try out.append(allocator, '-');
            last_dash = true;
        }
    }
    if (out.items.len > 0 and out.items[out.items.len - 1] == '-') _ = out.pop();
    if (out.items.len == 0) try out.append(allocator, '_');
    return try out.toOwnedSlice(allocator);
}

// =========================================================================
// Auto-detection (proposals, enrichment, apply)
// =========================================================================

/// A candidate association generated by `detectProposals` and consumed
/// by `applyProposals`. All string fields are owned by the allocator
/// passed to the producing function. Release via `deinitProposal` /
/// `deinitProposals`.
///
/// `assoc_exists` and `member_exists` are populated by
/// `enrichProposals` to give the operator a preview of what `--apply`
/// will do.
pub const Proposal = struct {
    slug: []const u8,
    kind: Kind,
    /// `project_associations.source` value to record on apply.
    source: AddMemberSource,
    /// Human-readable explanation (e.g. "from git remote host").
    reason: []const u8,
    /// Filled by enrichProposals. Default false.
    assoc_exists: bool = false,
    /// Filled by enrichProposals. Default false.
    member_exists: bool = false,
};

pub fn deinitProposal(p: Proposal, allocator: std.mem.Allocator) void {
    allocator.free(p.slug);
    allocator.free(p.reason);
}

pub fn deinitProposals(items: []const Proposal, allocator: std.mem.Allocator) void {
    for (items) |p| deinitProposal(p, allocator);
    allocator.free(items);
}

/// Inputs to `proposalsFromSignals`. The handler gathers these via IO
/// (`git remote get-url origin`, fs.access for lang markers); the
/// engine then turns them into Proposal rows without touching the
/// filesystem or shelling out.
pub const Signals = struct {
    /// Optional git remote origin URL (raw, as `git remote get-url
    /// origin` would return it).
    git_remote: ?[]const u8 = null,
    /// Optional parent directory basename of cwd.
    parent_basename: ?[]const u8 = null,
    /// Optional language ecosystem tag ("go", "rust", "javascript",
    /// "python") — caller derived from top-level marker files.
    lang: ?[]const u8 = null,
};

/// Pure: turn a `Signals` bundle into a slice of `Proposal`s. No DB,
/// no IO. Caller owns the returned slice; release via `deinitProposals`.
///
/// Mirrors the proposal-building branches of Go's
/// `association.DetectProposals` (host + org from git remote, path:
/// from parent basename, lang: from detected ecosystem).
pub fn proposalsFromSignals(allocator: std.mem.Allocator, sig: Signals) std.mem.Allocator.Error![]Proposal {
    var out: std.ArrayList(Proposal) = .empty;
    errdefer {
        for (out.items) |p| deinitProposal(p, allocator);
        out.deinit(allocator);
    }

    if (sig.git_remote) |remote| {
        const parsed = parseRemote(remote);
        if (parsed.host.len > 0) {
            const host_slug = try sanitizeHostSlug(allocator, parsed.host);
            defer allocator.free(host_slug);
            const slug = try std.fmt.allocPrint(allocator, "host:{s}", .{host_slug});
            errdefer allocator.free(slug);
            const reason = try allocator.dupe(u8, "from git remote host");
            try out.append(allocator, .{
                .slug = slug,
                .kind = .host,
                .source = .@"auto:git-remote",
                .reason = reason,
            });
        }
        if (parsed.owner.len > 0) {
            const owner_slug = try sanitizeSlugPart(allocator, parsed.owner);
            defer allocator.free(owner_slug);
            const slug = try std.fmt.allocPrint(allocator, "org:{s}", .{owner_slug});
            errdefer allocator.free(slug);
            const reason = try allocator.dupe(u8, "from git remote org");
            try out.append(allocator, .{
                .slug = slug,
                .kind = .org,
                .source = .@"auto:git-remote",
                .reason = reason,
            });
        }
    }

    if (sig.parent_basename) |raw| {
        if (raw.len > 0 and !std.mem.eql(u8, raw, ".") and !std.mem.eql(u8, raw, "/")) {
            const part = try sanitizeSlugPart(allocator, raw);
            defer allocator.free(part);
            if (part.len > 0) {
                const slug = try std.fmt.allocPrint(allocator, "path:{s}", .{part});
                errdefer allocator.free(slug);
                const reason = try allocator.dupe(u8, "from parent directory");
                try out.append(allocator, .{
                    .slug = slug,
                    .kind = .path,
                    .source = .@"auto:path",
                    .reason = reason,
                });
            }
        }
    }

    if (sig.lang) |lang| {
        if (lang.len > 0) {
            const slug = try std.fmt.allocPrint(allocator, "lang:{s}", .{lang});
            errdefer allocator.free(slug);
            const reason = try allocator.dupe(u8, "from detected language ecosystem");
            try out.append(allocator, .{
                .slug = slug,
                .kind = .lang,
                .source = .@"auto:lang",
                .reason = reason,
            });
        }
    }

    return try out.toOwnedSlice(allocator);
}

/// Inspect `dir` and produce proposals by gathering signals via IO:
///   - `git -C dir remote get-url origin` → host + org proposals
///   - `basename(dirname(dir))` → path proposal
///   - top-level marker files → lang proposal
///
/// Returns an empty slice when nothing matches. Caller owns the result
/// (release via `deinitProposals`).
///
/// `dir` should be an absolute path; `git` and the marker-file checks
/// will run with that as the working directory.
pub fn detectProposals(
    io: std.Io,
    allocator: std.mem.Allocator,
    dir: []const u8,
) std.mem.Allocator.Error![]Proposal {
    // ---- git remote origin (best-effort) ----
    const remote_owned: ?[]u8 = gitRemoteOrigin(io, allocator, dir);
    defer if (remote_owned) |b| allocator.free(b);
    const remote_trimmed: ?[]const u8 = if (remote_owned) |b|
        std.mem.trim(u8, b, " \t\r\n")
    else
        null;

    // ---- parent directory basename ----
    const parent = std.fs.path.dirname(dir);
    const parent_base: ?[]const u8 = if (parent) |p| std.fs.path.basename(p) else null;

    // ---- language ecosystem marker ----
    const lang = detectLang(io, dir);

    return try proposalsFromSignals(allocator, .{
        .git_remote = remote_trimmed,
        .parent_basename = parent_base,
        .lang = lang,
    });
}

/// Populate `assoc_exists` and `member_exists` on each proposal by
/// consulting the DB. Best-effort: a failed lookup leaves both flags
/// false so the operator still sees the proposal.
pub fn enrichProposals(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    proposals: []Proposal,
    root_path: []const u8,
) Error!void {
    _ = allocator; // not currently needed; reserved for future enrichment

    // Look up the project id by root_path; may be zero if not registered yet.
    var project_id: i64 = 0;
    {
        var stmt = d.prepare("select id from projects where root_path = ?") catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .text = root_path }}) catch return Error.QueryFailed;
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => {},
            .row => project_id = stmt.columnInt(0),
        }
    }

    for (proposals) |*p| {
        var assoc_id: i64 = 0;
        {
            var stmt = d.prepare("select id from associations where slug = ?") catch return Error.QueryFailed;
            defer stmt.finalize();
            stmt.bind(&.{.{ .text = p.slug }}) catch return Error.QueryFailed;
            switch (stmt.step() catch return Error.QueryFailed) {
                .done => continue,
                .row => assoc_id = stmt.columnInt(0),
            }
        }
        p.assoc_exists = true;
        if (project_id != 0) {
            var stmt = d.prepare(
                "select count(*) from project_associations where project_id = ? and association_id = ?",
            ) catch return Error.QueryFailed;
            defer stmt.finalize();
            stmt.bind(&.{ .{ .int = project_id }, .{ .int = assoc_id } }) catch return Error.QueryFailed;
            switch (stmt.step() catch return Error.QueryFailed) {
                .done => {},
                .row => p.member_exists = stmt.columnInt(0) > 0,
            }
        }
    }
}

/// Create associations and project_associations rows for each proposal
/// at `root_path`. Idempotent — uses `INSERT OR IGNORE` for associations
/// and an `ON CONFLICT DO UPDATE` for project_associations so the
/// source value stays current on re-detection.
///
/// `root_path` must already be registered as a project (call `engine.init.registerCwd`
/// first or fail with `NotFound`).
pub fn applyProposals(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    proposals: []const Proposal,
    root_path: []const u8,
) Error!void {
    // Wrap the whole apply in a savepoint for atomicity. Mirrors Go's
    // `sql.DB.Begin` / `tx.Commit` pattern. Without this, a mid-apply
    // failure could leave a partial-membership state (e.g. one assoc
    // created but the project_associations row missing). The savepoint
    // is released on success and rolled back on any error.
    const sp = "applyProposals";
    d.savepoint(allocator, sp) catch return Error.QueryFailed;
    var committed = false;
    errdefer {
        // Rollback then release so the savepoint stack stays balanced.
        // Both calls are best-effort: if rollback itself fails the
        // savepoint will be cleaned up when the connection closes.
        if (!committed) {
            d.rollbackToSavepoint(allocator, sp) catch {};
            d.releaseSavepoint(allocator, sp) catch {};
        }
    }

    // Resolve the project id once; fail early if unregistered.
    var project_id: i64 = 0;
    {
        var stmt = d.prepare("select id from projects where root_path = ?") catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .text = root_path }}) catch return Error.QueryFailed;
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => return Error.NotFound,
            .row => project_id = stmt.columnInt(0),
        }
    }

    for (proposals) |p| {
        // Upsert association (insert or ignore — slug uniqueness already
        // protects us; a pre-existing row is fine).
        _ = d.execParams(
            \\insert or ignore into associations (slug, name, kind, auto_detected)
            \\values (?, ?, ?, 1)
        , &.{
            .{ .text = p.slug },
            .{ .text = p.slug },
            .{ .text = @tagName(p.kind) },
        }) catch return Error.QueryFailed;

        // Look up the (now-extant) assoc id.
        var assoc_id: i64 = 0;
        {
            var stmt = d.prepare("select id from associations where slug = ?") catch return Error.QueryFailed;
            defer stmt.finalize();
            stmt.bind(&.{.{ .text = p.slug }}) catch return Error.QueryFailed;
            switch (stmt.step() catch return Error.QueryFailed) {
                .done => return Error.QueryFailed,
                .row => assoc_id = stmt.columnInt(0),
            }
        }

        // Upsert membership; refresh source on conflict (auto:* stays current).
        _ = d.execParams(
            \\insert into project_associations (project_id, association_id, source)
            \\values (?, ?, ?)
            \\on conflict (project_id, association_id) do update set source = excluded.source
        , &.{
            .{ .int = project_id },
            .{ .int = assoc_id },
            .{ .text = p.source.toText() },
        }) catch return Error.QueryFailed;

        // Record an audit row — mirrors addMember.
        const summary = try std.fmt.allocPrint(
            allocator,
            "auto-link project_id={d} → association '{s}'",
            .{ project_id, p.slug },
        );
        defer allocator.free(summary);
        try policy.audit.record(d, .{
            .verb = .link,
            .entity = .{ .kind = "association", .id = assoc_id },
            .summary = summary,
        });
    }

    // Commit (release the savepoint). After this point the errdefer above
    // is a no-op so a failure in release itself doesn't double-roll-back.
    d.releaseSavepoint(allocator, sp) catch return Error.QueryFailed;
    committed = true;
}

/// Parsed git remote URL. Empty fields when the URL is unrecognized.
pub const RemoteParts = struct {
    host: []const u8,
    owner: []const u8,
};

/// Extract (host, owner) from an SSH or HTTPS git remote URL. Returns
/// empty strings when the URL is unrecognized. Pure / no allocation:
/// returned slices borrow from `remote`.
///
///   ssh://git@github.com/owner/repo.git  → ("github.com", "owner")
///   git@github.com:owner/repo.git        → ("github.com", "owner")
///   https://github.com/owner/repo.git    → ("github.com", "owner")
pub fn parseRemote(remote: []const u8) RemoteParts {
    // SCP-style: git@host:owner/repo.git
    if (std.mem.startsWith(u8, remote, "git@")) {
        const rest = remote["git@".len..];
        const colon = std.mem.indexOfScalar(u8, rest, ':') orelse return .{ .host = "", .owner = "" };
        const host = rest[0..colon];
        const path = rest[colon + 1 ..];
        const slash = std.mem.indexOfScalar(u8, path, '/');
        const owner_raw = if (slash) |s| path[0..s] else path;
        return .{ .host = host, .owner = trimSuffix(owner_raw, ".git") };
    }

    // URL-style: scheme://host[:port]/owner/repo.git
    const scheme_end = std.mem.indexOf(u8, remote, "://") orelse return .{ .host = "", .owner = "" };
    var rest = remote[scheme_end + 3 ..];
    // Strip optional "user@" prefix.
    if (std.mem.indexOfScalar(u8, rest, '@')) |at| {
        rest = rest[at + 1 ..];
    }
    const slash_idx = std.mem.indexOfScalar(u8, rest, '/') orelse return .{ .host = "", .owner = "" };
    var host = rest[0..slash_idx];
    // Strip port.
    if (std.mem.indexOfScalar(u8, host, ':')) |c| host = host[0..c];
    const path = rest[slash_idx + 1 ..];
    if (path.len == 0) return .{ .host = host, .owner = "" };
    const next_slash = std.mem.indexOfScalar(u8, path, '/');
    const owner_raw = if (next_slash) |s| path[0..s] else path;
    return .{ .host = host, .owner = trimSuffix(owner_raw, ".git") };
}

fn trimSuffix(s: []const u8, suffix: []const u8) []const u8 {
    if (std.mem.endsWith(u8, s, suffix)) return s[0 .. s.len - suffix.len];
    return s;
}

/// sanitizeHostSlug lowercases and strips characters outside `[a-z0-9._-]`.
/// Caller owns the returned slice.
fn sanitizeHostSlug(allocator: std.mem.Allocator, s: []const u8) std.mem.Allocator.Error![]const u8 {
    var out: std.ArrayList(u8) = .empty;
    errdefer out.deinit(allocator);
    var prev_dash = true;
    for (s) |raw| {
        const ch = std.ascii.toLower(raw);
        const keep = std.ascii.isAlphanumeric(ch) or ch == '.' or ch == '_' or ch == '-';
        if (keep) {
            try out.append(allocator, ch);
            prev_dash = (ch == '-');
        } else if (!prev_dash) {
            try out.append(allocator, '-');
            prev_dash = true;
        }
    }
    while (out.items.len > 0 and out.items[out.items.len - 1] == '-') _ = out.pop();
    return try out.toOwnedSlice(allocator);
}

/// sanitizeSlugPart lowercases and strips characters outside `[a-z0-9_-]`.
/// Caller owns the returned slice.
fn sanitizeSlugPart(allocator: std.mem.Allocator, s: []const u8) std.mem.Allocator.Error![]const u8 {
    var out: std.ArrayList(u8) = .empty;
    errdefer out.deinit(allocator);
    var prev_dash = true;
    for (s) |raw| {
        const ch = std.ascii.toLower(raw);
        const keep = std.ascii.isAlphanumeric(ch) or ch == '_' or ch == '-';
        if (keep) {
            try out.append(allocator, ch);
            prev_dash = (ch == '-');
        } else if (!prev_dash) {
            try out.append(allocator, '-');
            prev_dash = true;
        }
    }
    while (out.items.len > 0 and out.items[out.items.len - 1] == '-') _ = out.pop();
    return try out.toOwnedSlice(allocator);
}

/// gitRemoteOrigin runs `git -C dir remote get-url origin` and returns
/// the stdout slice (allocator-owned) when the command succeeds with
/// exit 0 and non-empty output. Returns null when git is unavailable,
/// the dir is not a git repo, or no origin is set.
fn gitRemoteOrigin(io: std.Io, allocator: std.mem.Allocator, dir: []const u8) ?[]u8 {
    const argv = [_][]const u8{ "git", "-C", dir, "remote", "get-url", "origin" };
    const result = std.process.run(allocator, io, .{ .argv = &argv }) catch return null;
    defer allocator.free(result.stderr);
    switch (result.term) {
        .exited => |code| {
            if (code != 0) {
                allocator.free(result.stdout);
                return null;
            }
        },
        else => {
            allocator.free(result.stdout);
            return null;
        },
    }
    if (std.mem.trim(u8, result.stdout, " \t\r\n").len == 0) {
        allocator.free(result.stdout);
        return null;
    }
    return result.stdout;
}

/// detectLang inspects the top-level ecosystem marker files in `dir`
/// and returns the language tag. Returns null when no marker matches.
/// Conservative — top-level only.
fn detectLang(io: std.Io, dir: []const u8) ?[]const u8 {
    const markers = [_]struct { file: []const u8, lang: []const u8 }{
        .{ .file = "go.mod", .lang = "go" },
        .{ .file = "Cargo.toml", .lang = "rust" },
        .{ .file = "package.json", .lang = "javascript" },
        .{ .file = "pyproject.toml", .lang = "python" },
    };
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    for (markers) |m| {
        const full = std.fmt.bufPrint(&buf, "{s}/{s}", .{ dir, m.file }) catch continue;
        std.Io.Dir.accessAbsolute(io, full, .{}) catch continue;
        return m.lang;
    }
    return null;
}

// =========================================================================
// Tests
// =========================================================================

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

test "create + showBySlug round-trip" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const created = try create(&d, a, .{ .slug = "acme", .name = "Acme Corp", .kind = .org });
    defer deinit(created, a);
    try std.testing.expectEqualStrings("acme", created.slug);
    try std.testing.expectEqual(Kind.org, created.kind);

    const fetched = try showBySlug(&d, a, "acme");
    defer deinit(fetched, a);
    try std.testing.expectEqual(created.id, fetched.id);
}

test "create rejects duplicate slug" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const first = try create(&d, a, .{ .slug = "dupe" });
    defer deinit(first, a);
    try std.testing.expectError(Error.SlugConflict, create(&d, a, .{ .slug = "dupe" }));
}

test "list with kind filter" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const a1 = try create(&d, a, .{ .slug = "o-1", .kind = .org });
    defer deinit(a1, a);
    const a2 = try create(&d, a, .{ .slug = "c-1", .kind = .client });
    defer deinit(a2, a);

    const orgs = try list(&d, a, .{ .kind = .org });
    defer deinitMany(orgs, a);
    try std.testing.expectEqual(@as(usize, 1), orgs.len);
    try std.testing.expectEqualStrings("o-1", orgs[0].slug);
}

test "addMember creates project on first touch, then refuses duplicate" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const assoc = try create(&d, a, .{ .slug = "myorg", .kind = .org });
    defer deinit(assoc, a);

    try addMember(&d, a, "myorg", "/repos/foo", .user);
    try std.testing.expectError(Error.AlreadyMember, addMember(&d, a, "myorg", "/repos/foo", .user));

    const m = try members(&d, a, "myorg");
    defer deinitProjects(m, a);
    try std.testing.expectEqual(@as(usize, 1), m.len);
    try std.testing.expectEqualStrings("foo", m[0].slug);
    try std.testing.expectEqualStrings("/repos/foo", m[0].root_path.?);
}

test "addMember resolves basename collisions with a numeric suffix" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const assoc = try create(&d, a, .{ .slug = "x" });
    defer deinit(assoc, a);

    try addMember(&d, a, "x", "/work/foo", .user);
    try addMember(&d, a, "x", "/personal/foo", .user);

    const m = try members(&d, a, "x");
    defer deinitProjects(m, a);
    try std.testing.expectEqual(@as(usize, 2), m.len);
    // Slugs differ even though basenames collide.
    try std.testing.expect(!std.mem.eql(u8, m[0].slug, m[1].slug));
}

test "removeMember drops the join row" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const assoc = try create(&d, a, .{ .slug = "rm" });
    defer deinit(assoc, a);

    try addMember(&d, a, "rm", "/repos/bar", .user);
    try removeMember(&d, a, "rm", "/repos/bar");

    const m = try members(&d, a, "rm");
    defer deinitProjects(m, a);
    try std.testing.expectEqual(@as(usize, 0), m.len);
}

test "showBySlug returns NotFound for an unknown slug" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try std.testing.expectError(Error.NotFound, showBySlug(&d, a, "ghost"));
}

// ---- detect helpers ----

test "parseRemote: SCP-style git@github.com:owner/repo.git" {
    const p = parseRemote("git@github.com:owner/repo.git");
    try std.testing.expectEqualStrings("github.com", p.host);
    try std.testing.expectEqualStrings("owner", p.owner);
}

test "parseRemote: HTTPS-style https://github.com/owner/repo.git" {
    const p = parseRemote("https://github.com/owner/repo.git");
    try std.testing.expectEqualStrings("github.com", p.host);
    try std.testing.expectEqualStrings("owner", p.owner);
}

test "parseRemote: ssh://git@host:22/owner/repo" {
    const p = parseRemote("ssh://git@github.com:22/owner/repo.git");
    try std.testing.expectEqualStrings("github.com", p.host);
    try std.testing.expectEqualStrings("owner", p.owner);
}

test "parseRemote: empty when unrecognized" {
    const p = parseRemote("not a url");
    try std.testing.expectEqualStrings("", p.host);
    try std.testing.expectEqualStrings("", p.owner);
}

test "proposalsFromSignals: git remote yields host + org" {
    const a = std.testing.allocator;
    const props = try proposalsFromSignals(a, .{
        .git_remote = "git@github.com:planar/planar.git",
    });
    defer deinitProposals(props, a);
    try std.testing.expectEqual(@as(usize, 2), props.len);
    try std.testing.expectEqualStrings("host:github.com", props[0].slug);
    try std.testing.expectEqual(Kind.host, props[0].kind);
    try std.testing.expectEqualStrings("org:planar", props[1].slug);
    try std.testing.expectEqual(Kind.org, props[1].kind);
}

test "proposalsFromSignals: parent basename yields path:" {
    const a = std.testing.allocator;
    const props = try proposalsFromSignals(a, .{ .parent_basename = "Workspaces" });
    defer deinitProposals(props, a);
    try std.testing.expectEqual(@as(usize, 1), props.len);
    try std.testing.expectEqualStrings("path:workspaces", props[0].slug);
    try std.testing.expectEqual(Kind.path, props[0].kind);
}

test "proposalsFromSignals: lang yields lang:" {
    const a = std.testing.allocator;
    const props = try proposalsFromSignals(a, .{ .lang = "go" });
    defer deinitProposals(props, a);
    try std.testing.expectEqual(@as(usize, 1), props.len);
    try std.testing.expectEqualStrings("lang:go", props[0].slug);
}

test "proposalsFromSignals: empty signals yield empty slice" {
    const a = std.testing.allocator;
    const props = try proposalsFromSignals(a, .{});
    defer deinitProposals(props, a);
    try std.testing.expectEqual(@as(usize, 0), props.len);
}

test "proposalsFromSignals: parent '.' or '/' skipped" {
    const a = std.testing.allocator;
    const props = try proposalsFromSignals(a, .{ .parent_basename = "/" });
    defer deinitProposals(props, a);
    try std.testing.expectEqual(@as(usize, 0), props.len);
}

test "applyProposals: requires registered project; NotFound otherwise" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const props = try proposalsFromSignals(a, .{ .lang = "go" });
    defer deinitProposals(props, a);
    try std.testing.expectError(
        Error.NotFound,
        applyProposals(&d, a, props, "/no/such/path"),
    );
}

test "applyProposals: creates association + project_associations row, idempotent" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('p', 'P', '/work/p')",
        &.{},
    );

    const props = try proposalsFromSignals(a, .{ .lang = "go" });
    defer deinitProposals(props, a);

    try applyProposals(&d, a, props, "/work/p");
    try std.testing.expectEqual(@as(i64, 1), try d.intQuery("select count(*) from associations where slug = 'lang:go'"));
    try std.testing.expectEqual(@as(i64, 1), try d.intQuery("select count(*) from project_associations"));

    // Second call is a no-op (idempotent).
    try applyProposals(&d, a, props, "/work/p");
    try std.testing.expectEqual(@as(i64, 1), try d.intQuery("select count(*) from associations where slug = 'lang:go'"));
    try std.testing.expectEqual(@as(i64, 1), try d.intQuery("select count(*) from project_associations"));
}

test "enrichProposals: flags assoc_exists / member_exists correctly" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('p', 'P', '/work/p')",
        &.{},
    );
    const pid = try d.intQuery("select id from projects where slug = 'p'");
    _ = try d.execParams(
        "insert into associations (slug, name, kind) values ('lang:go', 'lang:go', 'lang')",
        &.{},
    );
    const aid = try d.intQuery("select id from associations where slug = 'lang:go'");
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'auto:lang')",
        &.{ .{ .int = pid }, .{ .int = aid } },
    );

    const props = try proposalsFromSignals(a, .{ .lang = "go", .parent_basename = "absent" });
    defer deinitProposals(props, a);
    try enrichProposals(&d, a, props, "/work/p");

    // proposalsFromSignals order: git_remote → parent_basename → lang.
    // With no git_remote, props[0] is path:absent (unregistered) and
    // props[1] is lang:go (already in the DB + linked).
    try std.testing.expectEqualStrings("path:absent", props[0].slug);
    try std.testing.expectEqual(false, props[0].assoc_exists);
    try std.testing.expectEqual(false, props[0].member_exists);

    try std.testing.expectEqualStrings("lang:go", props[1].slug);
    try std.testing.expectEqual(true, props[1].assoc_exists);
    try std.testing.expectEqual(true, props[1].member_exists);
}

test "sanitizeHostSlug + sanitizeSlugPart: lowercase + collapse + trim" {
    const a = std.testing.allocator;
    const host = try sanitizeHostSlug(a, "GitHub.com");
    defer a.free(host);
    try std.testing.expectEqualStrings("github.com", host);

    const owner = try sanitizeSlugPart(a, "My.Org");
    defer a.free(owner);
    // Dots collapse to '-' in slug parts (Go behavior).
    try std.testing.expectEqualStrings("my-org", owner);
}
