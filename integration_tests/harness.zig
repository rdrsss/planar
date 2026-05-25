//! integration_tests/harness.zig — black-box test harness for the Planar Zig binary.
//!
//! Mirrors Go's src/integration_tests/harness_test.go. Each integration test
//! gets a fresh ephemeral database via the Suite helper; the binary under test
//! is resolved from PLANAR_BIN (set by `zig build test-integration`) or panics
//! with a clear message so callers know to use the build step.
//!
//! Design constraints honored:
//!   D-harness-shape  — Suite struct with mustRun / mustRunJSON / expectFailure.
//!   D-location       — src-zig/integration_tests/ (mirrors Go's layout).
//!   D-no-modifying-binary — this file imports nothing from src/ handlers or engine.

const std = @import("std");

// Maximum number of additional argv entries (beyond the binary itself) that
// the harness accepts. Raise if a future test needs more.
const max_argv_extra: usize = 32;

/// Resolve the path to the compiled binary.
///
/// Scans the POSIX environ block for PLANAR_BIN. The `test-integration` build
/// step sets PLANAR_BIN before spawning the test executable; if it is absent
/// the test binary was invoked outside of `zig build test-integration`, which
/// is a developer error — panic with a clear message rather than doing the
/// wrong thing silently.
fn resolveBin() []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_BIN=")) {
            return s["PLANAR_BIN=".len..];
        }
    }
    @panic(
        \\PLANAR_BIN is not set.
        \\Run integration tests via: zig build test-integration
        \\That step builds the binary and exports PLANAR_BIN automatically.
    );
}

/// Shared state for one integration test run. Allocate on the stack in each
/// test function; call deinit() when done (or defer it).
///
/// The database file lives inside the TmpDir at `planar.db`; the absolute
/// path is stored in `db_path` and injected into the child's environment via
/// PLANAR_DB on every invocation.
pub const Suite = struct {
    allocator: std.mem.Allocator,
    bin: []const u8,
    /// Absolute path to the ephemeral database file (does not exist until the
    /// binary creates it on first use).
    db_path: []const u8,
    tmp_dir: std.testing.TmpDir,

    /// Initialize the suite: resolve the binary path and create an ephemeral
    /// temp directory. The DB file is not created here — the binary does that
    /// on its first invocation via `ensureDb`.
    pub fn init(allocator: std.mem.Allocator) Suite {
        const bin = resolveBin();
        const tmp = std.testing.tmpDir(.{});
        // Build the absolute DB path: .zig-cache/tmp/<random>/planar.db
        const db_path = std.fs.path.join(allocator, &.{
            ".zig-cache/tmp",
            &tmp.sub_path,
            "planar.db",
        }) catch @panic("OOM building db_path");
        return .{
            .allocator = allocator,
            .bin = bin,
            .db_path = db_path,
            .tmp_dir = tmp,
        };
    }

    /// Release the temp directory and allocations owned by the suite.
    /// Call via `defer suite.deinit()` at the top of each test.
    pub fn deinit(self: *Suite) void {
        self.allocator.free(self.db_path);
        self.tmp_dir.cleanup();
    }

    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------

    /// Build an environment map from the current process env with PLANAR_DB
    /// injected. The caller owns the returned map and must call `.deinit()`.
    fn buildEnvMap(self: *const Suite) std.process.Environ.Map {
        const gpa = self.allocator;
        // Wrap the POSIX environ block so Environ.createMap can enumerate it.
        const raw: [*:null]?[*:0]u8 = std.c.environ;
        var env_count: usize = 0;
        while (raw[env_count] != null) : (env_count += 1) {}
        const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
        const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
        const environ: std.process.Environ = .{ .block = posix_block };

        var env_map = environ.createMap(gpa) catch @panic("OOM creating env map");
        env_map.put("PLANAR_DB", self.db_path) catch @panic("OOM injecting PLANAR_DB");
        return env_map;
    }

    /// Result of a single binary invocation. Caller is responsible for freeing
    /// stdout and stderr via `gpa.free(...)` or `res.deinit(gpa)`.
    pub const RunResult = struct {
        stdout: []u8,
        stderr: []u8,
        term: std.process.Child.Term,

        pub fn deinit(self: RunResult, gpa: std.mem.Allocator) void {
            gpa.free(self.stdout);
            gpa.free(self.stderr);
        }
    };

    /// Execute the binary with the given extra arguments (the binary path is
    /// prepended automatically). Returns stdout, stderr, and the exit term.
    /// Never fails the test; callers inspect `term` to decide.
    pub fn exec(self: *const Suite, args: []const []const u8) RunResult {
        const gpa = self.allocator;

        // Compose full argv: binary + caller-supplied args.
        var argv_buf: [1 + max_argv_extra][]const u8 = undefined;
        argv_buf[0] = self.bin;
        if (args.len > max_argv_extra) @panic("too many argv entries; raise max_argv_extra");
        for (args, 0..) |a, j| argv_buf[1 + j] = a;
        const argv = argv_buf[0 .. 1 + args.len];

        var env_map = self.buildEnvMap();
        defer env_map.deinit();

        const result = std.process.run(gpa, std.testing.io, .{
            .argv = argv,
            .environ_map = &env_map,
        }) catch |e| {
            std.debug.panic("Suite.exec failed to spawn '{s}': {s}", .{ self.bin, @errorName(e) });
        };

        return .{
            .stdout = result.stdout,
            .stderr = result.stderr,
            .term = result.term,
        };
    }

    /// ExtraEnv carries extra environment variable overrides for execWith.
    pub const ExtraEnvEntry = struct { key: []const u8, value: []const u8 };

    /// Like exec, but accepts additional environment variables that are merged
    /// on top of the inherited + PLANAR_DB environment. Later entries override
    /// earlier ones. Used by tests that need to control PAGER, PLANAR_EDITOR, etc.
    pub fn execWith(
        self: *const Suite,
        args: []const []const u8,
        extra_env: []const ExtraEnvEntry,
    ) RunResult {
        const gpa = self.allocator;

        // Compose full argv: binary + caller-supplied args.
        var argv_buf: [1 + max_argv_extra][]const u8 = undefined;
        argv_buf[0] = self.bin;
        if (args.len > max_argv_extra) @panic("too many argv entries; raise max_argv_extra");
        for (args, 0..) |a, j| argv_buf[1 + j] = a;
        const argv = argv_buf[0 .. 1 + args.len];

        var env_map = self.buildEnvMap();
        defer env_map.deinit();
        for (extra_env) |e| {
            env_map.put(e.key, e.value) catch @panic("OOM injecting extra env");
        }

        const result = std.process.run(gpa, std.testing.io, .{
            .argv = argv,
            .environ_map = &env_map,
        }) catch |e| {
            std.debug.panic("Suite.execWith failed to spawn '{s}': {s}", .{ self.bin, @errorName(e) });
        };

        return .{
            .stdout = result.stdout,
            .stderr = result.stderr,
            .term = result.term,
        };
    }

    /// Like execWith, but runs the command with process cwd set to `cwd`.
    pub fn execWithInDir(
        self: *const Suite,
        cwd: []const u8,
        args: []const []const u8,
        extra_env: []const ExtraEnvEntry,
    ) RunResult {
        const gpa = self.allocator;

        var argv_buf: [1 + max_argv_extra][]const u8 = undefined;
        argv_buf[0] = self.bin;
        if (args.len > max_argv_extra) @panic("too many argv entries; raise max_argv_extra");
        for (args, 0..) |a, j| argv_buf[1 + j] = a;
        const argv = argv_buf[0 .. 1 + args.len];

        var env_map = self.buildEnvMap();
        defer env_map.deinit();
        for (extra_env) |e| {
            env_map.put(e.key, e.value) catch @panic("OOM injecting extra env");
        }

        const result = std.process.run(gpa, std.testing.io, .{
            .argv = argv,
            .environ_map = &env_map,
            .cwd = .{ .path = cwd },
        }) catch |e| {
            std.debug.panic("Suite.execWithInDir failed to spawn '{s}' in '{s}': {s}", .{ self.bin, cwd, @errorName(e) });
        };

        return .{
            .stdout = result.stdout,
            .stderr = result.stderr,
            .term = result.term,
        };
    }

    // -------------------------------------------------------------------------
    // Public test helpers (mirror Go's BaseSuite methods)
    // -------------------------------------------------------------------------

    /// mustRun executes the binary with the given arguments. Fails the test
    /// if the process exits non-zero. Returns stdout; caller must free with
    /// `self.allocator.free(stdout)`.
    pub fn mustRun(self: *const Suite, args: []const []const u8) []u8 {
        const res = self.exec(args);
        if (res.term != .exited or res.term.exited != 0) {
            std.debug.print(
                "\nmustRun: non-zero exit\nstdout: {s}\nstderr: {s}\n",
                .{ res.stdout, res.stderr },
            );
            self.allocator.free(res.stderr);
            std.testing.expect(false) catch {};
            return res.stdout;
        }
        self.allocator.free(res.stderr);
        return res.stdout;
    }

    /// mustRunJSON executes the binary, decodes stdout as a single JSON object
    /// into `T`, and returns the parsed value. Fails the test on non-zero exit
    /// or JSON decode error. String fields in the returned value are owned by
    /// `arena` and remain valid until the arena is freed.
    pub fn mustRunJSON(
        self: *const Suite,
        comptime T: type,
        arena: std.mem.Allocator,
        args: []const []const u8,
    ) T {
        const stdout = self.mustRun(args);
        defer self.allocator.free(stdout);

        const parsed = std.json.parseFromSlice(T, arena, stdout, .{
            .allocate = .alloc_always,
            .ignore_unknown_fields = true,
        }) catch |e| {
            std.debug.print(
                "\nmustRunJSON: JSON decode failed: {s}\nraw: {s}\n",
                .{ @errorName(e), stdout },
            );
            std.testing.expect(false) catch {};
            unreachable;
        };
        // The value is owned by the arena; just return the inner typed value.
        return parsed.value;
    }

    /// expectFailure runs the command and asserts that the process exits
    /// non-zero. Returns stderr; caller must free with `self.allocator.free`.
    pub fn expectFailure(self: *const Suite, args: []const []const u8) []u8 {
        const res = self.exec(args);
        if (res.term == .exited and res.term.exited == 0) {
            std.debug.print(
                "\nexpectFailure: command succeeded unexpectedly\nstdout: {s}\n",
                .{res.stdout},
            );
            self.allocator.free(res.stdout);
            std.testing.expect(false) catch {};
        }
        self.allocator.free(res.stdout);
        return res.stderr;
    }

    /// mustRunWith is like mustRun but merges extra_env on top of the
    /// inherited environment. Returns stdout; caller must free.
    pub fn mustRunWith(
        self: *const Suite,
        args: []const []const u8,
        extra_env: []const ExtraEnvEntry,
    ) []u8 {
        const res = self.execWith(args, extra_env);
        if (res.term != .exited or res.term.exited != 0) {
            std.debug.print(
                "\nmustRunWith: non-zero exit\nstdout: {s}\nstderr: {s}\n",
                .{ res.stdout, res.stderr },
            );
            self.allocator.free(res.stderr);
            std.testing.expect(false) catch {};
            return res.stdout;
        }
        self.allocator.free(res.stderr);
        return res.stdout;
    }

    /// expectFailureWith is like expectFailure but merges extra_env on top of
    /// the inherited environment. Returns stderr; caller must free.
    pub fn expectFailureWith(
        self: *const Suite,
        args: []const []const u8,
        extra_env: []const ExtraEnvEntry,
    ) []u8 {
        const res = self.execWith(args, extra_env);
        if (res.term == .exited and res.term.exited == 0) {
            std.debug.print(
                "\nexpectFailureWith: command succeeded unexpectedly\nstdout: {s}\n",
                .{res.stdout},
            );
            self.allocator.free(res.stdout);
            std.testing.expect(false) catch {};
        }
        self.allocator.free(res.stdout);
        return res.stderr;
    }
};
