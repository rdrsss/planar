//! Black-box contract for `planar-agent dispatch preview|confirm` — the
//! two-step authorization an orchestrator uses before spawning a host model.
//!
//! The engine-level rules are unit-tested in `engine/routing/dispatch.zig`.
//! This suite pins what a harness actually sees: the JSON shapes, the
//! single-use guarantee ACROSS SEPARATE PROCESSES (which is the case that
//! matters — a token is spent by whichever process gets there first), and the
//! promise that an opaque model identifier is never reparsed.

const std = @import("std");
const harness = @import("harness");

fn contains(haystack: []const u8, needle: []const u8) !void {
    try std.testing.expect(std.mem.indexOf(u8, haystack, needle) != null);
}

fn extractString(json: []const u8, key: []const u8) ?[]const u8 {
    const idx = std.mem.indexOf(u8, json, key) orelse return null;
    const start = idx + key.len;
    const end = std.mem.indexOfScalarPos(u8, json, start, '"') orelse return null;
    return json[start..end];
}

fn resolveAgentBin() []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_AGENT_BIN=")) {
            return s["PLANAR_AGENT_BIN=".len..];
        }
    }
    @panic(
        \\PLANAR_AGENT_BIN is not set.
        \\Run integration tests via: make test-integration (which sets it).
    );
}

fn runAgent(
    suite: *const harness.Suite,
    args: []const []const u8,
) harness.Suite.RunResult {
    const gpa = suite.allocator;

    var argv_list: std.ArrayList([]const u8) = .empty;
    defer argv_list.deinit(gpa);
    argv_list.append(gpa, resolveAgentBin()) catch @panic("OOM");
    for (args) |a| argv_list.append(gpa, a) catch @panic("OOM");

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = environ.createMap(gpa) catch @panic("OOM creating env map");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.db_path) catch @panic("OOM injecting PLANAR_DB");

    const result = std.process.run(gpa, std.testing.io, .{
        .argv = argv_list.items,
        .environ_map = &env_map,
    }) catch |e| std.debug.panic("runAgent spawn failed: {s}", .{@errorName(e)});

    return .{ .stdout = result.stdout, .stderr = result.stderr, .term = result.term };
}

fn mustRunAgent(suite: *const harness.Suite, args: []const []const u8) []u8 {
    const res = runAgent(suite, args);
    defer suite.allocator.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print(
            "planar-agent failed (term={any}):\n{s}\nstderr: {s}\n",
            .{ res.term, res.stdout, res.stderr },
        );
        @panic("planar-agent must-run failed");
    }
    return res.stdout;
}

/// Register a project plus one opaque candidate.
fn seed(suite: *harness.Suite, candidate_id: []const u8) void {
    const root = suite.registerProject("dispatch-cli");
    const added = suite.mustRunInDir(root, &.{
        "models", "registry", "add", "--vendor", "vendor-x", "--id", candidate_id, "--order", "0",
    });
    suite.allocator.free(added);
}

const preview_args = [_][]const u8{
    "dispatch",            "preview",
    "--work-item",         "lwi-1",
    "--project",           "1",
    "--validation-policy", "val-v1",
    "--routing-policy",    "route-v1",
    "--profile-rule",      "prof-v1",
    "--vendor",            "vendor-x",
    "--role",              "coder",
    "--tier",              "medium",
    "--work-type",         "feature",
    "--complexity",        "standard",
    "--packet-digest",     "pkt",
    "--profile-digest",    "prof",
    "--policy-digest",     "pol",
    "--capability-digest", "cap",
    "--candidate",         "1",
    "--host",              "host-a",
    "--class",             "default",
    "--evidence-state",    "observational",
    "--expires-at",        "2027-01-01T00:00:00Z",
    "--json",
};

fn confirmArgs(token: []const u8, key: []const u8, packet_digest: []const u8) [33][]const u8 {
    return .{
        "dispatch",            "confirm",
        "--token",             token,
        "--dispatch-key",      key,
        "--now",               "2026-06-01T00:00:00Z",
        "--packet-digest",     packet_digest,
        "--profile-digest",    "prof",
        "--policy-digest",     "pol",
        "--capability-digest", "cap",
        "--candidate",         "1",
        "--vendor",            "vendor-x",
        "--role",              "coder",
        "--tier",              "medium",
        "--work-type",         "feature",
        "--complexity",        "standard",
        "--validation-policy", "val-v1",
        "--routing-policy",    "route-v1",
        "--json",
    };
}

test "a preview token authorizes exactly one dispatch, across processes" {
    var suite = harness.Suite.init(std.testing.allocator);
    defer suite.deinit();
    seed(&suite, "candidate-a");

    const preview = mustRunAgent(&suite, &preview_args);
    defer suite.allocator.free(preview);
    try contains(preview, "\"ok\":true");
    try contains(preview, "\"preview_token\":\"");
    try contains(preview, "routing-dispatch-v1");

    const token = extractString(preview, "\"preview_token\":\"") orelse return error.NoToken;

    const ok_args = confirmArgs(token, "dk-1", "pkt");
    const confirmed = mustRunAgent(&suite, &ok_args);
    defer suite.allocator.free(confirmed);
    try contains(confirmed, "\"ok\":true");
    try contains(confirmed, "\"dispatch_id\":");

    // Spending the same authorization twice would be one operator decision
    // producing two spawns. A separate process must not be able to do it.
    const reuse_args = confirmArgs(token, "dk-2", "pkt");
    const reuse = runAgent(&suite, &reuse_args);
    defer reuse.deinit(suite.allocator);
    try std.testing.expect(reuse.term.exited != 0);
    try contains(reuse.stdout, "already_consumed");
}

test "a confirm whose bound state moved is refused and names what moved" {
    var suite = harness.Suite.init(std.testing.allocator);
    defer suite.deinit();
    seed(&suite, "candidate-a");

    const preview = mustRunAgent(&suite, &preview_args);
    defer suite.allocator.free(preview);
    const token = extractString(preview, "\"preview_token\":\"") orelse return error.NoToken;

    // The task was edited after the operator looked: the packet digest moved.
    const drift = confirmArgs(token, "dk-1", "MOVED");
    const res = runAgent(&suite, &drift);
    defer res.deinit(suite.allocator);
    try std.testing.expect(res.term.exited != 0);
    try contains(res.stdout, "stale_preview");
    try contains(res.stdout, "packet_changed");

    // Refusal is all-or-nothing: the token must still be spendable, otherwise
    // a transient drift would strand the operator with a dead authorization.
    const retry = confirmArgs(token, "dk-1", "pkt");
    const ok = mustRunAgent(&suite, &retry);
    defer suite.allocator.free(ok);
    try contains(ok, "\"ok\":true");
}

test "an opaque candidate id is carried as data, never reparsed" {
    var suite = harness.Suite.init(std.testing.allocator);
    defer suite.deinit();
    // If any layer shelled out, this identifier would execute or truncate.
    seed(&suite, "--opaque; $(whoami) && rm -rf /");

    const preview = mustRunAgent(&suite, &preview_args);
    defer suite.allocator.free(preview);
    try contains(preview, "\"ok\":true");

    const token = extractString(preview, "\"preview_token\":\"") orelse return error.NoToken;
    const args = confirmArgs(token, "dk-1", "pkt");
    const confirmed = mustRunAgent(&suite, &args);
    defer suite.allocator.free(confirmed);
    try contains(confirmed, "\"ok\":true");
}

test "an unknown token is refused rather than treated as a fresh dispatch" {
    var suite = harness.Suite.init(std.testing.allocator);
    defer suite.deinit();
    seed(&suite, "candidate-a");

    const args = confirmArgs("0123456789abcdef0123456789abcdef", "dk-1", "pkt");
    const res = runAgent(&suite, &args);
    defer res.deinit(suite.allocator);
    try std.testing.expect(res.term.exited != 0);
}

// REGRESSION (planar task 6092). `--complexity high-risk` -- one of the three
// values the verb's own --help documents -- failed with a bare
// `QueryFailed`, because the insert bound the Zig enum tag `high_risk` and
// every complexity CHECK constraint lists the hyphenated `high-risk`.
//
// The bug shipped because the fixture above exercises only `standard`, whose
// enum tag and wire spelling happen to be identical. So this test walks ALL
// THREE documented values through the real binary and the real INSERT. The
// unit test in engine/routing/store.zig pins the enum/schema agreement; only
// this one covers the actual bind site in engine/routing/dispatch.zig.
test "every documented complexity value is accepted end to end" {
    var suite = harness.Suite.init(std.testing.allocator);
    defer suite.deinit();
    seed(&suite, "candidate-complexity");

    for ([_][]const u8{ "bounded", "standard", "high-risk" }) |complexity| {
        var args = preview_args;
        // Overwrite the --complexity value in place; assert we patched the
        // slot we think we did, so a reordering of preview_args cannot make
        // this test silently exercise `standard` three times.
        const slot = for (args, 0..) |a, i| {
            if (std.mem.eql(u8, a, "--complexity")) break i + 1;
        } else @panic("preview_args no longer contains --complexity");
        args[slot] = complexity;

        const out = mustRunAgent(&suite, &args);
        defer suite.allocator.free(out);
        try contains(out, "\"ok\":true");
        try contains(out, "\"preview_token\":\"");
    }

    // The enum-tag spelling must NOT be quietly accepted as a second alias,
    // and the diagnostic must name the flag and the offending value rather
    // than surfacing as a generic query failure.
    var bad = preview_args;
    const slot = for (bad, 0..) |a, i| {
        if (std.mem.eql(u8, a, "--complexity")) break i + 1;
    } else @panic("preview_args no longer contains --complexity");
    bad[slot] = "high_risk";

    const res = runAgent(&suite, &bad);
    defer suite.allocator.free(res.stdout);
    defer suite.allocator.free(res.stderr);
    try std.testing.expect(res.term == .exited and res.term.exited != 0);
    try contains(res.stderr, "--complexity");
    try contains(res.stderr, "high_risk");
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "QueryFailed") == null);
}
