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

/// Resolve the path to the compiled `planar-ext` binary, if the caller's
/// environment names one.
///
/// Decisions 995-1001 moved the `ext`/top-level `sync` surface off `planar`
/// onto a dedicated `planar-ext` binary — but only for the C++ port. The
/// Zig-oracle `planar` binary still hosts `ext`/`sync` itself (it predates
/// the split), and `zig build test-integration` never sets PLANAR_EXT_BIN
/// because there is no oracle-side `planar-ext` to point it at.
///
/// So unlike `resolveBin`, this does NOT panic when the var is absent —
/// it returns null, and callers (`Suite.init`) fall back to the suite's
/// primary `bin` (PLANAR_BIN). `make test-parity-cpp` sets PLANAR_EXT_BIN
/// explicitly, which is what routes ext/sync calls to the real binary
/// under the C++ parity lane; every other lane is unaffected.
fn resolveExtBinOpt() ?[]const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_EXT_BIN=")) {
            return s["PLANAR_EXT_BIN=".len..];
        }
    }
    return null;
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
    /// Binary that owns the top-level `ext`/`sync` surface. Resolved from
    /// PLANAR_EXT_BIN when set (the C++ parity lane, via `make
    /// test-parity-cpp`); otherwise aliases `bin` (the Zig oracle, whose
    /// `planar` binary still hosts `ext`/`sync` itself). Only the
    /// ext*/mustRunExt* family of methods use this — plain `exec`/`mustRun`
    /// etc. always target `bin`.
    ext_bin: []const u8,
    /// Absolute path to the ephemeral database file (does not exist until the
    /// binary creates it on first use).
    db_path: []const u8,
    /// Absolute path to the ephemeral config file injected via PLANAR_CONFIG_PATH.
    /// The file does not exist by default, so the binary resolves config to its
    /// defaults (cli_log = false). Tests that need a specific config write the
    /// file and pass PLANAR_CONFIG_PATH via extra_env; that per-call value
    /// overrides this harness default because execWith applies extra_env after
    /// buildEnvMap.
    config_path: []const u8,
    /// Per-suite install roots keep filesystem health/status reads from
    /// observing the operator's real managed projections. Per-call extra_env
    /// overrides still win for tests that construct explicit install states.
    planar_home: []const u8,
    codex_home: []const u8,
    tmp_dir: std.testing.TmpDir,
    /// Lazily-resolved absolute path to `tmp_dir`. Owned by the suite; null
    /// until first access via `tmpAbsPath`.
    tmp_abs_cache: ?[]u8 = null,
    /// Lazily-resolved absolute version of `db_path`. Owned by the suite;
    /// null until first access via `absDbPath`. When `db_path` is already
    /// absolute the cache aliases it without re-allocating.
    abs_db_cache: ?[]u8 = null,
    /// Extra directories created by `freshSystemTmpDir` (literal-path
    /// system temp dirs, NOT under `.zig-cache/tmp/`). Each entry is an
    /// owned absolute path; `deinit` `rm -rf`s every entry and frees
    /// the slice.
    extra_dirs: std.ArrayList([]const u8) = .empty,

    /// Initialize the suite: resolve the binary path and create an ephemeral
    /// temp directory. The DB file is not created here — the binary does that
    /// on its first invocation via `ensureDb`.
    pub fn init(allocator: std.mem.Allocator) Suite {
        const bin = resolveBin();
        const ext_bin = resolveExtBinOpt() orelse bin;
        const tmp = std.testing.tmpDir(.{});
        // Build the absolute DB path: .zig-cache/tmp/<random>/planar.db
        const db_path = std.fs.path.join(allocator, &.{
            ".zig-cache/tmp",
            &tmp.sub_path,
            "planar.db",
        }) catch @panic("OOM building db_path");
        // Build the isolated config path: .zig-cache/tmp/<random>/config.toml
        // This file is intentionally NOT created here — the binary falls back to
        // built-in defaults (cli_log = false) when the path does not exist.
        // This prevents any test from reading the operator's real ~/.planar/config.toml.
        const config_path = std.fs.path.join(allocator, &.{
            ".zig-cache/tmp",
            &tmp.sub_path,
            "config.toml",
        }) catch @panic("OOM building config_path");
        const planar_home = std.fs.path.join(allocator, &.{
            ".zig-cache/tmp",
            &tmp.sub_path,
            "planar-home",
        }) catch @panic("OOM building planar_home");
        const codex_home = std.fs.path.join(allocator, &.{
            ".zig-cache/tmp",
            &tmp.sub_path,
            "codex-home",
        }) catch @panic("OOM building codex_home");
        // Isolation is load-bearing, so assert it rather than assume it. A
        // suite that ever resolved to the operator's real database would apply
        // this checkout's pending migrations to it on first use and break every
        // installed binary on the machine — silently, because migration is
        // automatic and a test that "passed" looks identical either way.
        assertIsolatedDbPath(db_path);

        const self: Suite = .{
            .allocator = allocator,
            .bin = bin,
            .ext_bin = ext_bin,
            .db_path = db_path,
            .config_path = config_path,
            .planar_home = planar_home,
            .codex_home = codex_home,
            .tmp_dir = tmp,
        };

        // `planar-ext` deliberately does NOT auto-apply migrations (decisions
        // 995-1001: it is read-only on planning tables and read-write on
        // exactly external_links/external_systems/sync_events, enforced by a
        // sqlite3_set_authorizer allowlist -- it is not the migration owner
        // and must not become one). `bin` (`planar`) DOES auto-migrate on
        // first use. When PLANAR_EXT_BIN names a distinct binary (the C++
        // parity lane, via `make test-parity-cpp`), a fresh per-suite DB has
        // no schema_migrations table at all yet, and the first call routed
        // to ext_bin would fail SchemaVersionBehind before any test body
        // runs. Seed once, here, by running `bin` against the same
        // db_path/env the suite will use for every subsequent call -- this
        // is the documented ordering contract (see docs/concepts.md
        // "Binaries"): an ext_bin caller must have `planar` initialize the
        // DB first. The Zig oracle lane (ext_bin aliases bin) needs no
        // seeding: whichever verb reaches the shared binary first migrates
        // it regardless of which family (`exec*` vs `execExt*`) issued it.
        //
        // The seed verb must (a) open and migrate the DB, (b) exit 0 on a
        // FRESH database, and (c) register nothing. `health` is the only
        // verb measured to satisfy all three. Do not "simplify" this to a
        // planning verb: `plan list --json` exits 1 on a fresh DB ("cwd is
        // not inside any registered Planar scope") even though it DOES
        // migrate, which panicked every one of the 652 suites; `scope show`
        // and `assoc list` exit 2 without creating the DB at all; `version`
        // exits 0 but never opens it; `init` works but registers cwd as a
        // project, perturbing fixtures that expect an unregistered scope.
        if (!std.mem.eql(u8, ext_bin, bin)) {
            const seed = self.execOnBin(bin, &.{"health"}, &.{});
            allocator.free(seed.stdout);
            allocator.free(seed.stderr);
            if (seed.term != .exited or seed.term.exited != 0) {
                std.debug.panic(
                    "Suite.init: seeding migrations via '{s} health' failed (needed before any planar-ext call against a fresh DB)",
                    .{bin},
                );
            }
        }

        return self;
    }

    /// Panic unless `path` is a build-directory scratch database.
    ///
    /// Checked positively (must live under `.zig-cache/tmp`) rather than by
    /// blocklisting `~/.planar/planar.db`: a blocklist only catches the one
    /// path someone thought of, while any DB outside the build dir is
    /// out of bounds for a test.
    fn assertIsolatedDbPath(path: []const u8) void {
        if (std.mem.startsWith(u8, path, ".zig-cache/tmp/")) return;
        std.debug.print(
            \\harness: refusing to run against a non-isolated database:
            \\  {s}
            \\Integration suites must use a scratch DB under .zig-cache/tmp/.
            \\
        , .{path});
        @panic("harness: non-isolated database path");
    }

    /// Release the temp directory and allocations owned by the suite.
    /// Call via `defer suite.deinit()` at the top of each test.
    pub fn deinit(self: *Suite) void {
        if (self.tmp_abs_cache) |p| self.allocator.free(p);
        if (self.abs_db_cache) |p| self.allocator.free(p);
        self.allocator.free(self.config_path);
        self.allocator.free(self.planar_home);
        self.allocator.free(self.codex_home);
        self.allocator.free(self.db_path);
        self.tmp_dir.cleanup();
        // Clean up any literal-path tmp dirs created via freshSystemTmpDir.
        // Shell out to `rm -rf` (matches the creation path which shells
        // to `mktemp`); any errors are swallowed since deinit must not
        // fail and a missing dir is harmless.
        for (self.extra_dirs.items) |dir_path| {
            const rm_result = std.process.run(self.allocator, std.testing.io, .{
                .argv = &.{ "rm", "-rf", dir_path },
            }) catch {
                self.allocator.free(dir_path);
                continue;
            };
            self.allocator.free(rm_result.stdout);
            self.allocator.free(rm_result.stderr);
            self.allocator.free(dir_path);
        }
        self.extra_dirs.deinit(self.allocator);
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

    /// Create a fresh empty directory at a literal `/var/folders/...`
    /// (macOS) or `/tmp/...` (Linux) path via shelling to `mktemp -d`.
    /// The returned path is the LITERAL string `mktemp` emits — on
    /// macOS `/var/folders/...` rather than `/private/var/folders/...`
    /// (the realPath of `/var` on macOS).
    ///
    /// Tests that exercise path-canonicalization-sensitive code paths
    /// (e.g. `init` vs `assoc add` binding) must use this rather than
    /// `tmpAbsPath()`. The harness's `tmp_dir` lives under
    /// `.zig-cache/tmp/...` which has no `/private/` shadow on macOS,
    /// so realpath-vs-literal divergence does not manifest there.
    ///
    /// The created directory is tracked in `extra_dirs` and removed
    /// recursively in `deinit()`. The returned slice is owned by the
    /// suite; callers must not free it.
    pub fn freshSystemTmpDir(self: *Suite) []const u8 {
        const gpa = self.allocator;
        const result = std.process.run(gpa, std.testing.io, .{
            .argv = &.{ "mktemp", "-d", "-t", "planar-int.XXXXXX" },
        }) catch |e| std.debug.panic(
            "freshSystemTmpDir: mktemp spawn failed: {s}",
            .{@errorName(e)},
        );
        defer gpa.free(result.stderr);
        if (result.term != .exited or result.term.exited != 0) {
            std.debug.print("\nmktemp stderr: {s}\n", .{result.stderr});
            gpa.free(result.stdout);
            @panic("freshSystemTmpDir: mktemp returned non-zero");
        }
        const trimmed = std.mem.trim(u8, result.stdout, " \t\r\n");
        const owned = gpa.dupe(u8, trimmed) catch @panic("OOM duping mktemp output");
        gpa.free(result.stdout);
        self.extra_dirs.append(gpa, owned) catch @panic("OOM tracking extra dir");
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
        // Isolate config: point every child process at a per-suite path that
        // does not exist by default. The binary resolves config to built-in
        // defaults (cli_log = false) when the file is absent, so no test
        // accidentally reads the operator's real ~/.planar/config.toml.
        // Tests that need a specific config write the file themselves and
        // pass PLANAR_CONFIG_PATH via extra_env; execWith applies extra_env
        // after buildEnvMap, so the per-call value wins.
        env_map.put("PLANAR_CONFIG_PATH", self.config_path) catch @panic("OOM injecting PLANAR_CONFIG_PATH");
        env_map.put("PLANAR_HOME", self.planar_home) catch @panic("OOM injecting PLANAR_HOME");
        env_map.put("CODEX_HOME", self.codex_home) catch @panic("OOM injecting CODEX_HOME");
        // Plan 297 M3 / t#2937: disable the worktree planning-verb gate
        // for the harness. The harness's tmp dirs may inherit a path
        // containing `.worktrees/` when Planar itself is being developed
        // inside a worktree (`.../planar/.worktrees/cycle/.../
        // .zig-cache/tmp/...`), which would otherwise refuse every
        // planning verb the scenarios drive.
        //
        // This env var is honored ONLY by test binaries compiled with
        // `-Dtest-binary=true` (wired in `Makefile` target
        // `test-integration`). The production binary ignores it — the
        // bypass is dead code when `build_options.test_binary = false`.
        //
        // The worktree-scope scenario test explicitly UN-sets this var
        // to exercise the actual refusal path.
        env_map.put("PLANAR_DISABLE_WORKTREE_GATE", "1") catch @panic("OOM injecting GATE flag");
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

    /// ExtraEnv carries extra environment variable overrides for execWith.
    pub const ExtraEnvEntry = struct { key: []const u8, value: []const u8 };

    /// Shared implementation behind exec/execWith/execExt/execExtWith:
    /// invoke `bin` with `args`, merging `extra_env` on top of the
    /// inherited + PLANAR_DB environment. Never fails the test; callers
    /// inspect `term` to decide.
    fn execOnBin(
        self: *const Suite,
        bin: []const u8,
        args: []const []const u8,
        extra_env: []const ExtraEnvEntry,
    ) RunResult {
        const gpa = self.allocator;

        // Compose full argv: binary + caller-supplied args.
        var argv_buf: [1 + max_argv_extra][]const u8 = undefined;
        argv_buf[0] = bin;
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
            std.debug.panic("Suite.execOnBin failed to spawn '{s}': {s}", .{ bin, @errorName(e) });
        };

        return .{
            .stdout = result.stdout,
            .stderr = result.stderr,
            .term = result.term,
        };
    }

    /// Execute the binary with the given extra arguments (the binary path is
    /// prepended automatically). Returns stdout, stderr, and the exit term.
    /// Never fails the test; callers inspect `term` to decide.
    pub fn exec(self: *const Suite, args: []const []const u8) RunResult {
        return self.execOnBin(self.bin, args, &.{});
    }

    /// Like exec, but accepts additional environment variables that are merged
    /// on top of the inherited + PLANAR_DB environment. Later entries override
    /// earlier ones. Used by tests that need to control PAGER, PLANAR_EDITOR, etc.
    pub fn execWith(
        self: *const Suite,
        args: []const []const u8,
        extra_env: []const ExtraEnvEntry,
    ) RunResult {
        return self.execOnBin(self.bin, args, extra_env);
    }

    /// Like exec, but targets the `ext`/top-level-`sync` binary (`ext_bin`)
    /// instead of `bin`. Use for `&.{"ext", ...}` / `&.{"sync", ...}`
    /// invocations — NOT for `workbench sync`, which stays on `bin`.
    pub fn execExt(self: *const Suite, args: []const []const u8) RunResult {
        return self.execOnBin(self.ext_bin, args, &.{});
    }

    /// Like execWith, but targets `ext_bin`. See `execExt`.
    pub fn execExtWith(
        self: *const Suite,
        args: []const []const u8,
        extra_env: []const ExtraEnvEntry,
    ) RunResult {
        return self.execOnBin(self.ext_bin, args, extra_env);
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

    /// Shared non-zero-exit panic path behind mustRun/mustRunWith/mustRunExt/
    /// mustRunExtWith. Frees `res.stderr` on success; returns `res.stdout`.
    fn mustFromResult(self: *const Suite, label: []const u8, res: RunResult) []u8 {
        if (res.term != .exited or res.term.exited != 0) {
            std.debug.print(
                "\n{s}: non-zero exit\nstdout: {s}\nstderr: {s}\n",
                .{ label, res.stdout, res.stderr },
            );
            self.allocator.free(res.stderr);
            self.allocator.free(res.stdout);
            @panic("mustRun: non-zero exit (see stderr above)");
        }
        self.allocator.free(res.stderr);
        return res.stdout;
    }

    /// Shared unexpected-success panic path behind expectFailure/
    /// expectFailureWith/expectFailureExt/expectFailureExtWith. Frees
    /// `res.stdout` on success; returns `res.stderr`.
    fn expectFailureFromResult(self: *const Suite, label: []const u8, res: RunResult) []u8 {
        if (res.term == .exited and res.term.exited == 0) {
            std.debug.print(
                "\n{s}: command succeeded unexpectedly\nstdout: {s}\n",
                .{ label, res.stdout },
            );
            self.allocator.free(res.stdout);
            self.allocator.free(res.stderr);
            @panic("expectFailure: command exited 0 (negative-path assertion broken)");
        }
        self.allocator.free(res.stdout);
        return res.stderr;
    }

    /// mustRun executes the binary with the given arguments. Fails the test
    /// LOUDLY if the process exits non-zero — panics so the test runner
    /// surfaces the failure rather than silently returning bogus stdout.
    /// Returns stdout; caller must free with `self.allocator.free(stdout)`.
    pub fn mustRun(self: *const Suite, args: []const []const u8) []u8 {
        return self.mustFromResult("mustRun", self.exec(args));
    }

    /// Like mustRun, but targets `ext_bin` — the binary that owns the
    /// top-level `ext`/`sync` surface. Use for `&.{"ext", ...}` /
    /// `&.{"sync", ...}` invocations — NOT for `workbench sync`.
    pub fn mustRunExt(self: *const Suite, args: []const []const u8) []u8 {
        return self.mustFromResult("mustRunExt", self.execExt(args));
    }

    /// mustRunJSON executes the binary, decodes stdout as a single JSON object
    /// into `T`, and returns the parsed value. Fails the test on non-zero exit
    /// or JSON decode error. String fields in the returned value are owned by
    /// `arena` and remain valid until the arena is freed.
    /// Shared JSON-decode panic path behind mustRunJSON/mustRunExtJSON.
    fn parseMustJSON(comptime T: type, arena: std.mem.Allocator, stdout: []const u8) T {
        const parsed = std.json.parseFromSlice(T, arena, stdout, .{
            .allocate = .alloc_always,
            .ignore_unknown_fields = true,
        }) catch |e| {
            std.debug.print(
                "\nmustRunJSON: JSON decode failed: {s}\nraw: {s}\n",
                .{ @errorName(e), stdout },
            );
            @panic("mustRunJSON: JSON decode failed (see raw above)");
        };
        // The value is owned by the arena; just return the inner typed value.
        return parsed.value;
    }

    pub fn mustRunJSON(
        self: *const Suite,
        comptime T: type,
        arena: std.mem.Allocator,
        args: []const []const u8,
    ) T {
        const stdout = self.mustRun(args);
        defer self.allocator.free(stdout);
        return parseMustJSON(T, arena, stdout);
    }

    /// Like mustRunJSON, but targets `ext_bin`. See `mustRunExt`.
    pub fn mustRunExtJSON(
        self: *const Suite,
        comptime T: type,
        arena: std.mem.Allocator,
        args: []const []const u8,
    ) T {
        const stdout = self.mustRunExt(args);
        defer self.allocator.free(stdout);
        return parseMustJSON(T, arena, stdout);
    }

    /// expectFailure runs the command and asserts that the process exits
    /// non-zero. Panics if the command succeeded — silent success on a
    /// negative-path test is misleading. Returns stderr; caller must
    /// free with `self.allocator.free`.
    pub fn expectFailure(self: *const Suite, args: []const []const u8) []u8 {
        return self.expectFailureFromResult("expectFailure", self.exec(args));
    }

    /// Like expectFailure, but targets `ext_bin`. See `mustRunExt`.
    pub fn expectFailureExt(self: *const Suite, args: []const []const u8) []u8 {
        return self.expectFailureFromResult("expectFailureExt", self.execExt(args));
    }

    /// mustRunWith is like mustRun but merges extra_env on top of the
    /// inherited environment. Panics LOUDLY on non-zero exit.
    /// Returns stdout; caller must free.
    pub fn mustRunWith(
        self: *const Suite,
        args: []const []const u8,
        extra_env: []const ExtraEnvEntry,
    ) []u8 {
        return self.mustFromResult("mustRunWith", self.execWith(args, extra_env));
    }

    /// Like mustRunWith, but targets `ext_bin`. See `mustRunExt`.
    pub fn mustRunExtWith(
        self: *const Suite,
        args: []const []const u8,
        extra_env: []const ExtraEnvEntry,
    ) []u8 {
        return self.mustFromResult("mustRunExtWith", self.execExtWith(args, extra_env));
    }

    /// expectFailureWith is like expectFailure but merges extra_env on top of
    /// the inherited environment. Panics if the command unexpectedly
    /// succeeded. Returns stderr; caller must free.
    pub fn expectFailureWith(
        self: *const Suite,
        args: []const []const u8,
        extra_env: []const ExtraEnvEntry,
    ) []u8 {
        return self.expectFailureFromResult("expectFailureWith", self.execWith(args, extra_env));
    }

    /// Like expectFailureWith, but targets `ext_bin`. See `mustRunExt`.
    pub fn expectFailureExtWith(
        self: *const Suite,
        args: []const []const u8,
        extra_env: []const ExtraEnvEntry,
    ) []u8 {
        return self.expectFailureFromResult("expectFailureExtWith", self.execExtWith(args, extra_env));
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
        // Set PWD to match the child's cwd. Shells do this on `cd`; the
        // child relies on PWD (not getcwd(3)) to learn its "literal as
        // passed" cwd vs the canonical realpath. Without this, init's
        // PWD-first cwd resolution (task 2375) would see the test
        // runner's PWD instead of the child's cwd — and the
        // path-canonicalization bug would silently disappear in tests
        // even when alive for real operators.
        const env = [_]ExtraEnvEntry{
            .{ .key = "PLANAR_DB", .value = self.absDbPath() },
            .{ .key = "PWD", .value = cwd },
        };
        const res = self.execWithInDir(cwd, args, &env);
        if (res.term != .exited or res.term.exited != 0) {
            std.debug.print(
                "\nmustRunInDir: non-zero exit in cwd '{s}'\nstdout: {s}\nstderr: {s}\n",
                .{ cwd, res.stdout, res.stderr },
            );
            self.allocator.free(res.stderr);
            self.allocator.free(res.stdout);
            @panic("mustRunInDir: non-zero exit (see stderr above)");
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
        // PWD mirrors mustRunInDir's rationale (task 2375).
        const env = [_]ExtraEnvEntry{
            .{ .key = "PLANAR_DB", .value = self.absDbPath() },
            .{ .key = "PWD", .value = cwd },
        };
        const res = self.execWithInDir(cwd, args, &env);
        if (res.term == .exited and res.term.exited == 0) {
            std.debug.print(
                "\nexpectFailureInDir: command succeeded unexpectedly in '{s}'\nstdout: {s}\n",
                .{ cwd, res.stdout },
            );
            self.allocator.free(res.stdout);
            self.allocator.free(res.stderr);
            @panic("expectFailureInDir: command exited 0 (negative-path assertion broken)");
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

// =========================================================================
// Shared --help SUBCOMMANDS-table parsing (task 6440 / task 6442).
//
// A raw substring/indexOf search over the whole `--help` text is unsafe
// under CLI11 (decision 948): wrapped description continuation lines are
// padded out to a fixed left-column width, so a description word can
// collide with a verb-shaped needle (e.g. "  claim " matching inside a
// wrapped sentence that happens to contain the word "claim"). Parsing the
// SUBCOMMANDS table into a verb set and testing set membership is immune
// to that false-positive class. Originally written for
// capability_boundary_test.zig (task 6440); promoted here so
// planar_watch_test.zig (task 6442) can share it rather than re-deriving
// it.
// =========================================================================

/// Parse a Planar `--help` SUBCOMMANDS table (CLI11's own formatter,
/// vendor/cli11/*/include/CLI/FormatterFwd.hpp's `column_width_{30}`) into
/// the set of verb names.
///
/// CLI11 emits the table as:
///   SUBCOMMANDS:
///     <name>                      <description ...>
///                                 <wrapped description continuation ...>
/// i.e. every real entry starts at EXACTLY 2 leading spaces; every wrapped
/// description continuation line is padded out to column 30 (CLI11's fixed
/// left-column width) and so starts with MORE than 2 leading spaces. We
/// tokenize an entry line on whitespace and take token[0] as the verb name,
/// and skip continuation lines outright rather than misreading their first
/// word as a verb. The table ends at the first blank line, or at the first
/// line that de-indents below 2 spaces (next top-level section).
///
/// The header itself must be matched as a whole line ("SUBCOMMANDS:"),
/// anchored at line-start — NOT via a raw substring search over the whole
/// help text, which would (and did) match inside the header's own tail
/// ("SUBCOMMANDS:" contains "COMMANDS:" as a suffix), silently defeating the
/// missing-header guard below.
pub fn parseHelpVerbs(
    gpa: std.mem.Allocator,
    help: []const u8,
) std.StringHashMap(void) {
    var set = std.StringHashMap(void).init(gpa);

    var line_it = std.mem.splitScalar(u8, help, '\n');
    const found_header = while (line_it.next()) |line| {
        if (std.mem.eql(u8, line, "SUBCOMMANDS:")) break true;
    } else false;
    if (!found_header) {
        std.debug.print("help output missing SUBCOMMANDS section:\n{s}\n", .{help});
        @panic("no SUBCOMMANDS header in help");
    }

    while (line_it.next()) |line| {
        if (line.len == 0) break; // table ends at first blank line.
        const indent = std.mem.indexOfNone(u8, line, " ") orelse continue;
        if (indent != 2) {
            if (indent < 2) break; // table ends at de-indent.
            continue; // wrapped description continuation line; not a verb.
        }
        // Tokenize on whitespace; first token is the verb.
        var tok_it = std.mem.tokenizeAny(u8, line, " \t");
        const first = tok_it.next() orelse continue;
        // Copy into a stable heap buffer so the map key outlives the
        // input slice (the input is owned by the caller and will be
        // freed before the caller reads the map).
        const owned = gpa.dupe(u8, first) catch @panic("OOM");
        set.put(owned, {}) catch @panic("OOM");
    }
    return set;
}

/// Free a verb set returned by `parseHelpVerbs`.
pub fn freeVerbSet(gpa: std.mem.Allocator, set: *std.StringHashMap(void)) void {
    var it = set.keyIterator();
    while (it.next()) |k| gpa.free(k.*);
    set.deinit();
}

/// Assert that `set` contains every verb in `required`; used to check that
/// read verbs are present.
pub fn assertContainsAll(
    set: *const std.StringHashMap(void),
    required: []const []const u8,
    bin: []const u8,
) !void {
    for (required) |v| {
        if (!set.contains(v)) {
            std.debug.print(
                "[{s}] capability-boundary: REQUIRED verb '{s}' missing from --help; have {d} verbs\n",
                .{ bin, v, set.count() },
            );
            var it = set.keyIterator();
            while (it.next()) |k| std.debug.print("  - {s}\n", .{k.*});
            return error.MissingRequiredVerb;
        }
    }
}

/// Assert that `set` contains none of `forbidden`; used for capability
/// boundary checks — a read-only binary must not expose a write verb.
pub fn assertContainsNone(
    set: *const std.StringHashMap(void),
    forbidden: []const []const u8,
    bin: []const u8,
) !void {
    for (forbidden) |v| {
        if (set.contains(v)) {
            std.debug.print(
                "[{s}] capability-boundary: FORBIDDEN verb '{s}' leaked into --help\n",
                .{ bin, v },
            );
            return error.ForbiddenVerbPresent;
        }
    }
}

/// Assert that `set` is exactly `expected` (same size, same members).
pub fn assertExactSet(
    set: *const std.StringHashMap(void),
    expected: []const []const u8,
    bin: []const u8,
) !void {
    if (set.count() != expected.len) {
        std.debug.print(
            "[{s}] capability-boundary: verb count mismatch — got {d}, expected {d}\n",
            .{ bin, set.count(), expected.len },
        );
        std.debug.print("  expected:\n", .{});
        for (expected) |v| std.debug.print("    - {s}\n", .{v});
        std.debug.print("  actual:\n", .{});
        var it = set.keyIterator();
        while (it.next()) |k| std.debug.print("    - {s}\n", .{k.*});
        return error.VerbSetSizeMismatch;
    }
    try assertContainsAll(set, expected, bin);
}
