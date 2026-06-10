//! journal.zig — append-only run journal for `planar-execute`
//! (plan 492 M8 task 3196 m8-journal; tech-spec § Component model "Run journal").
//!
//! ## What this is
//!
//! The run journal is the DURABILITY SUBSTRATE for the M8 resume + budget work
//! (tasks 3197 skip-done-tasks, 3198 max-attempt/run-ceiling). It records ONE
//! append-only record per WORKER SPAWN: the prompt fingerprint, the worktree,
//! the cycle branch, the claim token, the model tier, the worker exit code, the
//! terminal verb/status the harness decided, and the spawn→terminal wall-clock.
//! A re-driven run reads these records back to reason about prior attempts (did
//! this task already complete? how many times has it failed?).
//!
//! ## File shape — append-only NDJSON
//!
//! The journal is an NDJSON file (one compact JSON object per line) at
//!   `<repo_root>/.worktrees/.planar-execute/journal-<plan_id>.ndjson`
//! mirroring the run-lock path derivation (`runlock.zig` writes
//! `run-<plan_id>.lock` in the SAME dir). Each `append` writes EXACTLY ONE line
//! (`<compact-json>\n`) at the END of the file — it never rewrites or truncates
//! existing content. A crashed run leaves a valid prefix of complete lines; the
//! next run appends after them. `read` parses the file line-by-line into records.
//!
//! ## Single-writer invariant — NO MUTEX
//!
//! Only the MAIN (cooperative-scheduler) thread ever writes the journal. The
//! `append` call lives at the tail of `agentContinue` (main.zig), which runs
//! inside the single-threaded Lua scheduler: exactly one coroutine is resumed at
//! a time, so two `agentContinue` invocations never overlap. The preemptive
//! heartbeat thread (task 3187) touches ONLY claim-token strings — it NEVER
//! reads or writes the journal. So there is no cross-thread access to the journal
//! file and no mutex is needed. This invariant is load-bearing: if a future
//! change moves the journal write off the scheduler thread (or has the heartbeat
//! thread touch it), it MUST add synchronization.
//!
//! ## Best-effort writes
//!
//! The journal is durability METADATA, not the control path. A write failure
//! (disk full, permission, etc.) MUST NOT crash the run or fail the `agent()`
//! call — the caller logs a note and continues. The terminal verb has already
//! been applied to the claim by the time the journal write happens, so a missing
//! journal line costs only resume/budget visibility, never correctness.

const std = @import("std");

const Io = std.Io;

const log = std.log.scoped(.planar_execute_journal);

/// Errors the journal surface can return. Callers treat these as best-effort:
/// a failure is logged and swallowed, never propagated as an `agent()` error.
pub const JournalError = error{
    /// A filesystem operation (create-dir, open, stat, write) failed.
    FsError,
    /// Allocation failed.
    OutOfMemory,
    /// Serializing the record to JSON failed.
    EncodeError,
    /// Parsing a journal line back into a record failed (read path).
    DecodeError,
};

/// One append-only journal record — written once per worker spawn when the
/// worker reaches terminal. Field set mirrors the roadmap acceptance criteria
/// ("prompt hash, worktree, branch, claim token, model, exit code, terminal
/// verb, wall-clock"), plus `task_slug` + `role` + `timestamp` for resume/budget
/// keying and operator triage.
///
/// All string fields are BORROWS at construction time in the production write
/// path (the caller stamps them from `AgentCallState` + the spawn outcome and
/// serializes immediately). On the `read` path the returned records own their
/// strings via the `std.json.Parsed` arena (see `read`).
pub const JournalRecord = struct {
    /// A content fingerprint of the brief/prompt the worker ran with, rendered
    /// as lowercase hex. NOT the full prompt — a 64-bit Wyhash digest so the
    /// journal stays small and a resume can match "same brief" cheaply.
    prompt_hash: []const u8,
    /// The cycle worktree path the worker ran in.
    worktree: []const u8,
    /// The cycle branch the worker committed onto (may be empty when the
    /// harness could not derive it — degraded paths).
    branch: []const u8,
    /// The claim token the worker held for this spawn.
    claim_token: []const u8,
    /// The model the worker was spawned with (resolved role → model, e.g.
    /// "claude-opus-4-8").
    model: []const u8,
    /// The vendor whose CLI spawned the worker ("claude"/"codex"). Provenance
    /// for the resolved routing (plan 540 task 3629). Defaulted for back-compat
    /// with journals written before the field existed.
    vendor: []const u8 = "claude",
    /// The role the worker ran as ("coder"/"reviewer"/"test-coder"/"documenter").
    role: []const u8,
    /// The task slug the spawn was dispatched against.
    task_slug: []const u8,
    /// The worker's literal exit code (0..255; 255 = abnormal/signal sentinel).
    exit_code: i32,
    /// The terminal status/verb the harness recorded for the spawn — the final
    /// status string ("completed"/"released"/"failed"/"respected"/"timed-out"/
    /// "blocked"). This is the worker's outcome as the harness saw it.
    terminal_verb: []const u8,
    /// Spawn→terminal elapsed wall-clock in milliseconds (real monotonic).
    wall_clock_ms: i64,
    /// A wall-clock timestamp (Unix seconds) when the record was written —
    /// useful for triage and ordering. Informational; resume keys on the
    /// record sequence, not this value.
    timestamp: i64,
};

/// journalPath derives the append-only journal file path for `plan_id` under
/// `repo_root`, mirroring the run-lock derivation
/// (`<repo_root>/.worktrees/.planar-execute/`). Returns a heap-owned absolute-
/// or-relative path the caller frees. When `repo_root` is empty the path is
/// rooted at "." (the degenerate case the caller already guards against by
/// skipping the write entirely).
pub fn journalPath(allocator: std.mem.Allocator, repo_root: []const u8, plan_id: u64) JournalError![]u8 {
    const root = if (repo_root.len != 0) repo_root else ".";
    const file_name = std.fmt.allocPrint(allocator, "journal-{d}.ndjson", .{plan_id}) catch
        return JournalError.OutOfMemory;
    defer allocator.free(file_name);
    return std.fs.path.join(allocator, &.{ root, ".worktrees", ".planar-execute", file_name }) catch
        return JournalError.OutOfMemory;
}

/// hashPrompt returns a stable lowercase-hex Wyhash digest of `prompt` — a
/// compact content fingerprint for the journal `prompt_hash` field. NOT a
/// cryptographic hash; a 64-bit digest is plenty to recognize "same brief" on a
/// resume. Returns a heap-owned 16-char hex string the caller frees.
pub fn hashPrompt(allocator: std.mem.Allocator, prompt: []const u8) JournalError![]u8 {
    const digest = std.hash.Wyhash.hash(0, prompt);
    return std.fmt.allocPrint(allocator, "{x:0>16}", .{digest}) catch
        return JournalError.OutOfMemory;
}

/// append writes EXACTLY ONE compact-JSON line (`<json>\n`) at the END of the
/// journal at `journal_path`, creating the file (and its `.planar-execute/`
/// parent dir) if absent. Append-only: existing content is never rewritten or
/// truncated — the new line is positioned at the current end-of-file.
///
/// Single-writer: see the module doc — only the scheduler thread calls this, so
/// there is no mutex. Best-effort at the call site: the caller logs and swallows
/// any `JournalError`.
pub fn append(
    allocator: std.mem.Allocator,
    io: Io,
    journal_path: []const u8,
    record: JournalRecord,
) JournalError!void {
    // Serialize the record to ONE compact JSON line first (no embedded newlines —
    // std.json escapes any in string fields, preserving the one-record-per-line
    // NDJSON contract).
    var buf: std.Io.Writer.Allocating = .init(allocator);
    defer buf.deinit();
    std.json.Stringify.value(record, .{}, &buf.writer) catch return JournalError.EncodeError;
    buf.writer.writeByte('\n') catch return JournalError.EncodeError;
    const line = buf.written();

    // Ensure the parent dir exists (mirror the run-lock / owner-marker dir
    // creation). `journal_path` lives under `.worktrees/.planar-execute/`.
    if (std.fs.path.dirname(journal_path)) |dir| {
        std.Io.Dir.cwd().createDirPath(io, dir) catch return JournalError.FsError;
    }

    // Open-or-create WITHOUT truncation, then position the write at the current
    // end of file. `truncate = false` keeps existing lines; `read = false` is a
    // write-only handle. A fresh file starts at size 0, so the first append
    // writes at offset 0 — identical to a create.
    const file = std.Io.Dir.cwd().createFile(io, journal_path, .{
        .truncate = false,
        .read = false,
    }) catch return JournalError.FsError;
    defer file.close(io);

    const end_pos: u64 = blk: {
        const st = file.stat(io) catch return JournalError.FsError;
        break :blk st.size;
    };

    // Positional write at end-of-file = append. `writePositionalAll` loops over
    // short writes internally until the whole line lands.
    file.writePositionalAll(io, line, end_pos) catch return JournalError.FsError;
}

/// read parses the append-only journal at `journal_path` into a slice of owned
/// records (NDJSON → records). Provided for the M8 resume (3197) + budget (3198)
/// consumers; this task does NOT consume it for control logic.
///
/// Each line is parsed with `.allocate = .alloc_always` (so every returned
/// string is an independent copy owned by the per-record arena), matching the
/// state-read helpers. The returned `Parsed` records are owned by `arena`; the
/// caller deinits the arena to free them all at once. A missing file yields an
/// empty slice (no journal yet ⇒ no prior attempts). Blank/whitespace-only lines
/// are skipped; a malformed line surfaces `DecodeError`.
pub fn read(
    allocator: std.mem.Allocator,
    arena: std.mem.Allocator,
    io: Io,
    journal_path: []const u8,
) JournalError![]JournalRecord {
    const data = std.Io.Dir.cwd().readFileAlloc(io, journal_path, allocator, .limited(1 << 24)) catch |err| switch (err) {
        error.FileNotFound => return &.{}, // no journal yet — no prior attempts.
        error.OutOfMemory => return JournalError.OutOfMemory,
        else => return JournalError.FsError,
    };
    defer allocator.free(data);

    var records: std.ArrayList(JournalRecord) = .empty;
    errdefer records.deinit(arena);

    var it = std.mem.splitScalar(u8, data, '\n');
    while (it.next()) |raw| {
        const line = std.mem.trim(u8, raw, " \t\r");
        if (line.len == 0) continue; // skip blank lines (trailing newline, gaps).
        const parsed = std.json.parseFromSliceLeaky(JournalRecord, arena, line, .{
            .ignore_unknown_fields = true,
            .allocate = .alloc_always,
        }) catch return JournalError.DecodeError;
        records.append(arena, parsed) catch return JournalError.OutOfMemory;
    }

    return records.toOwnedSlice(arena) catch return JournalError.OutOfMemory;
}

// ===========================================================================
// Tests — deterministic, write into a fresh system temp dir (NOT the real
// `.worktrees/` of this checkout).
// ===========================================================================

const testing = std.testing;

/// mkTmpDir creates a fresh system temp directory via `mktemp -d` and returns
/// its absolute path (heap-owned; caller `rmTree`s + frees). Mirrors
/// runlock.zig's helper: a clean absolute path NOT nested under this checkout's
/// own `.worktrees/` tree, so the journal write never pollutes the real tree.
fn mkTmpDir(allocator: std.mem.Allocator) []const u8 {
    const r = std.process.run(allocator, std.testing.io, .{
        .argv = &.{ "mktemp", "-d", "-t", "planar-journal.XXXXXX" },
    }) catch @panic("mkTmpDir: mktemp spawn failed");
    defer allocator.free(r.stderr);
    if (!(r.term == .exited and r.term.exited == 0)) {
        allocator.free(r.stdout);
        @panic("mkTmpDir: mktemp non-zero exit");
    }
    const trimmed = std.mem.trim(u8, r.stdout, " \t\r\n");
    const owned = allocator.dupe(u8, trimmed) catch @panic("OOM");
    allocator.free(r.stdout);
    return owned;
}

/// rmTree removes `path` and its contents via `rm -rf` (best-effort cleanup).
fn rmTree(allocator: std.mem.Allocator, path: []const u8) void {
    const r = std.process.run(allocator, std.testing.io, .{ .argv = &.{ "rm", "-rf", path } }) catch return;
    allocator.free(r.stdout);
    allocator.free(r.stderr);
}

fn sampleRecord(prompt_hash: []const u8) JournalRecord {
    return .{
        .prompt_hash = prompt_hash,
        .worktree = "/tmp/wt/cycle",
        .branch = "epic/p1/cycle/ts1",
        .claim_token = "tok-abc",
        .model = "claude-opus-4-8",
        .role = "coder",
        .task_slug = "ts1",
        .exit_code = 0,
        .terminal_verb = "completed",
        .wall_clock_ms = 1234,
        .timestamp = 1717000000,
    };
}

test "journal: journalPath derives <root>/.worktrees/.planar-execute/journal-<plan>.ndjson" {
    const a = testing.allocator;
    const p = try journalPath(a, "/repo/root", 492);
    defer a.free(p);
    try testing.expect(std.mem.indexOf(u8, p, ".worktrees") != null);
    try testing.expect(std.mem.indexOf(u8, p, ".planar-execute") != null);
    try testing.expect(std.mem.endsWith(u8, p, "journal-492.ndjson"));
    try testing.expect(std.mem.startsWith(u8, p, "/repo/root"));
}

test "journal: hashPrompt is stable + differs by content" {
    const a = testing.allocator;
    const h1 = try hashPrompt(a, "the brief");
    defer a.free(h1);
    const h1b = try hashPrompt(a, "the brief");
    defer a.free(h1b);
    const h2 = try hashPrompt(a, "a different brief");
    defer a.free(h2);
    try testing.expectEqualStrings(h1, h1b); // stable
    try testing.expect(!std.mem.eql(u8, h1, h2)); // content-sensitive
    try testing.expectEqual(@as(usize, 16), h1.len); // 16 hex chars
}

test "journal: append writes ONE NDJSON line per call (two records → two lines)" {
    const a = testing.allocator;
    const io = testing.io;
    const dir = mkTmpDir(a);
    defer a.free(dir);
    defer rmTree(a, dir);

    const path = try std.fs.path.join(a, &.{ dir, "journal-1.ndjson" });
    defer a.free(path);

    try append(a, io, path, sampleRecord("hash-aaaa"));
    try append(a, io, path, sampleRecord("hash-bbbb"));

    const data = try std.Io.Dir.cwd().readFileAlloc(io, path, a, .limited(1 << 20));
    defer a.free(data);

    // Exactly two newline-terminated lines.
    var line_count: usize = 0;
    var it = std.mem.splitScalar(u8, data, '\n');
    while (it.next()) |l| {
        if (std.mem.trim(u8, l, " \t\r").len != 0) line_count += 1;
    }
    try testing.expectEqual(@as(usize, 2), line_count);
    try testing.expect(std.mem.indexOf(u8, data, "hash-aaaa") != null);
    try testing.expect(std.mem.indexOf(u8, data, "hash-bbbb") != null);
    // The file ends with a newline (each append terminates its line).
    try testing.expect(data.len > 0 and data[data.len - 1] == '\n');
}

test "journal: appended record JSON carries all fields" {
    const a = testing.allocator;
    const io = testing.io;
    const dir = mkTmpDir(a);
    defer a.free(dir);
    defer rmTree(a, dir);

    const path = try std.fs.path.join(a, &.{ dir, "journal-2.ndjson" });
    defer a.free(path);

    try append(a, io, path, sampleRecord("hash-c"));

    const data = try std.Io.Dir.cwd().readFileAlloc(io, path, a, .limited(1 << 20));
    defer a.free(data);

    inline for ([_][]const u8{
        "prompt_hash", "worktree",      "branch",        "claim_token",
        "model",       "vendor",        "role",          "task_slug",
        "exit_code",   "terminal_verb", "wall_clock_ms", "timestamp",
    }) |field| {
        try testing.expect(std.mem.indexOf(u8, data, field) != null);
    }
}

test "journal: read round-trips N records with correct fields (alloc_always)" {
    const a = testing.allocator;
    const io = testing.io;
    const dir = mkTmpDir(a);
    defer a.free(dir);
    defer rmTree(a, dir);

    const path = try std.fs.path.join(a, &.{ dir, "journal-3.ndjson" });
    defer a.free(path);

    try append(a, io, path, sampleRecord("hash-1"));
    try append(a, io, path, sampleRecord("hash-2"));
    try append(a, io, path, sampleRecord("hash-3"));

    var arena = std.heap.ArenaAllocator.init(a);
    defer arena.deinit();
    const recs = try read(a, arena.allocator(), io, path);
    try testing.expectEqual(@as(usize, 3), recs.len);
    try testing.expectEqualStrings("hash-1", recs[0].prompt_hash);
    try testing.expectEqualStrings("hash-2", recs[1].prompt_hash);
    try testing.expectEqualStrings("hash-3", recs[2].prompt_hash);
    // Non-hash fields survive the round-trip too.
    try testing.expectEqualStrings("claude-opus-4-8", recs[0].model);
    try testing.expectEqualStrings("completed", recs[0].terminal_verb);
    try testing.expectEqual(@as(i64, 1234), recs[0].wall_clock_ms);
    try testing.expectEqual(@as(i32, 0), recs[0].exit_code);
    try testing.expectEqualStrings("coder", recs[0].role);
    try testing.expectEqualStrings("ts1", recs[0].task_slug);
}

test "journal: read on a missing file returns an empty slice (no prior attempts)" {
    const a = testing.allocator;
    const io = testing.io;
    const dir = mkTmpDir(a);
    defer a.free(dir);
    defer rmTree(a, dir);

    const path = try std.fs.path.join(a, &.{ dir, "journal-absent.ndjson" });
    defer a.free(path);

    var arena = std.heap.ArenaAllocator.init(a);
    defer arena.deinit();
    const recs = try read(a, arena.allocator(), io, path);
    try testing.expectEqual(@as(usize, 0), recs.len);
}

test "journal: append creates the .planar-execute parent dir if absent" {
    const a = testing.allocator;
    const io = testing.io;
    const root = mkTmpDir(a);
    defer a.free(root);
    defer rmTree(a, root);

    // Derive the real production path under a brand-new root (dir does NOT exist).
    const path = try journalPath(a, root, 9);
    defer a.free(path);

    try append(a, io, path, sampleRecord("hash-mkdir"));

    // The file (and its nested dir) now exist and carry the line.
    const data = try std.Io.Dir.cwd().readFileAlloc(io, path, a, .limited(1 << 20));
    defer a.free(data);
    try testing.expect(std.mem.indexOf(u8, data, "hash-mkdir") != null);
}
