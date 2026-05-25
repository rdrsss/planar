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
    /// Lazily-resolved absolute path to `tmp_dir`. Owned by the suite; null
    /// until first access via `tmpAbsPath`.
    tmp_abs_cache: ?[]u8 = null,
    /// Lazily-resolved absolute version of `db_path`. Owned by the suite;
    /// null until first access via `absDbPath`. When `db_path` is already
    /// absolute the cache aliases it without re-allocating.
    abs_db_cache: ?[]u8 = null,

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
        if (self.tmp_abs_cache) |p| self.allocator.free(p);
        if (self.abs_db_cache) |p| self.allocator.free(p);
        self.allocator.free(self.db_path);
        self.tmp_dir.cleanup();
    }

    // -------------------------------------------------------------------------
    // Path helpers — useful when shelling commands into the tmp dir.
    // -------------------------------------------------------------------------

    /// Resolve the absolute path to the suite's tmp_dir. The returned slice
    /// is owned by the suite and freed by `deinit`; callers must not free it.
    pub fn tmpAbsPath(self: *Suite) []const u8 {
        if (self.tmp_abs_cache) |p| return p;
        var buf: [std.fs.max_path_bytes]u8 = undefined;
        const len = self.tmp_dir.dir.realPath(std.testing.io, &buf) catch
            @panic("cannot resolve tmp dir absolute path");
        const owned = self.allocator.dupe(u8, buf[0..len]) catch @panic("OOM");
        self.tmp_abs_cache = owned;
        return owned;
    }

    /// Return an absolute path to the suite DB, suitable for injection via
    /// `PLANAR_DB` when the child process runs with a non-default cwd
    /// (relative `db_path` would otherwise resolve against the child's cwd).
    /// The returned slice is owned by the suite and freed by `deinit`.
    pub fn absDbPath(self: *Suite) []const u8 {
        if (self.abs_db_cache) |p| return p;
        if (std.fs.path.isAbsolute(self.db_path)) {
            const owned = self.allocator.dupe(u8, self.db_path) catch @panic("OOM");
            self.abs_db_cache = owned;
            return owned;
        }
        const cwd_abs = std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", self.allocator) catch
            @panic("cannot resolve cwd absolute path");
        defer self.allocator.free(cwd_abs);
        const joined = std.fs.path.join(self.allocator, &.{ cwd_abs, self.db_path }) catch
            @panic("OOM building abs_db");
        self.abs_db_cache = joined;
        return joined;
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

    // -------------------------------------------------------------------------
    // Cwd-scope fixture primitives
    //
    // These helpers let an integration test make `suite.tmp_dir` look like a
    // registered Planar project so that the cwd-derive logic in
    // `src/engine/identity/scope.zig` resolves to a real scope when a verb
    // is run from inside the tmp. Mirrors the operator path — they shell
    // out to `planar init` / `planar assoc create` / `planar assoc add`
    // and never touch the database directly.
    //
    // Per-test isolation: every Suite has its own ephemeral tmp_dir + DB
    // file, so the registered project/association lives and dies with the
    // Suite — no leakage across tests.
    // -------------------------------------------------------------------------

    /// mustRunInDir runs the binary with cwd set to `cwd`. It also injects an
    /// absolute `PLANAR_DB` so the child can find the suite DB even when
    /// `suite.db_path` is relative to the test runner's cwd. Returns stdout;
    /// caller must free.
    pub fn mustRunInDir(
        self: *Suite,
        cwd: []const u8,
        args: []const []const u8,
    ) []u8 {
        const env = [_]ExtraEnvEntry{.{ .key = "PLANAR_DB", .value = self.absDbPath() }};
        const res = self.execWithInDir(cwd, args, &env);
        if (res.term != .exited or res.term.exited != 0) {
            std.debug.print(
                "\nmustRunInDir: non-zero exit in cwd '{s}'\nstdout: {s}\nstderr: {s}\n",
                .{ cwd, res.stdout, res.stderr },
            );
            self.allocator.free(res.stderr);
            std.testing.expect(false) catch {};
            return res.stdout;
        }
        self.allocator.free(res.stderr);
        return res.stdout;
    }

    /// expectFailureInDir runs the binary with cwd set to `cwd` and an
    /// absolute `PLANAR_DB` injected. Asserts the process exits non-zero;
    /// returns stderr (caller must free).
    pub fn expectFailureInDir(
        self: *Suite,
        cwd: []const u8,
        args: []const []const u8,
    ) []u8 {
        const env = [_]ExtraEnvEntry{.{ .key = "PLANAR_DB", .value = self.absDbPath() }};
        const res = self.execWithInDir(cwd, args, &env);
        if (res.term == .exited and res.term.exited == 0) {
            std.debug.print(
                "\nexpectFailureInDir: command succeeded unexpectedly in '{s}'\nstdout: {s}\n",
                .{ cwd, res.stdout },
            );
            self.allocator.free(res.stdout);
            std.testing.expect(false) catch {};
        }
        self.allocator.free(res.stdout);
        return res.stderr;
    }

    /// registerProject makes `suite.tmp_dir` look like a registered Planar
    /// project (writes a row into the `projects` table with the tmp's
    /// absolute path). Implemented by shelling `planar init` with the
    /// child's cwd set to the tmp dir — `init` reads cwd via realpath and
    /// inserts a project row pointing at it. Idempotent end-to-end:
    /// calling twice is harmless (the underlying `init` is INSERT OR
    /// IGNORE on the registration step).
    ///
    /// `name` is passed via `--name <name>` to the child. Pass null to let
    /// `planar init` derive the project name from the cwd basename.
    /// `--allow-no-repo` is always set because tmp dirs are not git repos.
    ///
    /// Returns the tmp's absolute path; the returned slice is owned by the
    /// suite (do not free).
    pub fn registerProject(self: *Suite, name: ?[]const u8) []const u8 {
        const root = self.tmpAbsPath();
        if (name) |n| {
            const out = self.mustRunInDir(root, &.{ "init", "--allow-no-repo", "--name", n });
            self.allocator.free(out);
        } else {
            const out = self.mustRunInDir(root, &.{ "init", "--allow-no-repo" });
            self.allocator.free(out);
        }
        return root;
    }

    /// addAssoc creates an association with `slug` (kind defaults to
    /// "project" since that is the common operator shape — single-repo
    /// project workflows) and binds the tmp-rooted project to it via
    /// `planar assoc add`. After this returns, cwd-derive run from inside
    /// `suite.tmp_dir` resolves to `slug`. Requires `registerProject` to
    /// have been called first (or for the caller to have otherwise
    /// registered a project at `suite.tmp_dir`).
    ///
    /// Pass `kind = null` to use the default ("project").
    pub fn addAssoc(self: *Suite, slug: []const u8, kind: ?[]const u8) void {
        const k = kind orelse "project";
        const create_out = self.mustRun(&.{ "assoc", "create", slug, "--kind", k });
        self.allocator.free(create_out);

        const root = self.tmpAbsPath();
        const add_out = self.mustRun(&.{ "assoc", "add", slug, root });
        self.allocator.free(add_out);
    }
};
