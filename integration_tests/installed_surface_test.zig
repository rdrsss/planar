//! Black-box installed projection status and manifest-owned repair tests.

const std = @import("std");
const harness = @import("harness");

const digest_a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
const digest_b = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
const fresh_body = "---\nx-planar-source-digest: " ++ digest_a ++ "\nx-planar-projection-digest: " ++ digest_b ++ "\n---\nfresh\n";
const stale_body = "---\nx-planar-source-digest: " ++ digest_a ++ "\nx-planar-projection-digest: " ++ digest_b ++ "\n---\nstale\n";

const Row = struct {
    vendor: []const u8,
    kind: []const u8,
    name: []const u8,
    staged: []const u8,
    installed: []const u8,
    install_kind: []const u8,
};

fn envFor(home: []const u8, planar_home: []const u8, codex_home: []const u8) [3]harness.Suite.ExtraEnvEntry {
    return .{
        .{ .key = "HOME", .value = home },
        .{ .key = "PLANAR_HOME", .value = planar_home },
        .{ .key = "CODEX_HOME", .value = codex_home },
    };
}

fn appendDigestField(gpa: std.mem.Allocator, out: *std.ArrayList(u8), label: []const u8, value: []const u8) !void {
    const prefix = try std.fmt.allocPrint(gpa, "{s} {d}:", .{ label, value.len });
    defer gpa.free(prefix);
    try out.appendSlice(gpa, prefix);
    try out.appendSlice(gpa, value);
    try out.append(gpa, '\n');
}

fn fixtureProjectionDigest(gpa: std.mem.Allocator, row: Row) ![64]u8 {
    const raw = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, row.staged, gpa, .limited(4 * 1024 * 1024));
    defer gpa.free(raw);
    const first_line_end = std.mem.indexOfScalar(u8, raw, '\n').? + 1;
    const source_line_end = std.mem.indexOfScalarPos(u8, raw, first_line_end, '\n').? + 1;
    const projection_line_end = std.mem.indexOfScalarPos(u8, raw, source_line_end, '\n').? + 1;
    const payload = try std.mem.concat(gpa, u8, &.{ raw[0..first_line_end], raw[projection_line_end..] });
    defer gpa.free(payload);
    var canonical: std.ArrayList(u8) = .empty;
    defer canonical.deinit(gpa);
    try appendDigestField(gpa, &canonical, "domain", "planar-render-projection-v2");
    try appendDigestField(gpa, &canonical, "kind", row.kind);
    try appendDigestField(gpa, &canonical, "source_digest", digest_a);
    try appendDigestField(gpa, &canonical, "vendor", row.vendor);
    try appendDigestField(gpa, &canonical, "payload", payload);
    var digest: [std.crypto.hash.sha2.Sha256.digest_length]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(canonical.items, &digest, .{});
    return std.fmt.bytesToHex(digest, .lower);
}

fn stampFixtureDigest(gpa: std.mem.Allocator, path: []const u8, digest: []const u8) !void {
    const raw = std.Io.Dir.cwd().readFileAlloc(std.testing.io, path, gpa, .limited(4 * 1024 * 1024)) catch |e| switch (e) {
        error.FileNotFound, error.IsDir => return,
        else => return e,
    };
    defer gpa.free(raw);
    const at = std.mem.indexOf(u8, raw, digest_b) orelse return error.InvalidFixture;
    @memcpy(raw[at .. at + digest.len], digest);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = path, .data = raw });
}

fn writeManifest(gpa: std.mem.Allocator, planar_home: []const u8, vendors: []const []const u8, rows: []const Row) !void {
    try std.Io.Dir.cwd().createDirPath(std.testing.io, planar_home);
    var body: std.ArrayList(u8) = .empty;
    defer body.deinit(gpa);
    try body.appendSlice(gpa, "{\"version\":1,\"build_id\":\"fixture\",\"install_mode\":\"copy\",\"vendors\":[");
    for (vendors, 0..) |vendor, i| {
        if (i > 0) try body.append(gpa, ',');
        const encoded = try std.fmt.allocPrint(gpa, "\"{s}\"", .{vendor});
        defer gpa.free(encoded);
        try body.appendSlice(gpa, encoded);
    }
    try body.appendSlice(gpa, "],\"projections\":[");
    for (rows, 0..) |row, i| {
        const projection_digest = try fixtureProjectionDigest(gpa, row);
        try stampFixtureDigest(gpa, row.staged, &projection_digest);
        try stampFixtureDigest(gpa, row.installed, &projection_digest);
        if (i > 0) try body.append(gpa, ',');
        const encoded = try std.fmt.allocPrint(
            gpa,
            "{{\"vendor\":\"{s}\",\"kind\":\"{s}\",\"name\":\"{s}\",\"staged_path\":\"{s}\",\"installed_path\":\"{s}\",\"install_kind\":\"{s}\",\"source_digest\":\"{s}\",\"projection_digest\":\"{s}\"}}",
            .{ row.vendor, row.kind, row.name, row.staged, row.installed, row.install_kind, digest_a, projection_digest },
        );
        defer gpa.free(encoded);
        try body.appendSlice(gpa, encoded);
    }
    try body.appendSlice(gpa, "]}\n");
    const path = try std.fs.path.join(gpa, &.{ planar_home, "install-manifest.json" });
    defer gpa.free(path);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = path, .data = body.items });
}

fn writeFile(path: []const u8, body: []const u8) !void {
    try std.Io.Dir.cwd().createDirPath(std.testing.io, std.fs.path.dirname(path).?);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = path, .data = body });
}

fn appendSemanticTamper(gpa: std.mem.Allocator, path: []const u8) !void {
    const before = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, path, gpa, .limited(4 * 1024 * 1024));
    defer gpa.free(before);
    const after = try std.fmt.allocPrint(gpa, "{s}\nsemantic tamper with digest headers unchanged\n", .{before});
    defer gpa.free(after);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = path, .data = after });
}

fn inheritedEnv(gpa: std.mem.Allocator) !std.process.Environ.Map {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var count: usize = 0;
    while (raw[count] != null) : (count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..count :null]);
    const environ: std.process.Environ = .{ .block = .{ .slice = env_slice } };
    return environ.createMap(gpa);
}

fn runInstaller(gpa: std.mem.Allocator, repo_root: []const u8, home: []const u8, prefix: []const u8, codex_home: []const u8, fake_bin: []const u8, link: bool) !std.process.RunResult {
    const install = try std.fs.path.join(gpa, &.{ repo_root, "install.sh" });
    defer gpa.free(install);
    var env = try inheritedEnv(gpa);
    defer env.deinit();
    const path = try std.fmt.allocPrint(gpa, "{s}:{s}", .{ fake_bin, env.get("PATH") orelse "" });
    defer gpa.free(path);
    try env.put("HOME", home);
    try env.put("PLANAR_HOME", prefix);
    try env.put("CODEX_HOME", codex_home);
    try env.put("PATH", path);
    try env.put("NO_COLOR", "1");
    const argv: []const []const u8 = if (link)
        &.{ "bash", install, "--prefix", prefix, "--vendors", "codex", "--link", "--optimize", "Debug" }
    else
        &.{ "bash", install, "--prefix", prefix, "--vendors", "codex", "--optimize", "Debug" };
    return std.process.run(gpa, std.testing.io, .{ .argv = argv, .environ_map = &env });
}

test "selected-vendor installer lifecycle writes a fresh manifest in copy and link modes" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    const home = suite.tmpAbsPath();
    const prefix = try std.fs.path.join(gpa, &.{ home, ".planar-install" });
    defer gpa.free(prefix);
    const codex_home = try std.fs.path.join(gpa, &.{ home, ".codex-install" });
    defer gpa.free(codex_home);
    const fake_bin = try std.fs.path.join(gpa, &.{ home, "fake-bin" });
    defer gpa.free(fake_bin);
    const fake_zig = try std.fs.path.join(gpa, &.{ fake_bin, "zig" });
    defer gpa.free(fake_zig);
    const personal = try std.fs.path.join(gpa, &.{ codex_home, "skills", "personal", "SKILL.md" });
    defer gpa.free(personal);
    const unselected = try std.fs.path.join(gpa, &.{ home, ".claude", "commands", "operator.md" });
    defer gpa.free(unselected);
    try writeFile(personal, "personal extension\n");
    try writeFile(unselected, "unselected vendor extension\n");

    // The installer build step is orthogonal to this seam. Replace only `zig`
    // with a fixture that installs the already-built black-box test binary;
    // every subsequent render, vendor wiring, manifest, status, and health
    // operation still runs through the public installer/CLI surfaces.
    try writeFile(fake_zig,
        \\#!/usr/bin/env bash
        \\set -euo pipefail
        \\if [[ "${1:-}" == version ]]; then printf '0.16.0\n'; exit 0; fi
        \\[[ "${1:-}" == build ]]
        \\prefix=""
        \\while [[ $# -gt 0 ]]; do
        \\  if [[ "$1" == --prefix ]]; then prefix="$2"; shift 2; else shift; fi
        \\done
        \\mkdir -p "$prefix/bin"
        \\for name in planar planar-agent planar-watch planar-doc planar-execute; do
        \\  cp "$PLANAR_BIN" "$prefix/bin/$name"
        \\done
    );
    const chmod = try std.process.run(gpa, std.testing.io, .{ .argv = &.{ "chmod", "+x", fake_zig } });
    defer gpa.free(chmod.stdout);
    defer gpa.free(chmod.stderr);
    try std.testing.expect(chmod.term == .exited and chmod.term.exited == 0);

    const repo_root = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", gpa);
    defer gpa.free(repo_root);
    const env = envFor(home, prefix, codex_home);
    inline for (.{ false, true }) |link| {
        const installed = try runInstaller(gpa, repo_root, home, prefix, codex_home, fake_bin, link);
        defer gpa.free(installed.stdout);
        defer gpa.free(installed.stderr);
        if (installed.term != .exited or installed.term.exited != 0) {
            std.debug.print("\ninstaller stdout:\n{s}\ninstaller stderr:\n{s}\n", .{ installed.stdout, installed.stderr });
        }
        try std.testing.expect(installed.term == .exited and installed.term.exited == 0);

        const manifest_path = try std.fs.path.join(gpa, &.{ prefix, "install-manifest.json" });
        defer gpa.free(manifest_path);
        const manifest = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, manifest_path, gpa, .limited(4 * 1024 * 1024));
        defer gpa.free(manifest);
        try std.testing.expect(std.mem.indexOf(u8, manifest, if (link) "\"install_mode\": \"link\"" else "\"install_mode\": \"copy\"") != null);
        try std.testing.expect(std.mem.indexOf(u8, manifest, "\"vendors\": [\"codex\"]") != null);
        try std.testing.expect(std.mem.indexOf(u8, manifest, "\"vendor\": \"claude\"") == null);
        try std.testing.expect(std.mem.indexOf(u8, manifest, "\"vendor\": \"copilot\"") == null);
        try std.testing.expect(std.mem.indexOf(u8, manifest, "\"install_kind\": \"copy\"") != null);
        try std.testing.expect(std.mem.indexOf(u8, manifest, "\"install_kind\": \"link\"") != null);

        const status = suite.mustRunWith(&.{ "skills", "status", "--json" }, &env);
        defer gpa.free(status);
        try std.testing.expect(std.mem.indexOf(u8, status, "\"manifest\":{\"status\":\"current\"") != null);
        try std.testing.expect(std.mem.indexOf(u8, status, "\"stale\":0") != null);
        try std.testing.expect(std.mem.indexOf(u8, status, "\"missing\":0") != null);
        try std.testing.expect(std.mem.indexOf(u8, status, "\"vendor\":\"claude\",\"status\":\"unselected\"") != null);

        const health = suite.mustRunWith(&.{ "health", "--json" }, &env);
        defer gpa.free(health);
        try std.testing.expect(std.mem.indexOf(u8, health, "\"projection_freshness\":{\"state\":\"fresh\"") != null);
        try std.testing.expect(std.mem.indexOf(u8, health, "\"overall\":\"ok\"") != null);

        const staged_status = try std.fs.path.join(gpa, &.{ prefix, "codex-skills", "pl-status", "SKILL.md" });
        defer gpa.free(staged_status);
        const installed_status = try std.fs.path.join(gpa, &.{ codex_home, "skills", "pl-status", "SKILL.md" });
        defer gpa.free(installed_status);
        try appendSemanticTamper(gpa, staged_status);
        if (!link) try appendSemanticTamper(gpa, installed_status);

        const tampered = suite.mustRunWith(&.{ "skills", "status", "--json" }, &env);
        defer gpa.free(tampered);
        try std.testing.expect(std.mem.indexOf(u8, tampered, "\"stale\":1") != null);

        const personal_after = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, personal, gpa, .limited(1024));
        defer gpa.free(personal_after);
        try std.testing.expectEqualStrings("personal extension\n", personal_after);
        const unselected_after = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, unselected, gpa, .limited(1024));
        defer gpa.free(unselected_after);
        try std.testing.expectEqualStrings("unselected vendor extension\n", unselected_after);
    }
}

test "health reports fresh managed and unmanaged projections without degradation or writes" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    const home = suite.tmpAbsPath();
    const planar_home = try std.fs.path.join(gpa, &.{ home, ".planar" });
    defer gpa.free(planar_home);
    const codex_home = try std.fs.path.join(gpa, &.{ home, ".codex" });
    defer gpa.free(codex_home);
    const staged = try std.fs.path.join(gpa, &.{ planar_home, "codex-skills", "pl-ok", "SKILL.md" });
    defer gpa.free(staged);
    const installed = try std.fs.path.join(gpa, &.{ codex_home, "skills", "pl-ok", "SKILL.md" });
    defer gpa.free(installed);
    const personal = try std.fs.path.join(gpa, &.{ codex_home, "skills", "personal", "SKILL.md" });
    defer gpa.free(personal);
    try writeFile(staged, fresh_body);
    try writeFile(installed, fresh_body);
    try writeFile(personal, "operator owned\n");
    const rows = [_]Row{.{ .vendor = "codex", .kind = "skill", .name = "pl-ok", .staged = staged, .installed = installed, .install_kind = "copy" }};
    try writeManifest(gpa, planar_home, &.{"codex"}, &rows);
    const manifest_path = try std.fs.path.join(gpa, &.{ planar_home, "install-manifest.json" });
    defer gpa.free(manifest_path);
    const manifest_before = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, manifest_path, gpa, .limited(8192));
    defer gpa.free(manifest_before);
    const personal_before = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, personal, gpa, .limited(1024));
    defer gpa.free(personal_before);
    const env = envFor(home, planar_home, codex_home);

    const json = suite.mustRunWith(&.{ "health", "--json" }, &env);
    defer gpa.free(json);
    try std.testing.expect(std.mem.indexOf(u8, json, "\"projection_freshness\":{\"state\":\"fresh\",\"manifest_status\":\"current\",\"managed\":1,\"fresh\":1,\"stale\":0,\"missing\":0,\"unmanaged\":1,\"unselected_vendors\":2,\"evidence\":null,\"repair_command\":null}") != null);
    try std.testing.expect(std.mem.indexOf(u8, json, "\"overall\":\"ok\"") != null);

    const text = suite.mustRunWith(&.{"health"}, &env);
    defer gpa.free(text);
    const freshness_at = std.mem.indexOf(u8, text, "projection freshness: fresh (1 managed: 1 fresh, 0 stale, 0 missing; 1 unmanaged; 2 unselected vendors)").?;
    const manifest_at = std.mem.indexOf(u8, text, "projection manifest:  current").?;
    const overall_at = std.mem.indexOf(u8, text, "overall:          ok").?;
    try std.testing.expect(freshness_at < manifest_at and manifest_at < overall_at);
    try std.testing.expect(std.mem.indexOf(u8, text, "projection repair:") == null);

    const manifest_after = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, manifest_path, gpa, .limited(8192));
    defer gpa.free(manifest_after);
    const personal_after = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, personal, gpa, .limited(1024));
    defer gpa.free(personal_after);
    try std.testing.expectEqualStrings(manifest_before, manifest_after);
    try std.testing.expectEqualStrings(personal_before, personal_after);
}

test "health degrades for stale and missing managed projections and preserves exact evidence" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    const home = suite.tmpAbsPath();
    const planar_home = try std.fs.path.join(gpa, &.{ home, ".planar" });
    defer gpa.free(planar_home);
    const codex_home = try std.fs.path.join(gpa, &.{ home, ".codex" });
    defer gpa.free(codex_home);
    const stale_staged = try std.fs.path.join(gpa, &.{ planar_home, "codex-skills", "stale", "SKILL.md" });
    defer gpa.free(stale_staged);
    const stale_installed = try std.fs.path.join(gpa, &.{ codex_home, "skills", "stale", "SKILL.md" });
    defer gpa.free(stale_installed);
    const missing_staged = try std.fs.path.join(gpa, &.{ planar_home, "codex-skills", "missing", "SKILL.md" });
    defer gpa.free(missing_staged);
    const missing_installed = try std.fs.path.join(gpa, &.{ codex_home, "skills", "missing", "SKILL.md" });
    defer gpa.free(missing_installed);
    try writeFile(stale_staged, fresh_body);
    try writeFile(stale_installed, stale_body);
    try writeFile(missing_staged, fresh_body);
    const rows = [_]Row{
        .{ .vendor = "codex", .kind = "skill", .name = "stale", .staged = stale_staged, .installed = stale_installed, .install_kind = "copy" },
        .{ .vendor = "codex", .kind = "skill", .name = "missing", .staged = missing_staged, .installed = missing_installed, .install_kind = "copy" },
    };
    try writeManifest(gpa, planar_home, &.{"codex"}, &rows);
    const env = envFor(home, planar_home, codex_home);
    const installed_before = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, stale_installed, gpa, .limited(1024));
    defer gpa.free(installed_before);

    const result = suite.execWith(&.{ "health", "--json" }, &env);
    defer result.deinit(gpa);
    try std.testing.expect(result.term == .exited and result.term.exited == 1);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "\"state\":\"degraded\",\"manifest_status\":\"current\",\"managed\":2,\"fresh\":0,\"stale\":1,\"missing\":1") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "\"evidence\":\"managed projections differ from the staged installation authority\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "\"repair_command\":\"planar skills repair --apply\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "\"overall\":\"degraded\"") != null);
    const text_result = suite.execWith(&.{"health"}, &env);
    defer text_result.deinit(gpa);
    try std.testing.expect(text_result.term == .exited and text_result.term.exited == 1);
    const freshness_at = std.mem.indexOf(u8, text_result.stdout, "projection freshness: degraded (2 managed: 0 fresh, 1 stale, 1 missing; 0 unmanaged; 2 unselected vendors)").?;
    const evidence_at = std.mem.indexOf(u8, text_result.stdout, "projection evidence:  managed projections differ from the staged installation authority").?;
    const repair_at = std.mem.indexOf(u8, text_result.stdout, "projection repair:    planar skills repair --apply").?;
    const overall_at = std.mem.indexOf(u8, text_result.stdout, "overall:          degraded").?;
    try std.testing.expect(freshness_at < evidence_at and evidence_at < repair_at and repair_at < overall_at);
    const installed_after = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, stale_installed, gpa, .limited(1024));
    defer gpa.free(installed_after);
    try std.testing.expectEqualStrings(installed_before, installed_after);
    try std.testing.expectError(error.FileNotFound, std.Io.Dir.cwd().access(std.testing.io, missing_installed, .{}));
}

test "health reports aggregate manifest recovery contributors without writes" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    const home = suite.tmpAbsPath();
    const planar_home = try std.fs.path.join(gpa, &.{ home, "legacy-planar" });
    defer gpa.free(planar_home);
    const codex_home = try std.fs.path.join(gpa, &.{ home, ".codex" });
    defer gpa.free(codex_home);
    const stamp = try std.fs.path.join(gpa, &.{ planar_home, ".planar-install" });
    defer gpa.free(stamp);
    try writeFile(stamp, "legacy ownership\n");
    const env = envFor(home, planar_home, codex_home);

    const result = suite.execWith(&.{ "health", "--json" }, &env);
    defer result.deinit(gpa);
    try std.testing.expect(result.term == .exited and result.term.exited == 1);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "\"state\":\"degraded\",\"manifest_status\":\"legacy\",\"managed\":0") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "legacy ownership stamp exists but the versioned install manifest is missing") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "./install.sh --prefix") != null);
    const stamp_after = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, stamp, gpa, .limited(1024));
    defer gpa.free(stamp_after);
    try std.testing.expectEqualStrings("legacy ownership\n", stamp_after);

    const manifest = try std.fs.path.join(gpa, &.{ planar_home, "install-manifest.json" });
    defer gpa.free(manifest);
    try writeFile(manifest, "not json\n");
    const invalid = suite.execWith(&.{ "health", "--json" }, &env);
    defer invalid.deinit(gpa);
    try std.testing.expect(invalid.term == .exited and invalid.term.exited == 1);
    try std.testing.expect(std.mem.indexOf(u8, invalid.stdout, "\"manifest_status\":\"invalid\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, invalid.stdout, "\"evidence\":\"install manifest is invalid\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, invalid.stdout, "./install.sh --prefix") != null);

    try writeFile(manifest, "{\"version\":99}\n");
    const unsupported = suite.execWith(&.{ "health", "--json" }, &env);
    defer unsupported.deinit(gpa);
    try std.testing.expect(unsupported.term == .exited and unsupported.term.exited == 1);
    try std.testing.expect(std.mem.indexOf(u8, unsupported.stdout, "\"manifest_status\":\"unsupported\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, unsupported.stdout, "\"evidence\":\"install manifest version is unsupported\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, unsupported.stdout, "./install.sh --prefix") != null);
}

test "skills status reports fresh unmanaged and unselected without writing" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    const home = suite.tmpAbsPath();
    const planar_home = try std.fs.path.join(gpa, &.{ home, ".planar" });
    defer gpa.free(planar_home);
    const codex_home = try std.fs.path.join(gpa, &.{ home, ".codex" });
    defer gpa.free(codex_home);
    const staged = try std.fs.path.join(gpa, &.{ planar_home, "codex-skills", "pl-ok", "SKILL.md" });
    defer gpa.free(staged);
    const installed = try std.fs.path.join(gpa, &.{ codex_home, "skills", "pl-ok", "SKILL.md" });
    defer gpa.free(installed);
    const personal = try std.fs.path.join(gpa, &.{ codex_home, "skills", "personal", "SKILL.md" });
    defer gpa.free(personal);
    try writeFile(staged, fresh_body);
    try writeFile(installed, fresh_body);
    try writeFile(personal, "personal extension\n");
    const rows = [_]Row{.{ .vendor = "codex", .kind = "skill", .name = "pl-ok", .staged = staged, .installed = installed, .install_kind = "copy" }};
    try writeManifest(gpa, planar_home, &.{"codex"}, &rows);
    const before = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, personal, gpa, .limited(1024));
    defer gpa.free(before);
    const env = envFor(home, planar_home, codex_home);
    const out = suite.mustRunWith(&.{ "skills", "status", "--json" }, &env);
    defer gpa.free(out);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"status\":\"fresh\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"status\":\"unmanaged\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"vendor\":\"claude\",\"status\":\"unselected\"") != null);
    const after = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, personal, gpa, .limited(1024));
    defer gpa.free(after);
    try std.testing.expectEqualStrings(before, after);
    const refused = suite.execWith(&.{ "skills", "repair", "personal", "--apply", "--json" }, &env);
    defer refused.deinit(gpa);
    try std.testing.expect(refused.term == .exited and refused.term.exited == 2);
}

test "skills repair previews without writes then repairs copy and link and rechecks fresh" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    const home = suite.tmpAbsPath();
    const planar_home = try std.fs.path.join(gpa, &.{ home, ".planar" });
    defer gpa.free(planar_home);
    const codex_home = try std.fs.path.join(gpa, &.{ home, ".codex" });
    defer gpa.free(codex_home);
    const copy_staged = try std.fs.path.join(gpa, &.{ planar_home, "codex-skills", "copy", "SKILL.md" });
    defer gpa.free(copy_staged);
    const copy_installed = try std.fs.path.join(gpa, &.{ codex_home, "skills", "copy", "SKILL.md" });
    defer gpa.free(copy_installed);
    const link_staged = try std.fs.path.join(gpa, &.{ planar_home, "agents", "codex", "link.toml" });
    defer gpa.free(link_staged);
    const link_installed = try std.fs.path.join(gpa, &.{ codex_home, "agents", "link.toml" });
    defer gpa.free(link_installed);
    const missing_staged = try std.fs.path.join(gpa, &.{ planar_home, "codex-skills", "missing", "SKILL.md" });
    defer gpa.free(missing_staged);
    const missing_installed = try std.fs.path.join(gpa, &.{ codex_home, "skills", "missing", "SKILL.md" });
    defer gpa.free(missing_installed);
    const personal = try std.fs.path.join(gpa, &.{ codex_home, "skills", "personal", "SKILL.md" });
    defer gpa.free(personal);
    const unselected = try std.fs.path.join(gpa, &.{ home, ".claude", "commands", "operator.md" });
    defer gpa.free(unselected);
    try writeFile(copy_staged, fresh_body);
    try writeFile(copy_installed, stale_body);
    try writeFile(link_staged, fresh_body);
    try writeFile(link_installed, stale_body);
    try writeFile(missing_staged, fresh_body);
    try writeFile(personal, "keep me\n");
    try writeFile(unselected, "unselected vendor extension\n");
    const rows = [_]Row{
        .{ .vendor = "codex", .kind = "skill", .name = "copy", .staged = copy_staged, .installed = copy_installed, .install_kind = "copy" },
        .{ .vendor = "codex", .kind = "agent", .name = "link", .staged = link_staged, .installed = link_installed, .install_kind = "link" },
        .{ .vendor = "codex", .kind = "skill", .name = "missing", .staged = missing_staged, .installed = missing_installed, .install_kind = "copy" },
    };
    try writeManifest(gpa, planar_home, &.{"codex"}, &rows);
    const expected_stale = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, copy_installed, gpa, .limited(1024));
    defer gpa.free(expected_stale);
    const env = envFor(home, planar_home, codex_home);
    const preview = suite.mustRunWith(&.{ "skills", "repair", "--json", "copy", "link", "missing" }, &env);
    defer gpa.free(preview);
    try std.testing.expect(std.mem.indexOf(u8, preview, "\"mode\":\"preview\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, preview, "\"attempted\":3") != null);
    try std.testing.expect(std.mem.indexOf(u8, preview, "\"applied\":0") != null);
    const still_stale = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, copy_installed, gpa, .limited(1024));
    defer gpa.free(still_stale);
    try std.testing.expectEqualStrings(expected_stale, still_stale);
    const applied = suite.mustRunWith(&.{ "skills", "repair", "--apply", "--json", "copy", "link", "missing" }, &env);
    defer gpa.free(applied);
    try std.testing.expect(std.mem.indexOf(u8, applied, "\"outcome\":\"ok\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, applied, "\"applied\":3") != null);
    var link_buf: [std.fs.max_path_bytes]u8 = undefined;
    const link_len = try std.Io.Dir.cwd().readLink(std.testing.io, link_installed, &link_buf);
    try std.testing.expectEqualStrings(link_staged, link_buf[0..link_len]);
    const status_out = suite.mustRunWith(&.{ "skills", "status", "--vendor", "codex", "--json" }, &env);
    defer gpa.free(status_out);
    try std.testing.expect(std.mem.indexOf(u8, status_out, "\"fresh\":3") != null);
    try std.testing.expect(std.mem.indexOf(u8, status_out, "\"stale\":0") != null);
    try std.testing.expect(std.mem.indexOf(u8, status_out, "\"missing\":0") != null);

    const health = suite.mustRunWith(&.{ "health", "--json" }, &env);
    defer gpa.free(health);
    try std.testing.expect(std.mem.indexOf(u8, health, "\"projection_freshness\":{\"state\":\"fresh\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, health, "\"overall\":\"ok\"") != null);

    const second_apply = suite.mustRunWith(&.{ "skills", "repair", "--apply", "--json", "copy", "link", "missing" }, &env);
    defer gpa.free(second_apply);
    try std.testing.expect(std.mem.indexOf(u8, second_apply, "\"outcome\":\"ok\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, second_apply, "\"attempted\":0") != null);
    try std.testing.expect(std.mem.indexOf(u8, second_apply, "\"applied\":0") != null);
    try std.testing.expect(std.mem.indexOf(u8, second_apply, "\"skipped\":3") != null);

    const personal_body = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, personal, gpa, .limited(1024));
    defer gpa.free(personal_body);
    try std.testing.expectEqualStrings("keep me\n", personal_body);
    const unselected_body = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, unselected, gpa, .limited(1024));
    defer gpa.free(unselected_body);
    try std.testing.expectEqualStrings("unselected vendor extension\n", unselected_body);
}

test "untrusted staged authority routes status and repair preview to reinstall" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    const home = suite.tmpAbsPath();
    const planar_home = try std.fs.path.join(gpa, &.{ home, ".planar" });
    defer gpa.free(planar_home);
    const codex_home = try std.fs.path.join(gpa, &.{ home, ".codex" });
    defer gpa.free(codex_home);
    const unavailable_staged = try std.fs.path.join(gpa, &.{ planar_home, "codex-skills", "unavailable", "SKILL.md" });
    defer gpa.free(unavailable_staged);
    const unavailable_installed = try std.fs.path.join(gpa, &.{ codex_home, "skills", "unavailable", "SKILL.md" });
    defer gpa.free(unavailable_installed);
    const invalid_staged = try std.fs.path.join(gpa, &.{ planar_home, "codex-skills", "invalid", "SKILL.md" });
    defer gpa.free(invalid_staged);
    const invalid_installed = try std.fs.path.join(gpa, &.{ codex_home, "skills", "invalid", "SKILL.md" });
    defer gpa.free(invalid_installed);
    const mismatched_staged = try std.fs.path.join(gpa, &.{ planar_home, "codex-skills", "mismatched", "SKILL.md" });
    defer gpa.free(mismatched_staged);
    const mismatched_installed = try std.fs.path.join(gpa, &.{ codex_home, "skills", "mismatched", "SKILL.md" });
    defer gpa.free(mismatched_installed);
    try writeFile(unavailable_staged, fresh_body);
    try writeFile(unavailable_installed, stale_body);
    try writeFile(invalid_staged, fresh_body);
    try writeFile(invalid_installed, stale_body);
    try writeFile(mismatched_staged, fresh_body);
    try writeFile(mismatched_installed, stale_body);
    const rows = [_]Row{
        .{ .vendor = "codex", .kind = "skill", .name = "unavailable", .staged = unavailable_staged, .installed = unavailable_installed, .install_kind = "copy" },
        .{ .vendor = "codex", .kind = "skill", .name = "invalid", .staged = invalid_staged, .installed = invalid_installed, .install_kind = "copy" },
        .{ .vendor = "codex", .kind = "skill", .name = "mismatched", .staged = mismatched_staged, .installed = mismatched_installed, .install_kind = "copy" },
    };
    try writeManifest(gpa, planar_home, &.{"codex"}, &rows);
    try std.Io.Dir.cwd().deleteFile(std.testing.io, unavailable_staged);
    try writeFile(invalid_staged, "no projection digest metadata\n");
    try appendSemanticTamper(gpa, mismatched_staged);
    const env = envFor(home, planar_home, codex_home);

    const status = suite.mustRunWith(&.{ "skills", "status", "--json" }, &env);
    defer gpa.free(status);
    try std.testing.expectEqual(@as(usize, 4), std.mem.count(u8, status, "./install.sh --prefix"));
    try std.testing.expect(std.mem.indexOf(u8, status, "planar skills repair unavailable --vendor codex --apply") == null);
    try std.testing.expect(std.mem.indexOf(u8, status, "planar skills repair invalid --vendor codex --apply") == null);
    try std.testing.expect(std.mem.indexOf(u8, status, "planar skills repair mismatched --vendor codex --apply") == null);

    const preview = suite.execWith(&.{ "skills", "repair", "--json" }, &env);
    defer preview.deinit(gpa);
    try std.testing.expect(preview.term == .exited and preview.term.exited == 1);
    try std.testing.expect(std.mem.indexOf(u8, preview.stdout, "\"action\":\"would-repair\"") == null);
    try std.testing.expect(std.mem.indexOf(u8, preview.stdout, "planar skills repair --apply") == null);
    try std.testing.expect(std.mem.indexOf(u8, preview.stdout, "./install.sh --prefix") != null);
}

test "skills status distinguishes missing legacy invalid and unsupported manifests" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    const home = suite.tmpAbsPath();
    const planar_home = try std.fs.path.join(gpa, &.{ home, "bootstrap" });
    defer gpa.free(planar_home);
    const codex_home = try std.fs.path.join(gpa, &.{ home, ".codex" });
    defer gpa.free(codex_home);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, planar_home);
    const env = envFor(home, planar_home, codex_home);
    const missing = suite.mustRunWith(&.{ "skills", "status", "--json" }, &env);
    defer gpa.free(missing);
    try std.testing.expect(std.mem.indexOf(u8, missing, "\"status\":\"missing\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, missing, "./install.sh --prefix") != null);
    const stamp = try std.fs.path.join(gpa, &.{ planar_home, ".planar-install" });
    defer gpa.free(stamp);
    try writeFile(stamp, "planar-install 1\n");
    const legacy = suite.mustRunWith(&.{ "skills", "status", "--json" }, &env);
    defer gpa.free(legacy);
    try std.testing.expect(std.mem.indexOf(u8, legacy, "\"status\":\"legacy\"") != null);
    const manifest = try std.fs.path.join(gpa, &.{ planar_home, "install-manifest.json" });
    defer gpa.free(manifest);
    try writeFile(manifest, "bad json\n");
    const invalid = suite.mustRunWith(&.{ "skills", "status", "--json" }, &env);
    defer gpa.free(invalid);
    try std.testing.expect(std.mem.indexOf(u8, invalid, "\"status\":\"invalid\"") != null);
    try writeFile(manifest, "{\"version\":99}\n");
    const unsupported = suite.mustRunWith(&.{ "skills", "status", "--json" }, &env);
    defer gpa.free(unsupported);
    try std.testing.expect(std.mem.indexOf(u8, unsupported, "\"status\":\"unsupported\"") != null);
    const repair = suite.execWith(&.{ "skills", "repair", "--apply", "--json" }, &env);
    defer repair.deinit(gpa);
    try std.testing.expect(repair.term == .exited and repair.term.exited == 1);
    try std.testing.expect(std.mem.indexOf(u8, repair.stdout, "\"manifest_status\":\"unsupported\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, repair.stdout, "./install.sh --prefix") != null);
}

test "skills repair reports precise partial apply and leaves failed target stale" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    const home = suite.tmpAbsPath();
    const planar_home = try std.fs.path.join(gpa, &.{ home, ".planar" });
    defer gpa.free(planar_home);
    const codex_home = try std.fs.path.join(gpa, &.{ home, ".codex" });
    defer gpa.free(codex_home);
    const good_staged = try std.fs.path.join(gpa, &.{ planar_home, "codex-skills", "a-good", "SKILL.md" });
    defer gpa.free(good_staged);
    const good_installed = try std.fs.path.join(gpa, &.{ codex_home, "skills", "a-good", "SKILL.md" });
    defer gpa.free(good_installed);
    const bad_staged = try std.fs.path.join(gpa, &.{ planar_home, "codex-skills", "z-bad", "SKILL.md" });
    defer gpa.free(bad_staged);
    const bad_installed = try std.fs.path.join(gpa, &.{ codex_home, "skills", "z-bad", "SKILL.md" });
    defer gpa.free(bad_installed);
    try writeFile(good_staged, fresh_body);
    try writeFile(good_installed, stale_body);
    try writeFile(bad_staged, fresh_body);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, bad_installed);
    const rows = [_]Row{
        .{ .vendor = "codex", .kind = "skill", .name = "a-good", .staged = good_staged, .installed = good_installed, .install_kind = "copy" },
        .{ .vendor = "codex", .kind = "skill", .name = "z-bad", .staged = bad_staged, .installed = bad_installed, .install_kind = "copy" },
    };
    try writeManifest(gpa, planar_home, &.{"codex"}, &rows);
    const env = envFor(home, planar_home, codex_home);
    const result = suite.execWith(&.{ "skills", "repair", "--apply", "--json" }, &env);
    defer result.deinit(gpa);
    try std.testing.expect(result.term == .exited and result.term.exited == 1);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "\"outcome\":\"partial\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "\"applied\":1") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "\"failed\":1") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "\"error_name\":\"UnsafeDestination\"") != null);
    const recheck = suite.mustRunWith(&.{ "skills", "status", "--vendor", "codex", "--json" }, &env);
    defer gpa.free(recheck);
    try std.testing.expect(std.mem.indexOf(u8, recheck, "\"fresh\":1") != null);
    try std.testing.expect(std.mem.indexOf(u8, recheck, "\"stale\":1") != null);

    try std.Io.Dir.cwd().deleteTree(std.testing.io, bad_installed);
    const recovered = suite.mustRunWith(&.{ "skills", "repair", "z-bad", "--apply", "--json" }, &env);
    defer gpa.free(recovered);
    try std.testing.expect(std.mem.indexOf(u8, recovered, "\"outcome\":\"ok\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, recovered, "\"attempted\":1") != null);
    try std.testing.expect(std.mem.indexOf(u8, recovered, "\"applied\":1") != null);
    try std.testing.expect(std.mem.indexOf(u8, recovered, "\"failed\":0") != null);

    const recovered_status = suite.mustRunWith(&.{ "skills", "status", "--vendor", "codex", "--json" }, &env);
    defer gpa.free(recovered_status);
    try std.testing.expect(std.mem.indexOf(u8, recovered_status, "\"fresh\":2") != null);
    try std.testing.expect(std.mem.indexOf(u8, recovered_status, "\"stale\":0") != null);
    try std.testing.expect(std.mem.indexOf(u8, recovered_status, "\"missing\":0") != null);

    const recovered_health = suite.mustRunWith(&.{ "health", "--json" }, &env);
    defer gpa.free(recovered_health);
    try std.testing.expect(std.mem.indexOf(u8, recovered_health, "\"projection_freshness\":{\"state\":\"fresh\"") != null);
}
