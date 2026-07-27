//! Black-box contracts for the opaque model candidate registry.

const std = @import("std");
const harness = @import("harness");

fn contains(haystack: []const u8, needle: []const u8) !void {
    try std.testing.expect(std.mem.indexOf(u8, haystack, needle) != null);
}

fn addAndBind(suite: *harness.Suite) void {
    const added = suite.mustRun(&.{
        "models", "registry",    "add",     "--vendor", "vendor-x",
        "--id",   "candidate-a", "--order", "0",
    });
    suite.allocator.free(added);
    const bound = suite.mustRun(&.{
        "models", "registry", "bind",   "--candidate", "1",
        "--role", "coder",    "--tier", "medium",
    });
    suite.allocator.free(bound);
}

fn observe(
    suite: *harness.Suite,
    host: []const u8,
    version: []const u8,
    availability: []const u8,
    verification: []const u8,
    expires_at: []const u8,
) void {
    const out = suite.mustRun(&.{
        "models",        "registry",             "observe",      "--candidate",    "1",
        "--host",        host,                   "--version",    version,          "--availability",
        availability,    "--spawn-verification", verification,   "--evidence-ref", "probe",
        "--captured-at", "2026-01-01T00:00:00Z", "--expires-at", expires_at,
    });
    suite.allocator.free(out);
}

test "models registry eligibility is host scoped and reports disappearance and expiry" {
    var suite = harness.Suite.init(std.testing.allocator);
    defer suite.deinit();
    addAndBind(&suite);
    observe(&suite, "host-a", "100", "unavailable", "failed", "2027-01-01T00:00:00Z");
    observe(&suite, "host-b", "1", "available", "verified", "2027-01-01T00:00:00Z");

    const host_b = suite.mustRun(&.{
        "models", "registry", "eligibility",          "--candidate",          "1",
        "--host", "host-b",   "--role",               "coder",                "--tier",
        "medium", "--now",    "2026-06-01T00:00:00Z", "--override-supported", "--policy-permits",
    });
    defer suite.allocator.free(host_b);
    try contains(host_b, "\"host\":\"host-b\"");
    try contains(host_b, "\"eligible\":true");

    const host_a = suite.mustRun(&.{
        "models", "registry", "eligibility",          "--candidate",          "1",
        "--host", "host-a",   "--role",               "coder",                "--tier",
        "medium", "--now",    "2026-06-01T00:00:00Z", "--override-supported", "--policy-permits",
    });
    defer suite.allocator.free(host_a);
    try contains(host_a, "provider_cli_unavailable");
    try contains(host_a, "exact_spawn_unverified");

    const expired = suite.mustRun(&.{
        "models", "registry", "eligibility",          "--candidate",          "1",
        "--host", "host-b",   "--role",               "coder",                "--tier",
        "medium", "--now",    "2028-01-01T00:00:00Z", "--override-supported", "--policy-permits",
    });
    defer suite.allocator.free(expired);
    try contains(expired, "host_observation_expired");
}

test "models registry preserves opaque values, rejects controls, and names identity mismatch" {
    var suite = harness.Suite.init(std.testing.allocator);
    defer suite.deinit();

    const safe = suite.mustRun(&.{
        "models", "registry",            "add",     "--vendor", "vendor-x",
        "--id",   "--opaque value; $()", "--order", "0",
    });
    suite.allocator.free(safe);
    const exported = suite.mustRun(&.{ "models", "registry", "export", "--json" });
    defer suite.allocator.free(exported);
    try contains(exported, "--opaque value; $()");

    const unsafe = suite.exec(&.{
        "models", "registry",      "add",     "--vendor", "vendor-x",
        "--id",   "unsafe\nvalue", "--order", "1",
    });
    defer unsafe.deinit(suite.allocator);
    try std.testing.expect(unsafe.term == .exited and unsafe.term.exited != 0);

    const mismatch = suite.mustRun(&.{
        "models",          "registry", "verify-identity", "--candidate", "1",
        "--actual-vendor", "vendor-x", "--actual-id",     "other",
    });
    defer suite.allocator.free(mismatch);
    try contains(mismatch, "\"identity\":\"candidate_mismatch\"");
}

test "models registry legacy import preserves IDs and exports migration warning" {
    var suite = harness.Suite.init(std.testing.allocator);
    defer suite.deinit();
    const config =
        \\[models.codex]
        \\small = ["operator exact id"]
        \\medium = "operator-medium"
        \\large = "operator-large"
        \\[roles]
        \\coder = "small"
    ;
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = suite.config_path, .data = config });
    const imported = suite.mustRun(&.{ "models", "registry", "import-legacy" });
    defer suite.allocator.free(imported);
    try contains(imported, "imported");

    const exported = suite.mustRun(&.{ "models", "registry", "export", "--json" });
    defer suite.allocator.free(exported);
    try contains(exported, "operator exact id");
    try contains(exported, "\"compatibility_source\":\"legacy_config\"");
    try contains(exported, "migration_warning");
}

test "models registry refuses deletion when immutable host evidence exists" {
    var suite = harness.Suite.init(std.testing.allocator);
    defer suite.deinit();
    addAndBind(&suite);
    observe(&suite, "host-a", "1", "available", "verified", "2027-01-01T00:00:00Z");
    const result = suite.exec(&.{ "models", "registry", "remove", "--candidate", "1" });
    defer result.deinit(suite.allocator);
    try std.testing.expect(result.term == .exited and result.term.exited != 0);
}
