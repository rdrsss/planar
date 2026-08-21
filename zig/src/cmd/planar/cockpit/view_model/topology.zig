//! Association and project-topology cockpit queries.

const std = @import("std");
const db = @import("db");
const testing = std.testing;

// =========================================================================
// Scope / Association Topology view-model  (tasks 4039, 4040)
// =========================================================================
//
// Schema from migrations/00001_foundation.up.sql:
//   projects(id, slug, name, root_path, git_remote, created_at, updated_at)
//   associations(id, slug, name, kind, auto_detected, config_json,
//                created_at, updated_at)
//   project_associations(project_id, association_id, source, created_at)
//
// MEMORY GUARD (brief rule (c)):
//   All string fields in TopologyAssocRow and TopologyMemberRow are
//   freshly heap-allocated via dupe/allocPrint. No field aliases into
//   a query-result list or another struct's string. The caller owns the
//   result and releases it via TopologyAssocRow.deinitMany.
//
// Engine fidelity (task 4040 / CRITICAL):
//   The scope-resolution context rendered for each association mirrors the
//   algorithm in src/engine/identity/scope.zig `deriveFromCwd` (the
//   cwd-derived scope resolution).
//
//   That algorithm is:
//     1. Find the project whose root_path is the longest prefix of cwd.
//     2. Look up that project's association memberships.
//     3a. Exactly 1 membership → scope = that association's slug
//         (Reason.project_single_association).
//     3b. 0 memberships → scope = null (Reason.project_unassociated).
//     3c. 2+ memberships → scope = null (Reason.project_multiple_associations).
//     3d. No matching project → scope = null (Reason.no_project_match).
//
//   For each member project in an association, we surface:
//     • member_count: total number of associations the project belongs to.
//       This is the key for task 4040: when member_count = 1, a cwd inside
//       this project unambiguously resolves to THIS association (the engine's
//       project_single_association case). When member_count > 1, the cwd is
//       ambiguous (project_multiple_associations). When member_count = 0 (not
//       possible given we reached it via project_associations), we would see
//       project_unassociated.
//     • resolution_rule: a human-readable string encoding the outcome the
//       engine would produce for any cwd inside this project:
//         "→ resolves here"   (member_count = 1: unambiguous single membership)
//         "→ ambiguous (N)"   (member_count > 1: multiple associations compete)
//       This is the exact check src/engine/identity/scope.zig does at step 3
//       (branch on assoc_slugs.items.len). Cite: scope.zig lines 242–280.
//     • source: the project_associations.source column
//       ("user" | "auto:git-remote" | "auto:path" | "auto:lang").
//     • root_path / git_remote: the project's registration fields, which
//       are what the engine's `lookupProjectByPrefix` matches against.
//       Surfacing these lets the operator see exactly which path or remote
//       drove auto-detection.

/// One member project in an association, for the Topology view.
///
/// Mirrored from the engine's scope-resolution model:
/// src/engine/identity/scope.zig `deriveFromCwd`.
pub const TopologyMemberRow = struct {
    /// projects.id
    project_id: i64,
    /// projects.slug
    project_slug: []const u8,
    /// projects.name
    project_name: []const u8,
    /// projects.root_path (null when not set).
    root_path: ?[]const u8,
    /// projects.git_remote (null when not set).
    git_remote: ?[]const u8,
    /// project_associations.source for this membership.
    source: []const u8,
    /// Human-readable source label (from scope.reasonFromSource).
    source_label: []const u8,
    /// Total number of associations this project belongs to.
    /// Used to derive resolution_rule (engine fidelity, task 4040).
    member_count: i64,
    /// Scope-resolution outcome for any cwd inside this project.
    /// Mirrors src/engine/identity/scope.zig deriveFromCwd step 3:
    ///   member_count = 1 → "→ resolves here"
    ///   member_count > 1 → "→ ambiguous (N)"
    /// Heap-allocated. MEMORY GUARD (rule (c)).
    resolution_rule: []const u8,

    pub fn deinit(self: TopologyMemberRow, allocator: std.mem.Allocator) void {
        allocator.free(self.project_slug);
        allocator.free(self.project_name);
        if (self.root_path) |s| allocator.free(s);
        if (self.git_remote) |s| allocator.free(s);
        allocator.free(self.source);
        allocator.free(self.source_label);
        allocator.free(self.resolution_rule);
    }

    pub fn deinitMany(rows: []TopologyMemberRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// One association with its member projects, for the Topology view navigator.
pub const TopologyAssocRow = struct {
    /// associations.id
    id: i64,
    /// associations.slug
    slug: []const u8,
    /// associations.name
    name: []const u8,
    /// associations.kind ("org" | "project" | "client" | "personal" |
    ///                    "ad-hoc" | "host" | "path" | "lang")
    kind: []const u8,
    /// true when associations.auto_detected = 1.
    auto_detected: bool,
    /// Member projects for this association, ordered by project slug.
    members: []TopologyMemberRow,
    /// Pre-formatted navigator display text:
    ///   "[<kind>] <slug>  (<N> members)"
    /// Heap-allocated. MEMORY GUARD (rule (c)).
    display_text: []const u8,

    pub fn deinit(self: TopologyAssocRow, allocator: std.mem.Allocator) void {
        allocator.free(self.slug);
        allocator.free(self.name);
        allocator.free(self.kind);
        TopologyMemberRow.deinitMany(self.members, allocator);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []TopologyAssocRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Convert project_associations.source to a human-readable label.
/// Mirrors src/engine/identity/scope.zig `reasonFromSource`.
fn topologySourceLabel(source: []const u8) []const u8 {
    if (std.mem.eql(u8, source, "user")) return "explicit member";
    if (std.mem.eql(u8, source, "auto:git-remote")) return "from git remote";
    if (std.mem.eql(u8, source, "auto:path")) return "from parent directory";
    if (std.mem.eql(u8, source, "auto:lang")) return "from language ecosystem";
    return source;
}

/// Query all associations with their member projects for the Topology view.
///
/// Returns associations ordered by slug. For each association, member
/// projects are ordered by project slug. Each member carries the
/// scope-resolution context (resolution_rule) derived from the engine's
/// `deriveFromCwd` algorithm (scope.zig lines 242–280, branch on
/// assoc_slugs.items.len):
///   member_count = 1 → "→ resolves here"
///   member_count > 1 → "→ ambiguous (N)"
///
/// Columns queried:
///   associations.id, slug, name, kind, auto_detected
///   projects.id, slug, name, root_path, git_remote
///   project_associations.source
///   count(*) over(partition by project_id) as member_count
///
/// Caller owns the result; release via `TopologyAssocRow.deinitMany`.
pub fn queryTopology(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) ![]TopologyAssocRow {
    // Step 1: load all associations ordered by slug.
    var assoc_list: std.ArrayList(TopologyAssocRow) = .empty;
    errdefer {
        for (assoc_list.items) |r| r.deinit(allocator);
        assoc_list.deinit(allocator);
    }

    {
        var stmt = d.prepare(
            \\select a.id, a.slug, a.name, a.kind, a.auto_detected
            \\from associations a
            \\order by a.slug
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{}) catch return error.QueryFailed;

        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const aid = stmt.columnInt(0);
                    const slug = try stmt.columnTextAlloc(1, allocator);
                    errdefer allocator.free(slug);
                    const name = try stmt.columnTextAlloc(2, allocator);
                    errdefer allocator.free(name);
                    const kind = try stmt.columnTextAlloc(3, allocator);
                    errdefer allocator.free(kind);
                    const auto_det = stmt.columnInt(4) != 0;

                    // Members loaded in step 2 below.
                    const display_text = try std.fmt.allocPrint(
                        allocator,
                        "[{s}] {s}",
                        .{ kind, slug },
                    );
                    errdefer allocator.free(display_text);

                    try assoc_list.append(allocator, .{
                        .id = aid,
                        .slug = slug,
                        .name = name,
                        .kind = kind,
                        .auto_detected = auto_det,
                        // Filled in step 2.
                        .members = &.{},
                        .display_text = display_text,
                    });
                },
            }
        }
    }

    // Step 2: for each association, load its member projects with
    // the member_count sub-query that powers the resolution_rule.
    //
    // The sub-query `(select count(*) from project_associations where
    // project_id = p.id)` counts the total number of associations a
    // project belongs to. This is the exact quantity the engine's
    // `deriveFromCwd` branches on (assoc_slugs.items.len in scope.zig
    // lines 242–280):
    //   = 1 → project_single_association → "→ resolves here"
    //   > 1 → project_multiple_associations → "→ ambiguous (N)"
    //   = 0 → project_unassociated (impossible via project_associations join)
    for (assoc_list.items) |*assoc_row| {
        var member_list: std.ArrayList(TopologyMemberRow) = .empty;
        errdefer {
            for (member_list.items) |mr| mr.deinit(allocator);
            member_list.deinit(allocator);
        }

        var mstmt = d.prepare(
            \\select p.id, p.slug, p.name, p.root_path, p.git_remote,
            \\       pa.source,
            \\       (select count(*) from project_associations where project_id = p.id) as member_count
            \\from project_associations pa
            \\join projects p on p.id = pa.project_id
            \\where pa.association_id = ?
            \\order by p.slug
        ) catch return error.QueryFailed;
        defer mstmt.finalize();
        mstmt.bind(&.{.{ .int = assoc_row.id }}) catch return error.QueryFailed;

        while (true) {
            switch (mstmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const pid = mstmt.columnInt(0);
                    const pslug = try mstmt.columnTextAlloc(1, allocator);
                    errdefer allocator.free(pslug);
                    const pname = try mstmt.columnTextAlloc(2, allocator);
                    errdefer allocator.free(pname);
                    const root_path = try mstmt.columnTextOpt(3, allocator);
                    errdefer if (root_path) |s| allocator.free(s);
                    const git_remote = try mstmt.columnTextOpt(4, allocator);
                    errdefer if (git_remote) |s| allocator.free(s);
                    const source_raw = try mstmt.columnTextAlloc(5, allocator);
                    errdefer allocator.free(source_raw);
                    const member_count = mstmt.columnInt(6);

                    // source_label: mirrors scope.reasonFromSource (scope.zig).
                    const source_label_str = topologySourceLabel(source_raw);
                    const source_label = try allocator.dupe(u8, source_label_str);
                    errdefer allocator.free(source_label);

                    // resolution_rule: mirrors scope.zig deriveFromCwd lines 242–280.
                    // member_count = 1 → project_single_association → unambiguous.
                    // member_count > 1 → project_multiple_associations → ambiguous.
                    const resolution_rule = if (member_count == 1)
                        try allocator.dupe(u8, "\u{2192} resolves here")
                    else
                        try std.fmt.allocPrint(
                            allocator,
                            "\u{2192} ambiguous ({d})",
                            .{member_count},
                        );
                    errdefer allocator.free(resolution_rule);

                    try member_list.append(allocator, .{
                        .project_id = pid,
                        .project_slug = pslug,
                        .project_name = pname,
                        .root_path = root_path,
                        .git_remote = git_remote,
                        .source = source_raw,
                        .source_label = source_label,
                        .member_count = member_count,
                        .resolution_rule = resolution_rule,
                    });
                },
            }
        }

        const members = try member_list.toOwnedSlice(allocator);
        assoc_row.members = members;

        // Refresh display_text now that we know the member count.
        // Re-allocate so the old one (which had no member count) is replaced.
        const old_dt = assoc_row.display_text;
        const new_dt = try std.fmt.allocPrint(
            allocator,
            "[{s}] {s}  ({d} member{s})",
            .{
                assoc_row.kind,
                assoc_row.slug,
                members.len,
                if (members.len == 1) @as([]const u8, "") else "s",
            },
        );
        allocator.free(old_dt);
        assoc_row.display_text = new_dt;
    }

    return try assoc_list.toOwnedSlice(allocator);
}

// =========================================================================
// Topology view-model tests
// =========================================================================

fn setupTestDbTopology(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

test "view_model: queryTopology empty DB returns empty slice (task 4039 empty)" {
    const a = testing.allocator;
    var d = try setupTestDbTopology(a);
    defer d.close();

    const rows = try queryTopology(&d, a);
    defer TopologyAssocRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 0), rows.len);
}

test "view_model: queryTopology returns association with member projects (task 4039)" {
    const a = testing.allocator;
    var d = try setupTestDbTopology(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name, root_path, git_remote) values ('myrepo', 'My Repo', '/work/myrepo', 'git@gh:org/myrepo')",
        &.{},
    );
    const pid = try d.intQuery("select id from projects where slug = 'myrepo'");
    _ = try d.execParams(
        "insert into associations (slug, name, kind) values ('myorg', 'My Org', 'org')",
        &.{},
    );
    const aid = try d.intQuery("select id from associations where slug = 'myorg'");
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = pid }, .{ .int = aid } },
    );

    const rows = try queryTopology(&d, a);
    defer TopologyAssocRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    try testing.expectEqualStrings("myorg", rows[0].slug);
    try testing.expectEqualStrings("My Org", rows[0].name);
    try testing.expectEqualStrings("org", rows[0].kind);
    try testing.expectEqual(@as(usize, 1), rows[0].members.len);

    const m = rows[0].members[0];
    try testing.expectEqualStrings("myrepo", m.project_slug);
    try testing.expectEqualStrings("My Repo", m.project_name);
    try testing.expect(m.root_path != null);
    try testing.expectEqualStrings("/work/myrepo", m.root_path.?);
    try testing.expect(m.git_remote != null);
    try testing.expectEqualStrings("git@gh:org/myrepo", m.git_remote.?);
    try testing.expectEqualStrings("user", m.source);
    try testing.expectEqualStrings("explicit member", m.source_label);
    try testing.expectEqual(@as(i64, 1), m.member_count);
    // Engine fidelity (task 4040): 1 association → "→ resolves here".
    try testing.expect(std.mem.indexOf(u8, m.resolution_rule, "resolves here") != null);
}

test "view_model: queryTopology resolution_rule ambiguous when project has multiple associations (task 4040)" {
    // This test verifies that the resolution_rule correctly mirrors
    // src/engine/identity/scope.zig `deriveFromCwd` lines 242–280:
    // when a project belongs to 2+ associations, the engine returns
    // Reason.project_multiple_associations; we surface "→ ambiguous (N)".
    const a = testing.allocator;
    var d = try setupTestDbTopology(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('shared', 'Shared Repo', '/work/shared')",
        &.{},
    );
    const pid = try d.intQuery("select id from projects where slug = 'shared'");
    _ = try d.execParams(
        "insert into associations (slug, name, kind) values ('org-a', 'Org A', 'org')",
        &.{},
    );
    const aid1 = try d.intQuery("select id from associations where slug = 'org-a'");
    _ = try d.execParams(
        "insert into associations (slug, name, kind) values ('org-b', 'Org B', 'org')",
        &.{},
    );
    const aid2 = try d.intQuery("select id from associations where slug = 'org-b'");
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = pid }, .{ .int = aid1 } },
    );
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'auto:git-remote')",
        &.{ .{ .int = pid }, .{ .int = aid2 } },
    );

    const rows = try queryTopology(&d, a);
    defer TopologyAssocRow.deinitMany(rows, a);

    // Two associations: org-a and org-b (sorted by slug).
    try testing.expectEqual(@as(usize, 2), rows.len);
    try testing.expectEqualStrings("org-a", rows[0].slug);
    try testing.expectEqualStrings("org-b", rows[1].slug);

    // Both reference the same project; member_count for that project = 2.
    const m0 = rows[0].members[0];
    const m1 = rows[1].members[0];
    try testing.expectEqual(@as(i64, 2), m0.member_count);
    try testing.expectEqual(@as(i64, 2), m1.member_count);

    // Engine fidelity: member_count > 1 → "→ ambiguous (2)".
    try testing.expect(std.mem.indexOf(u8, m0.resolution_rule, "ambiguous") != null);
    try testing.expect(std.mem.indexOf(u8, m0.resolution_rule, "2") != null);
    try testing.expect(std.mem.indexOf(u8, m1.resolution_rule, "ambiguous") != null);

    // source_label mirrors topologySourceLabel (= scope.reasonFromSource):
    // org-b was added via auto:git-remote.
    const m1_ref = rows[1].members[0];
    try testing.expectEqualStrings("from git remote", m1_ref.source_label);
}

test "view_model: queryTopology association with no members has empty members slice (task 4039)" {
    const a = testing.allocator;
    var d = try setupTestDbTopology(a);
    defer d.close();

    _ = try d.execParams(
        "insert into associations (slug, name, kind) values ('empty-org', 'Empty', 'org')",
        &.{},
    );

    const rows = try queryTopology(&d, a);
    defer TopologyAssocRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    try testing.expectEqualStrings("empty-org", rows[0].slug);
    try testing.expectEqual(@as(usize, 0), rows[0].members.len);
    // Display text for 0 members.
    try testing.expect(std.mem.indexOf(u8, rows[0].display_text, "0 members") != null);
}

test "view_model: queryTopology display_text format (task 4039)" {
    const a = testing.allocator;
    var d = try setupTestDbTopology(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name) values ('p1', 'Project 1')",
        &.{},
    );
    const pid = try d.intQuery("select id from projects where slug = 'p1'");
    _ = try d.execParams(
        "insert into associations (slug, name, kind) values ('my-assoc', 'My Assoc', 'project')",
        &.{},
    );
    const aid = try d.intQuery("select id from associations where slug = 'my-assoc'");
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = pid }, .{ .int = aid } },
    );

    const rows = try queryTopology(&d, a);
    defer TopologyAssocRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    // Display text must contain kind, slug, and member count.
    try testing.expect(std.mem.indexOf(u8, rows[0].display_text, "project") != null);
    try testing.expect(std.mem.indexOf(u8, rows[0].display_text, "my-assoc") != null);
    try testing.expect(std.mem.indexOf(u8, rows[0].display_text, "1 member") != null);
}

test "view_model: TopologyAssocRow deinit handles null root_path and git_remote" {
    const a = testing.allocator;
    var d = try setupTestDbTopology(a);
    defer d.close();

    // Project with no root_path or git_remote.
    _ = try d.execParams(
        "insert into projects (slug, name) values ('bare', 'Bare')",
        &.{},
    );
    const pid = try d.intQuery("select id from projects where slug = 'bare'");
    _ = try d.execParams(
        "insert into associations (slug, name, kind) values ('bare-org', 'Bare Org', 'org')",
        &.{},
    );
    const aid = try d.intQuery("select id from associations where slug = 'bare-org'");
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = pid }, .{ .int = aid } },
    );

    const rows = try queryTopology(&d, a);
    defer TopologyAssocRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    const m = rows[0].members[0];
    try testing.expect(m.root_path == null);
    try testing.expect(m.git_remote == null);
}
