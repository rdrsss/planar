//! integration_tests/local_test.zig — local sandbox handlers (M17).

const std = @import("std");
const harness = @import("harness");

fn tmpDirForSuite(suite: *const harness.Suite) []const u8 {
    return std.fs.path.dirname(suite.db_path) orelse ".";
}

test "local import(no-link) + link + list + unlink works and emits JSON rows" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const tmp_dir = tmpDirForSuite(&suite);
    const local_home = try std.fs.path.join(gpa, &.{ tmp_dir, "local-home" });
    defer gpa.free(local_home);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, local_home);

    const ext_dir = try std.fs.path.join(gpa, &.{ tmp_dir, "external" });
    defer gpa.free(ext_dir);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, ext_dir);

    const src = try std.fs.path.join(gpa, &.{ ext_dir, "foo.md" });
    defer gpa.free(src);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = src,
        .data =
        \\---
        \\description: foo
        \\kind: skill
        \\vendors: [claude]
        \\---
        \\body
        ,
    });

    const env: []const harness.Suite.ExtraEnvEntry = &.{
        .{ .key = "PLANAR_LOCAL_HOME", .value = local_home },
    };

    const import_out = suite.mustRunWith(&.{ "local", "import", "--json", "--kind", "skill", "--no-link", src }, env);
    defer gpa.free(import_out);
    try std.testing.expect(std.mem.indexOf(u8, import_out, "\"Imported\"") != null);

    const link_out = suite.mustRunWith(&.{ "local", "link", "--json", "foo" }, env);
    defer gpa.free(link_out);
    try std.testing.expect(std.mem.indexOf(u8, link_out, "\"Source\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, link_out, "\"Records\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, link_out, "\"vendor\":\"claude\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, link_out, "\"linked_at\":\"now\"") == null);
    try std.testing.expect(std.mem.indexOf(u8, link_out, "\"linked_at\":\"20") != null);

    const list_out = suite.mustRunWith(&.{ "local", "list", "--json" }, env);
    defer gpa.free(list_out);
    try std.testing.expect(std.mem.indexOf(u8, list_out, "\"Name\":\"foo\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, list_out, "\"action\":\"live\"") != null);

    const unlink_out = suite.mustRunWith(&.{ "local", "unlink", "--json", "foo" }, env);
    defer gpa.free(unlink_out);
    try std.testing.expect(std.mem.indexOf(u8, unlink_out, "\"result\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, unlink_out, "\"Removed\"") != null);
}

test "local migrate converts legacy flat skill layout" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const tmp_dir = tmpDirForSuite(&suite);
    const local_home = try std.fs.path.join(gpa, &.{ tmp_dir, "local-home-migrate" });
    defer gpa.free(local_home);

    const skills = try std.fs.path.join(gpa, &.{ local_home, ".planar", "local", "skills" });
    defer gpa.free(skills);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, skills);

    const legacy = try std.fs.path.join(gpa, &.{ skills, "legacy.md" });
    defer gpa.free(legacy);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = legacy,
        .data =
        \\---
        \\description: legacy
        \\kind: skill
        \\---
        \\body
        ,
    });

    const env: []const harness.Suite.ExtraEnvEntry = &.{
        .{ .key = "PLANAR_LOCAL_HOME", .value = local_home },
    };

    const migrate_out = suite.mustRunWith(&.{ "local", "migrate", "--json" }, env);
    defer gpa.free(migrate_out);
    try std.testing.expect(std.mem.indexOf(u8, migrate_out, "\"Migrated\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, migrate_out, "\"Name\":\"legacy\"") != null);

    const migrated = try std.fs.path.join(gpa, &.{ skills, "legacy", "SKILL.md" });
    defer gpa.free(migrated);
    try std.Io.Dir.cwd().access(std.testing.io, migrated, .{});
}

test "local list marks broken when symlink target source is missing" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const tmp_dir = tmpDirForSuite(&suite);
    const local_home = try std.fs.path.join(gpa, &.{ tmp_dir, "local-home-broken" });
    defer gpa.free(local_home);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, local_home);

    const ext_dir = try std.fs.path.join(gpa, &.{ tmp_dir, "external-broken" });
    defer gpa.free(ext_dir);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, ext_dir);
    const src = try std.fs.path.join(gpa, &.{ ext_dir, "brk.md" });
    defer gpa.free(src);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = src,
        .data =
        \\---
        \\description: brk
        \\kind: skill
        \\vendors: [claude]
        \\---
        \\body
        ,
    });

    const env: []const harness.Suite.ExtraEnvEntry = &.{
        .{ .key = "PLANAR_LOCAL_HOME", .value = local_home },
    };
    const imported = suite.mustRunWith(&.{ "local", "import", "--kind", "skill", "--no-link", src }, env);
    defer gpa.free(imported);
    const linked = suite.mustRunWith(&.{ "local", "link", "brk" }, env);
    defer gpa.free(linked);

    const local_source = try std.fs.path.join(gpa, &.{ local_home, ".planar", "local", "skills", "brk", "SKILL.md" });
    defer gpa.free(local_source);
    try std.Io.Dir.cwd().deleteFile(std.testing.io, local_source);

    const listed = suite.mustRunWith(&.{ "local", "list", "--json" }, env);
    defer gpa.free(listed);
    try std.testing.expect(std.mem.indexOf(u8, listed, "\"action\":\"broken\"") != null);
}

test "local link --reconcile recreates consistency and emits JSON action rows" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const tmp_dir = tmpDirForSuite(&suite);
    const local_home = try std.fs.path.join(gpa, &.{ tmp_dir, "local-home-reconcile" });
    defer gpa.free(local_home);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, local_home);

    const ext_dir = try std.fs.path.join(gpa, &.{ tmp_dir, "external-reconcile" });
    defer gpa.free(ext_dir);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, ext_dir);
    const src = try std.fs.path.join(gpa, &.{ ext_dir, "rcn.md" });
    defer gpa.free(src);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = src,
        .data =
        \\---
        \\description: rcn
        \\kind: skill
        \\vendors: [claude]
        \\---
        \\body
        ,
    });

    const env: []const harness.Suite.ExtraEnvEntry = &.{
        .{ .key = "PLANAR_LOCAL_HOME", .value = local_home },
    };
    const imported = suite.mustRunWith(&.{ "local", "import", "--kind", "skill", "--no-link", src }, env);
    defer gpa.free(imported);
    const linked = suite.mustRunWith(&.{ "local", "link", "rcn" }, env);
    defer gpa.free(linked);

    const local_source = try std.fs.path.join(gpa, &.{ local_home, ".planar", "local", "skills", "rcn", "SKILL.md" });
    defer gpa.free(local_source);
    try std.Io.Dir.cwd().deleteFile(std.testing.io, local_source);

    const reconciled = suite.mustRunWith(&.{ "local", "link", "--reconcile", "--json" }, env);
    defer gpa.free(reconciled);
    try std.testing.expect(std.mem.indexOf(u8, reconciled, "\"name\":\"rcn\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, reconciled, "\"kind\":\"skill\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, reconciled, "\"reason\":\"source-missing\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, reconciled, "\"removed_targets\"") != null);

    const listed = suite.mustRunWith(&.{ "local", "list", "--json" }, env);
    defer gpa.free(listed);
    try std.testing.expect(std.mem.indexOf(u8, listed, "\"Name\":\"rcn\"") == null);
}

test "local link --reconcile recreates missing vendor target and emits JSON" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const tmp_dir = tmpDirForSuite(&suite);
    const local_home = try std.fs.path.join(gpa, &.{ tmp_dir, "local-home-repair-target" });
    defer gpa.free(local_home);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, local_home);

    const ext_dir = try std.fs.path.join(gpa, &.{ tmp_dir, "external-repair-target" });
    defer gpa.free(ext_dir);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, ext_dir);
    const src = try std.fs.path.join(gpa, &.{ ext_dir, "fix.md" });
    defer gpa.free(src);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = src,
        .data =
        \\---
        \\description: fix
        \\kind: skill
        \\vendors: [claude]
        \\---
        \\body
        ,
    });

    const env: []const harness.Suite.ExtraEnvEntry = &.{
        .{ .key = "PLANAR_LOCAL_HOME", .value = local_home },
    };
    const imported = suite.mustRunWith(&.{ "local", "import", "--kind", "skill", "--no-link", src }, env);
    defer gpa.free(imported);
    const linked = suite.mustRunWith(&.{ "local", "link", "fix" }, env);
    defer gpa.free(linked);

    const vendor_target = try std.fs.path.join(gpa, &.{ local_home, ".claude", "commands", "local-fix.md" });
    defer gpa.free(vendor_target);
    try std.Io.Dir.cwd().deleteFile(std.testing.io, vendor_target);

    const reconciled = suite.mustRunWith(&.{ "local", "link", "--reconcile", "--json" }, env);
    defer gpa.free(reconciled);
    try std.testing.expect(std.mem.indexOf(u8, reconciled, "\"name\":\"fix\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, reconciled, "\"reason\":\"target-missing\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, reconciled, "\"removed_targets\"") != null);

    try std.Io.Dir.cwd().access(std.testing.io, vendor_target, .{});
    const listed = suite.mustRunWith(&.{ "local", "list", "--json" }, env);
    defer gpa.free(listed);
    try std.testing.expect(std.mem.indexOf(u8, listed, "\"Name\":\"fix\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, listed, "\"action\":\"live\"") != null);
}
