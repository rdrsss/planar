//! engine/workbench/feature — Feature-directory layout helpers.
//!
//! Mirrors Go's internal/workbench root.go (FeatureDir) and the storedPath /
//! ArtifactFilename helpers in sync.go and render.go.
//!
//! Directory layout (from Go's workbench/root.go):
//!
//!   <workbench-root>/
//!     <safe-assoc-slug>/             (':' → '_')
//!       <plan-key>-<plan-slug>/      feature directory
//!         README.md                  anchor plan
//!         plans/<slug>.md            child plans
//!         tasks/<repo-slug-or-cross>/<id>-<slug>.md
//!         artifacts/<id>-<slug>.md
//!         scenarios/<id>-<slug>.md
//!         decisions/<id>-<slug>.md
//!         questions/<id>-<slug>.md
//!
//! All returned strings are owned by the allocator passed to the function.

const std = @import("std");

// =========================================================================
// Public API
// =========================================================================

/// featureDir returns the absolute path of the feature directory for an anchor plan:
///   <root>/<safe-assoc-slug>/<plan-key>-<plan-slug>/
///
/// ':' in assoc_slug is replaced with '_' for filesystem safety.
/// Mirrors Go's workbench.FeatureDir.
pub fn featureDir(
    allocator: std.mem.Allocator,
    root: []const u8,
    assoc_slug: []const u8,
    plan_key: []const u8,
    plan_slug: []const u8,
) ![]u8 {
    const safe_assoc = try safeAssocSlug(allocator, assoc_slug);
    defer allocator.free(safe_assoc);
    const safe_key = try safePathSegment(allocator, plan_key);
    defer allocator.free(safe_key);
    const safe_slug = try safePathSegment(allocator, plan_slug);
    defer allocator.free(safe_slug);

    const feature_name = try std.fmt.allocPrint(allocator, "{s}-{s}", .{ safe_key, safe_slug });
    defer allocator.free(feature_name);

    return std.fs.path.join(allocator, &.{ root, safe_assoc, feature_name });
}

/// storedPath returns the manifest-stored root-relative path for a file:
///   <safe-assoc-slug>/<plan-key>-<plan-slug>/<rel-path>
///
/// rel_path uses forward slashes (as produced by renderEntity).
/// Mirrors Go's sync.go storedPath helper.
pub fn storedPath(
    allocator: std.mem.Allocator,
    assoc_slug: []const u8,
    plan_key: []const u8,
    plan_slug: []const u8,
    rel_path: []const u8,
) ![]u8 {
    const safe_assoc = try safeAssocSlug(allocator, assoc_slug);
    defer allocator.free(safe_assoc);
    const safe_key = try safePathSegment(allocator, plan_key);
    defer allocator.free(safe_key);
    const safe_slug = try safePathSegment(allocator, plan_slug);
    defer allocator.free(safe_slug);

    const feature_name = try std.fmt.allocPrint(allocator, "{s}-{s}", .{ safe_key, safe_slug });
    defer allocator.free(feature_name);

    return std.fs.path.join(allocator, &.{ safe_assoc, feature_name, rel_path });
}

fn safePathSegment(allocator: std.mem.Allocator, raw: []const u8) ![]u8 {
    if (raw.len == 0 or std.mem.eql(u8, raw, ".") or std.mem.eql(u8, raw, "..")) {
        return allocator.dupe(u8, "_");
    }
    const out = try allocator.dupe(u8, raw);
    for (out) |*ch| {
        if (ch.* == '/' or ch.* == '\\' or ch.* == 0) ch.* = '_';
    }
    return out;
}

/// artifactFilename derives the workbench filename for an artifact:
///   <id>-<slug>.md
///
/// Mirrors Go's workbench.ArtifactFilename. The slug is derived from the
/// title via slugify(). When the title produces an empty or "untitled" slug
/// the kind (with '_' → '-') is used as fallback.
pub fn artifactFilename(
    allocator: std.mem.Allocator,
    id: i64,
    title: []const u8,
    kind: []const u8,
) ![]u8 {
    var slug_str = try slugify(allocator, title);
    defer allocator.free(slug_str);

    if (slug_str.len == 0 or std.mem.eql(u8, slug_str, "untitled")) {
        allocator.free(slug_str);
        // Replace '_' with '-' in kind for the fallback.
        slug_str = try replaceChar(allocator, kind, '_', '-');
    }

    return std.fmt.allocPrint(allocator, "{d}-{s}.md", .{ id, slug_str });
}

/// canonicalEntityKind maps storage/API aliases to canonical entity kind names.
/// Mirrors Go's workbench.CanonicalEntityKind.
pub fn canonicalEntityKind(kind: []const u8) []const u8 {
    if (std.mem.eql(u8, kind, "test_scenario")) return "scenario";
    return kind;
}

// =========================================================================
// Internal helpers
// =========================================================================

/// safeAssocSlug encodes separators so an association cannot escape the root.
fn safeAssocSlug(allocator: std.mem.Allocator, assoc_slug: []const u8) ![]u8 {
    const out = try allocator.dupe(u8, assoc_slug);
    for (out) |*c| {
        if (c.* == ':' or c.* == '/' or c.* == '\\' or c.* == 0) c.* = '_';
    }
    if (std.mem.eql(u8, out, ".") or std.mem.eql(u8, out, "..")) @memset(out, '_');
    return out;
}

/// replaceChar returns a copy of s with every occurrence of from replaced by to.
fn replaceChar(allocator: std.mem.Allocator, s: []const u8, from: u8, to: u8) ![]u8 {
    const buf = try allocator.dupe(u8, s);
    for (buf) |*c| {
        if (c.* == from) c.* = to;
    }
    return buf;
}

/// slugify converts a title to a URL/filesystem slug:
///   1. Lowercase.
///   2. Replace runs of non-alphanumeric characters with a single '-'.
///   3. Trim leading/trailing '-'.
///   4. Truncate to 60 characters, then re-trim trailing '-'.
///   5. Returns "untitled" when the result is empty.
///
/// Mirrors Go's health/render.Slug (used by workbench render.go).
pub fn slugify(allocator: std.mem.Allocator, title: []const u8) ![]u8 {
    if (title.len == 0) return allocator.dupe(u8, "untitled");

    var buf: std.ArrayListUnmanaged(u8) = .empty;
    errdefer buf.deinit(allocator);

    var in_sep = true; // suppress leading '-'
    for (title) |c| {
        if (std.ascii.isAlphanumeric(c)) {
            try buf.append(allocator, std.ascii.toLower(c));
            in_sep = false;
        } else {
            if (!in_sep) {
                try buf.append(allocator, '-');
                in_sep = true;
            }
        }
    }

    // Trim trailing '-'.
    while (buf.items.len > 0 and buf.items[buf.items.len - 1] == '-') {
        _ = buf.pop();
    }

    // Truncate to 60 characters, then re-trim trailing '-' in case the
    // truncation point landed on a separator. Mirrors Go's Slug().
    if (buf.items.len > 60) {
        buf.items.len = 60;
        while (buf.items.len > 0 and buf.items[buf.items.len - 1] == '-') {
            _ = buf.pop();
        }
    }

    if (buf.items.len == 0) {
        buf.deinit(allocator);
        return allocator.dupe(u8, "untitled");
    }

    return buf.toOwnedSlice(allocator);
}

// =========================================================================
// Tests
// =========================================================================

const testing = std.testing;

test "featureDir: simple assoc slug" {
    const result = try featureDir(testing.allocator, "/wb", "project-checkout", "p42", "my-plan");
    defer testing.allocator.free(result);
    try testing.expectEqualStrings("/wb/project-checkout/p42-my-plan", result);
}

test "featureDir: assoc slug with colon gets sanitized" {
    const result = try featureDir(testing.allocator, "/wb", "project:checkout", "p42", "my-plan");
    defer testing.allocator.free(result);
    try testing.expectEqualStrings("/wb/project_checkout/p42-my-plan", result);
}

test "featureDir: empty assoc slug (global)" {
    const result = try featureDir(testing.allocator, "/wb", "", "p1", "root-plan");
    defer testing.allocator.free(result);
    try testing.expectEqualStrings("/wb/p1-root-plan", result);
}

test "featureDir: external key (JIRA-style)" {
    const result = try featureDir(testing.allocator, "/home/user/.planar/workbench", "org:eng", "JIRA-123", "add-auth");
    defer testing.allocator.free(result);
    try testing.expectEqualStrings("/home/user/.planar/workbench/org_eng/JIRA-123-add-auth", result);
}

test "featureDir confines traversal and hierarchical external keys" {
    const traversal = try featureDir(testing.allocator, "/wb", "org", "../../target", "plan");
    defer testing.allocator.free(traversal);
    try testing.expectEqualStrings("/wb/org/.._.._target-plan", traversal);

    const github = try featureDir(testing.allocator, "/wb", "org", "owner/repo#1", "plan");
    defer testing.allocator.free(github);
    try testing.expectEqualStrings("/wb/org/owner_repo#1-plan", github);
}

test "storedPath: basic" {
    const result = try storedPath(testing.allocator, "project:checkout", "p42", "my-plan", "tasks/cross/5-some-task.md");
    defer testing.allocator.free(result);
    try testing.expectEqualStrings("project_checkout/p42-my-plan/tasks/cross/5-some-task.md", result);
}

test "artifactFilename: normal title" {
    const result = try artifactFilename(testing.allocator, 7, "Tech Spec: Auth", "tech_spec");
    defer testing.allocator.free(result);
    try testing.expectEqualStrings("7-tech-spec-auth.md", result);
}

test "artifactFilename: empty title falls back to kind" {
    const result = try artifactFilename(testing.allocator, 3, "", "tech_spec");
    defer testing.allocator.free(result);
    try testing.expectEqualStrings("3-tech-spec.md", result);
}

test "artifactFilename: id prefix + slug" {
    const result = try artifactFilename(testing.allocator, 99, "My Roadmap", "roadmap");
    defer testing.allocator.free(result);
    try testing.expectEqualStrings("99-my-roadmap.md", result);
}

test "slugify: normal title" {
    const result = try slugify(testing.allocator, "Tech Spec: Auth");
    defer testing.allocator.free(result);
    try testing.expectEqualStrings("tech-spec-auth", result);
}

test "slugify: empty string returns untitled" {
    const result = try slugify(testing.allocator, "");
    defer testing.allocator.free(result);
    try testing.expectEqualStrings("untitled", result);
}

test "slugify: leading/trailing non-alnum stripped" {
    const result = try slugify(testing.allocator, "  Hello World!  ");
    defer testing.allocator.free(result);
    try testing.expectEqualStrings("hello-world", result);
}

test "slugify: all non-alnum returns untitled" {
    const result = try slugify(testing.allocator, "---");
    defer testing.allocator.free(result);
    try testing.expectEqualStrings("untitled", result);
}

test "slugify: numbers preserved" {
    const result = try slugify(testing.allocator, "Plan 226 v2");
    defer testing.allocator.free(result);
    try testing.expectEqualStrings("plan-226-v2", result);
}

test "canonicalEntityKind: test_scenario maps to scenario" {
    try testing.expectEqualStrings("scenario", canonicalEntityKind("test_scenario"));
}

test "canonicalEntityKind: other kinds pass through" {
    try testing.expectEqualStrings("task", canonicalEntityKind("task"));
    try testing.expectEqualStrings("plan", canonicalEntityKind("plan"));
    try testing.expectEqualStrings("artifact", canonicalEntityKind("artifact"));
    try testing.expectEqualStrings("decision", canonicalEntityKind("decision"));
    try testing.expectEqualStrings("question", canonicalEntityKind("question"));
}

test "slugify: long title is truncated to 60 chars and has no trailing hyphen" {
    // 65 alphanumeric chars → slug would be 65 chars without truncation.
    const title = "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz012";
    const result = try slugify(testing.allocator, title);
    defer testing.allocator.free(result);
    try testing.expect(result.len <= 60);
    try testing.expect(result.len > 0);
    try testing.expect(result[result.len - 1] != '-');
}

test "slugify: truncation at hyphen boundary removes trailing hyphen" {
    // Construct a title whose slug is exactly 61 chars before truncation,
    // with the 61st char being a separator so position 60 becomes '-'.
    // "aaa...aaa!bbb" where "aaa...aaa" is 60 chars → slug = "aaa...aaa-bbb" (64 chars).
    // After truncating to 60: "aaa...aaa-" → re-trim trailing '-' → "aaa...aaa" (59 chars, no trailing hyphen).
    const title = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa!bbb";
    const result = try slugify(testing.allocator, title);
    defer testing.allocator.free(result);
    try testing.expect(result.len <= 60);
    try testing.expect(result[result.len - 1] != '-');
}
