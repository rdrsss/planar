//! integration_tests/config_test.zig
//!
//! Black-box integration tests for `planar config` sub-commands.
//!
//! Tests:
//!   1. `planar config path` exits 0 and output contains "config.toml".
//!   2. `planar config init` on a fresh PLANAR_CONFIG_PATH exits 0 and creates the file.
//!   3. Second `planar config init` on an existing file exits 0 (idempotent).
//!   4. `planar config show --defaults` exits 0 and output contains "vendor" and "claude".
//!   5. `planar config validate` on an init'd config file exits 0.
//!   6. `planar config validate` on a malformed file exits non-zero with "line" in stderr.
//!
//! All tests inject PLANAR_CONFIG_PATH into a path inside the suite's temp
//! dir to avoid touching the operator's ~/.planar/config.toml.
//!
//! Run via: zig build test-integration

const std = @import("std");
const harness = @import("harness");

/// Return the config.toml path inside the suite's temp dir.
/// Caller must free the returned slice via suite.allocator.free().
fn configPathInTmp(suite: *const harness.Suite) []u8 {
    const dir = std.fs.path.dirname(suite.db_path) orelse ".";
    return std.fs.path.join(suite.allocator, &.{ dir, "config.toml" }) catch
        @panic("OOM building config_path");
}

test "config path: exits 0 and output contains config.toml" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const cfg_path = configPathInTmp(&suite);
    defer gpa.free(cfg_path);

    const extra: []const harness.Suite.ExtraEnvEntry = &.{
        .{ .key = "PLANAR_CONFIG_PATH", .value = cfg_path },
    };
    const stdout = suite.mustRunWith(&.{ "config", "path" }, extra);
    defer gpa.free(stdout);

    try std.testing.expect(std.mem.indexOf(u8, stdout, "config.toml") != null);
}

test "config init: creates file on fresh PLANAR_CONFIG_PATH, exits 0" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const cfg_path = configPathInTmp(&suite);
    defer gpa.free(cfg_path);

    const extra: []const harness.Suite.ExtraEnvEntry = &.{
        .{ .key = "PLANAR_CONFIG_PATH", .value = cfg_path },
    };

    // Run init — should create the file.
    const stdout = suite.mustRunWith(&.{ "config", "init" }, extra);
    defer gpa.free(stdout);

    // File must now exist — access() returns error.FileNotFound when absent.
    std.Io.Dir.cwd().access(std.testing.io, cfg_path, .{}) catch |e| {
        std.debug.print("\nconfig init: file not created ({s}): {s}\n", .{ cfg_path, @errorName(e) });
        try std.testing.expect(false);
    };
}

test "config init: idempotent — second call on existing file exits 0" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const cfg_path = configPathInTmp(&suite);
    defer gpa.free(cfg_path);

    const extra: []const harness.Suite.ExtraEnvEntry = &.{
        .{ .key = "PLANAR_CONFIG_PATH", .value = cfg_path },
    };

    // First init.
    const s1 = suite.mustRunWith(&.{ "config", "init" }, extra);
    gpa.free(s1);

    // Second init — must exit 0 (already exists message).
    const s2 = suite.mustRunWith(&.{ "config", "init" }, extra);
    defer gpa.free(s2);

    try std.testing.expect(std.mem.indexOf(u8, s2, "already exists") != null);
}

test "config show --defaults: exits 0, output contains 'vendor' and 'claude'" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const cfg_path = configPathInTmp(&suite);
    defer gpa.free(cfg_path);

    const extra: []const harness.Suite.ExtraEnvEntry = &.{
        .{ .key = "PLANAR_CONFIG_PATH", .value = cfg_path },
    };

    const stdout = suite.mustRunWith(&.{ "config", "show", "--defaults" }, extra);
    defer gpa.free(stdout);

    try std.testing.expect(std.mem.indexOf(u8, stdout, "vendor") != null);
    try std.testing.expect(std.mem.indexOf(u8, stdout, "claude") != null);
}

test "config validate: exits 0 on a freshly init'd config file" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const cfg_path = configPathInTmp(&suite);
    defer gpa.free(cfg_path);

    const extra: []const harness.Suite.ExtraEnvEntry = &.{
        .{ .key = "PLANAR_CONFIG_PATH", .value = cfg_path },
    };

    // Init first.
    const si = suite.mustRunWith(&.{ "config", "init" }, extra);
    gpa.free(si);

    // Validate — should exit 0 and print "ok".
    const stdout = suite.mustRunWith(&.{ "config", "validate" }, extra);
    defer gpa.free(stdout);

    try std.testing.expect(std.mem.indexOf(u8, stdout, "ok") != null);
}

test "config validate: exits non-zero on malformed TOML with 'line' in stderr" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const cfg_path = configPathInTmp(&suite);
    defer gpa.free(cfg_path);

    // Write malformed TOML directly into the temp dir.
    // The dir already exists (created for planar.db).
    const bad_toml =
        \\# bad config — unclosed table header triggers a parse error
        \\[defaults]
        \\vendor = "claude"
        \\[unclosed
        \\
    ;
    std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = cfg_path,
        .data = bad_toml,
    }) catch |e| {
        std.debug.panic("failed to write malformed config to '{s}': {s}", .{ cfg_path, @errorName(e) });
    };

    const extra: []const harness.Suite.ExtraEnvEntry = &.{
        .{ .key = "PLANAR_CONFIG_PATH", .value = cfg_path },
    };

    const stderr = suite.expectFailureWith(&.{ "config", "validate" }, extra);
    defer gpa.free(stderr);

    // Stderr must contain "line" (citing the location of the parse error).
    try std.testing.expect(std.mem.indexOf(u8, stderr, "line") != null);
}

test "config validate: exits non-zero when a sensitive key carries a literal" {
    // Mirrors Go's checkSensitiveValues — token/password/secret/*_token/*_password/*_secret/*_key
    // keys may NOT carry a literal value in the config file; the convention is to
    // name an env var via the `*_env` companion key instead.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const cfg_path = configPathInTmp(&suite);
    defer gpa.free(cfg_path);

    const bad_toml =
        \\[external.jira]
        \\base_url = "https://example.atlassian.net"
        \\api_token = "literal-secret-value-must-not-appear-here"
        \\
    ;
    std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = cfg_path,
        .data = bad_toml,
    }) catch |e| {
        std.debug.panic("failed to write sensitive-bearing config to '{s}': {s}", .{ cfg_path, @errorName(e) });
    };

    const extra: []const harness.Suite.ExtraEnvEntry = &.{
        .{ .key = "PLANAR_CONFIG_PATH", .value = cfg_path },
    };

    const stderr = suite.expectFailureWith(&.{ "config", "validate" }, extra);
    defer gpa.free(stderr);

    // Stderr cites the offending key + the line. "api_token" appears in the
    // stderr message; "line 3" identifies the location.
    try std.testing.expect(std.mem.indexOf(u8, stderr, "api_token") != null);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "line") != null);
}

test "config show --effective: includes plan-540 model tier maps + role tiers (defaults)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Non-existent config path → pure embedded defaults. The effective view
    // renders dotted keys (models.<vendor>.<tier>, roles.<role>); --defaults
    // would instead dump defaults.toml verbatim ([models.claude] form).
    const cfg_path = configPathInTmp(&suite);
    defer gpa.free(cfg_path);
    const extra: []const harness.Suite.ExtraEnvEntry = &.{
        .{ .key = "PLANAR_CONFIG_PATH", .value = cfg_path },
    };

    const stdout = suite.mustRunWith(&.{ "config", "show", "--effective" }, extra);
    defer gpa.free(stdout);

    for ([_][]const u8{
        "models.claude.medium", "claude-sonnet-5",
        "models.codex.large",   "gpt-5.6-sol",
        "roles.coder",          "roles.reviewer",
    }) |needle| {
        if (std.mem.indexOf(u8, stdout, needle) == null) {
            std.debug.print("\nconfig show --effective missing '{s}'\n{s}\n", .{ needle, stdout });
            return error.TestUnexpectedResult;
        }
    }
}

test "config show --effective: a config-file [models] override wins with config-file provenance" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const cfg_path = configPathInTmp(&suite);
    defer gpa.free(cfg_path);

    // Write a config file that re-routes codex medium.
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = cfg_path,
        .data =
        \\[models.codex]
        \\medium = "gpt-5.5"
        ,
    });

    const extra: []const harness.Suite.ExtraEnvEntry = &.{
        .{ .key = "PLANAR_CONFIG_PATH", .value = cfg_path },
    };
    const stdout = suite.mustRunWith(&.{ "config", "show", "--effective" }, extra);
    defer gpa.free(stdout);

    // The overridden line should carry the new value and a config-file source.
    var found = false;
    var lines = std.mem.splitScalar(u8, stdout, '\n');
    while (lines.next()) |line| {
        if (std.mem.indexOf(u8, line, "models.codex.medium") == null) continue;
        if (std.mem.indexOf(u8, line, "gpt-5.5") == null) {
            std.debug.print("\nmodels.codex.medium not overridden: {s}\n", .{line});
            return error.TestUnexpectedResult;
        }
        // provenance label is bracketed, e.g. "[config file]".
        if (std.mem.indexOf(u8, line, "config file") == null) {
            std.debug.print("\nmodels.codex.medium override missing config-file provenance: {s}\n", .{line});
            return error.TestUnexpectedResult;
        }
        found = true;
    }
    try std.testing.expect(found);
}

test "config show --effective: an array-shaped [models] tier surfaces the candidate list (plan 899 D3/D5)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const cfg_path = configPathInTmp(&suite);
    defer gpa.free(cfg_path);

    // Route codex.large to an ordered candidate list; claude.small stays
    // scalar in the embedded defaults (the untouched-edge case).
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = cfg_path,
        .data =
        \\[models.codex]
        \\large = ["gpt-5.5", "gpt-5.3-codex-spark"]
        ,
    });

    const extra: []const harness.Suite.ExtraEnvEntry = &.{
        .{ .key = "PLANAR_CONFIG_PATH", .value = cfg_path },
    };

    // --- human mode ---
    const stdout = suite.mustRunWith(&.{ "config", "show", "--effective" }, extra);
    defer gpa.free(stdout);

    var found_candidates_line = false;
    var found_scalar_line = false;
    var lines = std.mem.splitScalar(u8, stdout, '\n');
    while (lines.next()) |line| {
        if (std.mem.indexOf(u8, line, "models.codex.large") != null) {
            if (std.mem.indexOf(u8, line, "(candidates: gpt-5.5, gpt-5.3-codex-spark)") == null) {
                std.debug.print("\nmodels.codex.large missing candidates suffix: {s}\n", .{line});
                return error.TestUnexpectedResult;
            }
            found_candidates_line = true;
        }
        if (std.mem.indexOf(u8, line, "models.claude.small") != null) {
            // Scalar/one-element edge: no "(candidates: " suffix at all.
            if (std.mem.indexOf(u8, line, "(candidates:") != null) {
                std.debug.print("\nmodels.claude.small (scalar) unexpectedly carries a candidates suffix: {s}\n", .{line});
                return error.TestUnexpectedResult;
            }
            found_scalar_line = true;
        }
    }
    try std.testing.expect(found_candidates_line);
    try std.testing.expect(found_scalar_line);

    // --- JSON mode ---
    const stdout_json = suite.mustRunWith(&.{ "config", "show", "--effective", "--json" }, extra);
    defer gpa.free(stdout_json);

    var found_candidates_json = false;
    var found_scalar_json = false;
    var json_lines = std.mem.splitScalar(u8, stdout_json, '\n');
    while (json_lines.next()) |line| {
        if (std.mem.indexOf(u8, line, "\"key\":\"models.codex.large\"") != null) {
            if (std.mem.indexOf(u8, line, "\"value\":\"gpt-5.5\"") == null) {
                std.debug.print("\nmodels.codex.large JSON missing list[0] value: {s}\n", .{line});
                return error.TestUnexpectedResult;
            }
            if (std.mem.indexOf(u8, line, "\"candidates\":[\"gpt-5.5\",\"gpt-5.3-codex-spark\"]") == null) {
                std.debug.print("\nmodels.codex.large JSON missing candidates array: {s}\n", .{line});
                return error.TestUnexpectedResult;
            }
            found_candidates_json = true;
        }
        if (std.mem.indexOf(u8, line, "\"key\":\"models.claude.small\"") != null) {
            // Scalar/one-element edge: no "candidates" key at all.
            if (std.mem.indexOf(u8, line, "\"candidates\"") != null) {
                std.debug.print("\nmodels.claude.small (scalar) JSON unexpectedly carries a candidates key: {s}\n", .{line});
                return error.TestUnexpectedResult;
            }
            found_scalar_json = true;
        }
    }
    try std.testing.expect(found_candidates_json);
    try std.testing.expect(found_scalar_json);
}

// ---------------------------------------------------------------------------
// Work-type routing map (plan 899 D4/D7/D9/D10/D11, task worktype-routing-map)
// ---------------------------------------------------------------------------

test "config show --effective: a routing-map override for a work type wins with config-file provenance" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const cfg_path = configPathInTmp(&suite);
    defer gpa.free(cfg_path);

    // A schema-work-type task should route to the designated (non-default)
    // candidate, not list[0].
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = cfg_path,
        .data =
        \\[models.codex]
        \\large = ["gpt-5.5", "gpt-5.3-codex-spark"]
        \\[routing.codex.large]
        \\schema = "gpt-5.3-codex-spark"
        ,
    });

    const extra: []const harness.Suite.ExtraEnvEntry = &.{
        .{ .key = "PLANAR_CONFIG_PATH", .value = cfg_path },
    };

    const stdout_json = suite.mustRunWith(&.{ "config", "show", "--effective", "--json" }, extra);
    defer gpa.free(stdout_json);

    var found = false;
    var lines = std.mem.splitScalar(u8, stdout_json, '\n');
    while (lines.next()) |line| {
        if (std.mem.indexOf(u8, line, "\"key\":\"routing.codex.large.schema\"") == null) continue;
        if (std.mem.indexOf(u8, line, "\"value\":\"gpt-5.3-codex-spark\"") == null) {
            std.debug.print("\nrouting.codex.large.schema not routed to designated candidate: {s}\n", .{line});
            return error.TestUnexpectedResult;
        }
        if (std.mem.indexOf(u8, line, "\"provenance\":\"config file\"") == null) {
            std.debug.print("\nrouting.codex.large.schema missing config-file provenance: {s}\n", .{line});
            return error.TestUnexpectedResult;
        }
        found = true;
    }
    try std.testing.expect(found);

    // A work type absent from the routing map (e.g. "feature") is not
    // recorded in the effective map at all — it falls back to the tier
    // default via the resolver, not via a materialized routing entry.
    try std.testing.expect(std.mem.indexOf(u8, stdout_json, "\"key\":\"routing.codex.large.feature\"") == null);

    // The shipped embedded default routes "mechanical" for every vendor/tier
    // to that tier's default candidate.
    try std.testing.expect(std.mem.indexOf(u8, stdout_json, "\"key\":\"routing.claude.medium.mechanical\"") != null);
}

test "config validate: exits non-zero when a routing entry names a model absent from the tier's candidate list" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const cfg_path = configPathInTmp(&suite);
    defer gpa.free(cfg_path);

    // "c-model" is not a member of the large tier's candidate list.
    const bad_toml =
        \\[models.codex]
        \\large = ["a-model", "b-model"]
        \\[routing.codex.large]
        \\schema = "c-model"
        \\
    ;
    std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = cfg_path,
        .data = bad_toml,
    }) catch |e| {
        std.debug.panic("failed to write bad-routing config to '{s}': {s}", .{ cfg_path, @errorName(e) });
    };

    const extra: []const harness.Suite.ExtraEnvEntry = &.{
        .{ .key = "PLANAR_CONFIG_PATH", .value = cfg_path },
    };

    const stderr = suite.expectFailureWith(&.{ "config", "validate" }, extra);
    defer gpa.free(stderr);

    // The diagnostic names the offending routing entry and the bad model id.
    try std.testing.expect(std.mem.indexOf(u8, stderr, "routing.codex.large.schema") != null);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "c-model") != null);
}

test "config validate: exits 0 when every routing entry names a model present in its tier's candidate list" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const cfg_path = configPathInTmp(&suite);
    defer gpa.free(cfg_path);

    const good_toml =
        \\[models.codex]
        \\large = ["a-model", "b-model"]
        \\[routing.codex.large]
        \\schema = "b-model"
        \\mechanical = "a-model"
        \\
    ;
    std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = cfg_path,
        .data = good_toml,
    }) catch |e| {
        std.debug.panic("failed to write good-routing config to '{s}': {s}", .{ cfg_path, @errorName(e) });
    };

    const extra: []const harness.Suite.ExtraEnvEntry = &.{
        .{ .key = "PLANAR_CONFIG_PATH", .value = cfg_path },
    };

    const stdout = suite.mustRunWith(&.{ "config", "validate" }, extra);
    defer gpa.free(stdout);

    try std.testing.expect(std.mem.indexOf(u8, stdout, "ok") != null);
}
