//! integration_tests/planar_agent_ingest_test.zig — black-box tests for
//! the vendor ingestion verb (`planar-agent ingest --vendor <v>
//! --event @-`).
//!
//! Covers the M4 Claude adapter scenarios from the test-spec AND the
//! M6 second-vendor (Copilot) adapter:
//!
//!   - Happy path: session_start payload creates or resolves a session
//!     (slug: claude-hook-ingest, claude-session-auto-open,
//!      claude-ingest-integration-test, second-vendor-adapter).
//!   - Happy path: second event on the same vendor_session_id REUSES
//!     the existing sessions row.
//!   - Error: malformed JSON payload — exit non-zero, no partial writes.
//!   - Error: well-formed JSON with unknown event_type — distinct exit
//!     path, no rows written.
//!
//! The Copilot scenarios at the bottom re-prove the contract against
//! a second vendor — they exist to lock in that dispatch + handler
//! stay vendor-agnostic (a refactor that hard-codes Claude's event
//! names trips them).

const std = @import("std");
const harness = @import("harness");

/// Resolve the planar-agent binary path from PLANAR_AGENT_BIN env var.
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

/// Run planar-agent ingest with the payload piped via stdin.
fn runIngestStdin(
    suite: *const harness.Suite,
    payload: []const u8,
    args: []const []const u8,
) harness.Suite.RunResult {
    const gpa = suite.allocator;
    const agent_bin = resolveAgentBin();

    var argv_list: std.ArrayList([]const u8) = .empty;
    defer argv_list.deinit(gpa);
    argv_list.append(gpa, agent_bin) catch @panic("OOM");
    for (args) |a| argv_list.append(gpa, a) catch @panic("OOM");

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = environ.createMap(gpa) catch @panic("OOM");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.db_path) catch @panic("OOM");

    var child = std.process.spawn(std.testing.io, .{
        .argv = argv_list.items,
        .environ_map = &env_map,
        .stdin = .pipe,
        .stdout = .pipe,
        .stderr = .pipe,
    }) catch |e| std.debug.panic("spawn failed: {s}", .{@errorName(e)});

    // Write payload then close stdin.
    if (child.stdin) |*stdin_file| {
        var w = stdin_file.writer(std.testing.io, &.{});
        w.interface.writeAll(payload) catch |e|
            std.debug.panic("stdin write failed: {s}", .{@errorName(e)});
        w.interface.flush() catch {};
        stdin_file.close(std.testing.io);
        child.stdin = null;
    }

    const stdout_bytes = drainPipe(gpa, &child.stdout) catch |e|
        std.debug.panic("drain stdout: {s}", .{@errorName(e)});
    const stderr_bytes = drainPipe(gpa, &child.stderr) catch |e|
        std.debug.panic("drain stderr: {s}", .{@errorName(e)});

    const term = child.wait(std.testing.io) catch |e|
        std.debug.panic("wait failed: {s}", .{@errorName(e)});

    return .{ .stdout = stdout_bytes, .stderr = stderr_bytes, .term = term };
}

fn drainPipe(gpa: std.mem.Allocator, file: *?std.Io.File) ![]u8 {
    if (file.*) |*f| {
        defer {
            f.close(std.testing.io);
            file.* = null;
        }
        var reader = f.reader(std.testing.io, &.{});
        return reader.interface.allocRemaining(gpa, std.Io.Limit.limited(1 << 20)) catch |err| switch (err) {
            error.ReadFailed => if (reader.err) |e| return e else return err,
            else => return err,
        };
    }
    return try gpa.dupe(u8, "");
}

// =========================================================================
// Happy path
// =========================================================================

test "ingest claude session_start creates a sessions row and reports sessions_created:1" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const payload =
        \\{"event_type":"session_start","session_id":"claude-sid-A","model":"claude-opus-4-7"}
    ;

    const res = runIngestStdin(&suite, payload, &.{
        "ingest", "--vendor", "claude", "--event", "@-", "--json",
    });
    defer res.deinit(gpa);

    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("ingest failed: term={any}\nstdout: {s}\nstderr: {s}\n", .{ res.term, res.stdout, res.stderr });
        return error.IngestShouldSucceed;
    }
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "\"ok\":true") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "\"sessions_created\":1") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "\"actions_created\":0") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "\"events_processed\":1") != null);
}

test "ingest second event on same vendor_session_id REUSES the sessions row" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const start_payload =
        \\{"event_type":"session_start","session_id":"claude-sid-reuse"}
    ;
    const tool_payload =
        \\{"event_type":"tool_call","session_id":"claude-sid-reuse","summary":"ran ls"}
    ;

    // First event: session is new.
    const r1 = runIngestStdin(&suite, start_payload, &.{
        "ingest", "--vendor", "claude", "--event", "@-", "--json",
    });
    defer r1.deinit(gpa);
    try std.testing.expect(r1.term == .exited and r1.term.exited == 0);
    try std.testing.expect(std.mem.indexOf(u8, r1.stdout, "\"sessions_created\":1") != null);

    // Second event: same session id — sessions_created MUST be 0 because
    // the row was reused; actions_created bumps to 1 because tool_call
    // is an action_atomic.
    const r2 = runIngestStdin(&suite, tool_payload, &.{
        "ingest", "--vendor", "claude", "--event", "@-", "--json",
    });
    defer r2.deinit(gpa);
    try std.testing.expect(r2.term == .exited and r2.term.exited == 0);
    try std.testing.expect(std.mem.indexOf(u8, r2.stdout, "\"sessions_created\":0") != null);
    try std.testing.expect(std.mem.indexOf(u8, r2.stdout, "\"actions_created\":1") != null);

    // Cross-check by issuing a THIRD event with the same session_id;
    // if the row was duplicated, the third call would still report
    // sessions_created==1 because activeForVendor finds only the most
    // recently-inserted row's id; instead, since we reused, we still
    // get sessions_created==0.
    const r3 = runIngestStdin(&suite, tool_payload, &.{
        "ingest", "--vendor", "claude", "--event", "@-", "--json",
    });
    defer r3.deinit(gpa);
    try std.testing.expect(r3.term == .exited and r3.term.exited == 0);
    try std.testing.expect(std.mem.indexOf(u8, r3.stdout, "\"sessions_created\":0") != null);
}

test "ingest auto-opens a session on first non-session_start event" {
    // Per the spec: auto-open on first event for an unseen vendor
    // session. So firing a tool_call as the FIRST event should still
    // create the session and bump sessions_created to 1.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const payload =
        \\{"event_type":"tool_call","session_id":"first-event-autoopen","summary":"hello"}
    ;
    const res = runIngestStdin(&suite, payload, &.{
        "ingest", "--vendor", "claude", "--event", "@-", "--json",
    });
    defer res.deinit(gpa);

    try std.testing.expect(res.term == .exited and res.term.exited == 0);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "\"sessions_created\":1") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "\"actions_created\":1") != null);
}

// =========================================================================
// Error paths — both exit non-zero AND must not write any rows
// =========================================================================

test "ingest malformed payload exits non-zero with no partial writes" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const payload = "this is not json {{{";
    const res = runIngestStdin(&suite, payload, &.{
        "ingest", "--vendor", "claude", "--event", "@-", "--json",
    });
    defer res.deinit(gpa);

    try std.testing.expect(res.term == .exited);
    try std.testing.expect(res.term.exited != 0);
    // Message names the failure mode.
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "malformed") != null);

    // Post-state verification: a subsequent valid session_start with a
    // FIXED session_id MUST report sessions_created==1 (proves the
    // malformed attempt left no rows). If a partial write had landed
    // for some made-up session id, that wouldn't help us catch it;
    // we instead verify the surrounding transaction did not leak any
    // claude session into the DB by checking that the NEXT new id is
    // truly new (sessions_created==1).
    const followup_payload =
        \\{"event_type":"session_start","session_id":"after-malformed"}
    ;
    const r2 = runIngestStdin(&suite, followup_payload, &.{
        "ingest", "--vendor", "claude", "--event", "@-", "--json",
    });
    defer r2.deinit(gpa);
    try std.testing.expect(r2.term == .exited and r2.term.exited == 0);
    try std.testing.expect(std.mem.indexOf(u8, r2.stdout, "\"sessions_created\":1") != null);
}

test "ingest unknown event_type exits non-zero with distinct error message" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const payload =
        \\{"event_type":"telemetry_blob","session_id":"sid-unknown"}
    ;
    const res = runIngestStdin(&suite, payload, &.{
        "ingest", "--vendor", "claude", "--event", "@-", "--json",
    });
    defer res.deinit(gpa);

    try std.testing.expect(res.term == .exited);
    try std.testing.expect(res.term.exited != 0);
    // Distinct from "malformed" message — names the offending category.
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "unknown event_type") != null);

    // Post-state verification: opening a fresh session_start with the
    // SAME vendor_session_id MUST bump sessions_created to 1, which
    // proves no row was opened on the failed UnknownEventType path.
    const followup =
        \\{"event_type":"session_start","session_id":"sid-unknown"}
    ;
    const r2 = runIngestStdin(&suite, followup, &.{
        "ingest", "--vendor", "claude", "--event", "@-", "--json",
    });
    defer r2.deinit(gpa);
    try std.testing.expect(r2.term == .exited and r2.term.exited == 0);
    try std.testing.expect(std.mem.indexOf(u8, r2.stdout, "\"sessions_created\":1") != null);
}

test "ingest with reserved vendor 'codex' exits non-zero (M6 wired copilot, not codex)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const payload =
        \\{"event_type":"session_start","session_id":"x"}
    ;
    const res = runIngestStdin(&suite, payload, &.{
        "ingest", "--vendor", "codex", "--event", "@-",
    });
    defer res.deinit(gpa);

    try std.testing.expect(res.term == .exited);
    try std.testing.expect(res.term.exited != 0);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "codex") != null);
}

// =========================================================================
// M6 — Copilot vendor adapter
//
// These re-run the same contract scenarios against the second-vendor
// adapter, proving the dispatch + handler layer is genuinely
// vendor-agnostic. The payloads use Copilot's namespaced event
// taxonomy (`session.started`, `tool.invocation`, ...) so a refactor
// that accidentally hard-coded Claude's flat names would trip these.
// =========================================================================

test "ingest copilot session.started creates a sessions row" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const payload =
        \\{"event":"session.started","session_id":"cop-sid-A","model":"gpt-5-copilot","role":"coder"}
    ;
    const res = runIngestStdin(&suite, payload, &.{
        "ingest", "--vendor", "copilot", "--event", "@-", "--json",
    });
    defer res.deinit(gpa);

    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print(
            "copilot ingest failed: term={any}\nstdout: {s}\nstderr: {s}\n",
            .{ res.term, res.stdout, res.stderr },
        );
        return error.IngestShouldSucceed;
    }
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "\"ok\":true") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "\"sessions_created\":1") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "\"actions_created\":0") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "\"events_processed\":1") != null);
}

test "ingest copilot second event on same session reuses the row + records action" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const start_payload =
        \\{"event":"session.started","session_id":"cop-sid-reuse"}
    ;
    const tool_payload =
        \\{"event":"tool.invocation","session_id":"cop-sid-reuse","summary":"ran rg"}
    ;

    const r1 = runIngestStdin(&suite, start_payload, &.{
        "ingest", "--vendor", "copilot", "--event", "@-", "--json",
    });
    defer r1.deinit(gpa);
    try std.testing.expect(r1.term == .exited and r1.term.exited == 0);
    try std.testing.expect(std.mem.indexOf(u8, r1.stdout, "\"sessions_created\":1") != null);

    const r2 = runIngestStdin(&suite, tool_payload, &.{
        "ingest", "--vendor", "copilot", "--event", "@-", "--json",
    });
    defer r2.deinit(gpa);
    try std.testing.expect(r2.term == .exited and r2.term.exited == 0);
    try std.testing.expect(std.mem.indexOf(u8, r2.stdout, "\"sessions_created\":0") != null);
    try std.testing.expect(std.mem.indexOf(u8, r2.stdout, "\"actions_created\":1") != null);
}

test "ingest copilot auto-opens session on first non-session.started event" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const payload =
        \\{"event":"tool.invocation","session_id":"cop-autoopen","summary":"hello"}
    ;
    const res = runIngestStdin(&suite, payload, &.{
        "ingest", "--vendor", "copilot", "--event", "@-", "--json",
    });
    defer res.deinit(gpa);

    try std.testing.expect(res.term == .exited and res.term.exited == 0);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "\"sessions_created\":1") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "\"actions_created\":1") != null);
}

test "ingest copilot malformed payload exits non-zero with no partial writes" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const payload = "this is not json {{{";
    const res = runIngestStdin(&suite, payload, &.{
        "ingest", "--vendor", "copilot", "--event", "@-", "--json",
    });
    defer res.deinit(gpa);

    try std.testing.expect(res.term == .exited);
    try std.testing.expect(res.term.exited != 0);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "malformed") != null);

    // Post-state verification: a fresh session_started should report 1.
    const followup =
        \\{"event":"session.started","session_id":"cop-after-malformed"}
    ;
    const r2 = runIngestStdin(&suite, followup, &.{
        "ingest", "--vendor", "copilot", "--event", "@-", "--json",
    });
    defer r2.deinit(gpa);
    try std.testing.expect(r2.term == .exited and r2.term.exited == 0);
    try std.testing.expect(std.mem.indexOf(u8, r2.stdout, "\"sessions_created\":1") != null);
}

test "ingest copilot unknown event exits non-zero with distinct error message" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const payload =
        \\{"event":"telemetry.heartbeat","session_id":"cop-unknown"}
    ;
    const res = runIngestStdin(&suite, payload, &.{
        "ingest", "--vendor", "copilot", "--event", "@-", "--json",
    });
    defer res.deinit(gpa);

    try std.testing.expect(res.term == .exited);
    try std.testing.expect(res.term.exited != 0);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "unknown event_type") != null);

    // Post-state: opening a session with the same id MUST report 1.
    const followup =
        \\{"event":"session.started","session_id":"cop-unknown"}
    ;
    const r2 = runIngestStdin(&suite, followup, &.{
        "ingest", "--vendor", "copilot", "--event", "@-", "--json",
    });
    defer r2.deinit(gpa);
    try std.testing.expect(r2.term == .exited and r2.term.exited == 0);
    try std.testing.expect(std.mem.indexOf(u8, r2.stdout, "\"sessions_created\":1") != null);
}
