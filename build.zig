const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // -----------------------------------------------------------------
    // Vendored deps — fail-fast at configure time if any vendor/<name>/
    // VENDOR.toml drifts from vendor/manifest.zon. `zig build
    // vendor-sync` invokes the same tool in `sync` mode to refresh the
    // tree (fetch + sha256 verify + extract + restamp). See
    // tools/vendor_sync.zig and vendor/manifest.zon.
    // -----------------------------------------------------------------
    const vendor_sync_exe = b.addExecutable(.{
        .name = "vendor_sync",
        .root_module = b.createModule(.{
            .root_source_file = b.path("tools/vendor_sync.zig"),
            .target = b.graph.host,
            .optimize = .Debug,
        }),
    });

    const vendor_check_run = b.addRunArtifact(vendor_sync_exe);
    vendor_check_run.addArg("check");
    vendor_check_run.addFileArg(b.path("vendor/manifest.zon"));
    vendor_check_run.addDirectoryArg(b.path("vendor"));
    addVendorStampInputs(b, vendor_check_run, "vendor");

    const vendor_sync_step = b.step("vendor-sync", "Fetch and re-vendor third-party deps per vendor/manifest.zon");
    const vendor_sync_run = b.addRunArtifact(vendor_sync_exe);
    vendor_sync_run.addArg("sync");
    vendor_sync_run.addFileArg(b.path("vendor/manifest.zon"));
    vendor_sync_run.addDirectoryArg(b.path("vendor"));
    vendor_sync_step.dependOn(&vendor_sync_run.step);

    // -----------------------------------------------------------------
    // SQLite — compile the vendored amalgamation as a static library.
    // We import the C source directly rather than depend on a wrapper
    // crate. Flags mirror the defaults Planar wants regardless of host
    // (thread-safe, FTS5, JSON1, strict DQS off).
    // -----------------------------------------------------------------
    const sqlite_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    sqlite_mod.addCSourceFile(.{
        .file = b.path("vendor/sqlite/sqlite3.c"),
        .flags = &.{
            "-DSQLITE_THREADSAFE=1",
            "-DSQLITE_ENABLE_FTS5",
            "-DSQLITE_ENABLE_JSON1",
            "-DSQLITE_DQS=0",
            "-DSQLITE_DEFAULT_FOREIGN_KEYS=1",
            "-DSQLITE_USE_URI=1",
            "-std=c99",
        },
    });
    sqlite_mod.addIncludePath(b.path("vendor/sqlite"));
    const sqlite_lib = b.addLibrary(.{
        .name = "sqlite3",
        .linkage = .static,
        .root_module = sqlite_mod,
    });

    // -----------------------------------------------------------------
    // Lua 5.5 — compile the vendored multi-file library as a static lib.
    // lua.c and luac.c (standalone interpreter/compiler mains) are excluded
    // from the source list; only the library sources are compiled.
    //
    // Platform define: Lua's Makefile uses LUA_USE_MACOSX on macOS,
    // LUA_USE_LINUX on Linux, and LUA_USE_POSIX as a portable fallback;
    // we mirror that conditional here via the host OS tag so cross-compile
    // targets get the right syscall/readline guards.
    // -----------------------------------------------------------------
    const lua_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    const lua_platform_define: []const u8 = switch (target.result.os.tag) {
        .macos => "-DLUA_USE_MACOSX",
        .linux => "-DLUA_USE_LINUX",
        else => "-DLUA_USE_POSIX",
    };
    const lua_src_dir = "vendor/lua/src";
    // Library C sources — mirrors CORE_O + LIB_O from Lua's Makefile.
    // lua.c and luac.c are intentionally absent.
    const lua_lib_c_sources: []const []const u8 = &.{
        lua_src_dir ++ "/lapi.c",
        lua_src_dir ++ "/lauxlib.c",
        lua_src_dir ++ "/lbaselib.c",
        lua_src_dir ++ "/lcode.c",
        lua_src_dir ++ "/lcorolib.c",
        lua_src_dir ++ "/lctype.c",
        lua_src_dir ++ "/ldblib.c",
        lua_src_dir ++ "/ldebug.c",
        lua_src_dir ++ "/ldo.c",
        lua_src_dir ++ "/ldump.c",
        lua_src_dir ++ "/lfunc.c",
        lua_src_dir ++ "/lgc.c",
        lua_src_dir ++ "/linit.c",
        lua_src_dir ++ "/liolib.c",
        lua_src_dir ++ "/llex.c",
        lua_src_dir ++ "/lmathlib.c",
        lua_src_dir ++ "/lmem.c",
        lua_src_dir ++ "/loadlib.c",
        lua_src_dir ++ "/lobject.c",
        lua_src_dir ++ "/lopcodes.c",
        lua_src_dir ++ "/loslib.c",
        lua_src_dir ++ "/lparser.c",
        lua_src_dir ++ "/lstate.c",
        lua_src_dir ++ "/lstring.c",
        lua_src_dir ++ "/lstrlib.c",
        lua_src_dir ++ "/ltable.c",
        lua_src_dir ++ "/ltablib.c",
        lua_src_dir ++ "/ltm.c",
        lua_src_dir ++ "/lundump.c",
        lua_src_dir ++ "/lutf8lib.c",
        lua_src_dir ++ "/lvm.c",
        lua_src_dir ++ "/lzio.c",
    };
    for (lua_lib_c_sources) |src| {
        lua_mod.addCSourceFile(.{
            .file = b.path(src),
            .flags = &.{ lua_platform_define, "-std=c99" },
        });
    }
    lua_mod.addIncludePath(b.path(lua_src_dir));
    const lua_lib = b.addLibrary(.{
        .name = "lua55",
        .linkage = .static,
        .root_module = lua_mod,
    });

    // -----------------------------------------------------------------
    // Migrations codegen — scan ../migrations/ and emit a manifest.zig
    // that the `migrations` module exposes as `pub const all: []Migration`.
    // The generator runs on the build host, not the user's target.
    // -----------------------------------------------------------------
    const gen_exe = b.addExecutable(.{
        .name = "gen_migrations",
        .root_module = b.createModule(.{
            .root_source_file = b.path("tools/gen_migrations.zig"),
            .target = b.graph.host,
            .optimize = .Debug,
        }),
    });

    const gen_run = b.addRunArtifact(gen_exe);
    gen_run.addDirectoryArg(b.path("migrations"));
    const manifest_path = gen_run.addOutputFileArg("manifest.zig");

    // addDirectoryArg alone doesn't track directory CONTENTS for cache
    // invalidation — adding/removing a migration file leaves the
    // manifest stale. Enumerate every *.sql here and register each
    // as a file input so the cache key flips on any change.
    addMigrationDirInputs(b, gen_run, "migrations");

    const migrations_mod = b.addModule("migrations", .{
        .root_source_file = manifest_path,
        .target = target,
    });

    // -----------------------------------------------------------------
    // Templates codegen — scan ../src/internal/templates/defaults/ and
    // emit a `templates_embed.zig` exposing every JSON template as
    // `pub const all: []TemplateFile`. The defaults dir is the same one
    // the Go binary embeds via `embed.FS`; we keep a single source of
    // truth on disk and let both binaries pick it up at build time.
    // -----------------------------------------------------------------
    const gen_tmpl_exe = b.addExecutable(.{
        .name = "gen_templates",
        .root_module = b.createModule(.{
            .root_source_file = b.path("tools/gen_templates.zig"),
            .target = b.graph.host,
            .optimize = .Debug,
        }),
    });

    const gen_tmpl_run = b.addRunArtifact(gen_tmpl_exe);
    gen_tmpl_run.addDirectoryArg(b.path("templates/defaults"));
    const tmpl_manifest_path = gen_tmpl_run.addOutputFileArg("templates_embed.zig");
    addTemplateDirInputs(b, gen_tmpl_run, "templates/defaults");

    const templates_embed_mod = b.addModule("templates_embed", .{
        .root_source_file = tmpl_manifest_path,
        .target = target,
    });

    // -----------------------------------------------------------------
    // libvaxis — Zig TUI library (v0.6.0, MIT). Used by the cockpit
    // (`planar explore` / bare `planar` on a TTY). Vendored as a path
    // dep under vendor/libvaxis/; its transitive deps (zigimg, uucode)
    // are also vendored under vendor/zigimg/ and vendor/uucode/.
    //
    // libvaxis's own build.zig resolves zigimg and uucode from its dep
    // tree (patched to path deps in vendor/libvaxis/build.zig.zon).
    // We pass the uucode fields required for the cockpit's Unicode ops.
    // -----------------------------------------------------------------
    const libvaxis_dep = b.dependency("libvaxis", .{
        .target = target,
        .optimize = optimize,
    });
    const libvaxis_mod = libvaxis_dep.module("vaxis");

    // -----------------------------------------------------------------
    // `db` module: SQLite wrapper + migration runner. Needs the sqlite
    // headers (for @cImport) and the static lib (for linking).
    // -----------------------------------------------------------------
    const db_mod = b.addModule("db", .{
        .root_source_file = b.path("src/db/db.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    db_mod.addIncludePath(b.path("vendor/sqlite"));
    db_mod.linkLibrary(sqlite_lib);
    db_mod.addImport("migrations", migrations_mod);

    // -----------------------------------------------------------------
    // `cli` module: comptime-driven command-tree argument parser.
    // Sourced from the etc-cli package (vendored under vendor/etc-cli;
    // declared in build.zig.zon as a path dependency). Previously
    // lived in-tree under src/cli/; extracted upstream so it can be
    // shared across Planar and other CLI projects.
    // -----------------------------------------------------------------
    const etc_cli_dep = b.dependency("etc_cli", .{
        .target = target,
        .optimize = optimize,
    });
    const cli_mod = etc_cli_dep.module("cli");

    // -----------------------------------------------------------------
    // `runtime` module: shared process-context bootstrap. Linked into
    // every Planar binary (`planar`, future `planar-agent`,
    // `planar-watch`). Owns the singleton Ctx, lazy DB acquisition,
    // and the schema-version handshake. Depends on `db`.
    // -----------------------------------------------------------------
    const runtime_mod = b.addModule("runtime", .{
        .root_source_file = b.path("src/runtime/runtime.zig"),
        .target = target,
        .optimize = optimize,
    });
    runtime_mod.addImport("db", db_mod);

    // -----------------------------------------------------------------
    // `engine` module: Planar's business-logic layer.
    // Domain entities, CRUD, validation — owns no IO, takes *db.sqlite.Db
    // explicitly. The CLI handlers and integration tests both call into
    // it. Depends on `db` and nothing else.
    // -----------------------------------------------------------------
    const engine_mod = b.addModule("engine", .{
        .root_source_file = b.path("src/engine/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    engine_mod.addImport("db", db_mod);
    engine_mod.addImport("templates_embed", templates_embed_mod);

    // -----------------------------------------------------------------
    // Library module (existing planar package surface).
    // -----------------------------------------------------------------
    const mod = b.addModule("planar", .{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
    });
    mod.addImport("db", db_mod);
    mod.addImport("cli", cli_mod);
    mod.addImport("engine", engine_mod);

    // -----------------------------------------------------------------
    // build_options module: compile-time git sha + build date + dirty
    // flag, exposed to the `version` verb. Operators can override the
    // defaults via -Dgit-sha=<sha> / -Dbuild-date=<ISO8601> /
    // -Dgit-dirty=true; otherwise we auto-resolve from `git` against
    // the repo root. Auto-resolution failures produce "unknown" rather
    // than a build error so the binary still builds outside a git
    // checkout.
    // -----------------------------------------------------------------
    const sha_opt = b.option([]const u8, "git-sha", "Override git commit sha embedded in `planar version`");
    const date_opt = b.option([]const u8, "build-date", "Override ISO8601 build date embedded in `planar version`");
    const dirty_opt = b.option(bool, "git-dirty", "Override git-dirty marker embedded in `planar version`");
    // Plan 297 t#2937: test-binary flag gates the worktree-gate env-var
    // bypass. Production builds default to false — the env var
    // PLANAR_DISABLE_WORKTREE_GATE is dead code in that case and cannot
    // be used to bypass the gate. Integration-test builds pass
    // -Dtest-binary=true so the harness can inject the env var to
    // suppress the gate for fixture commands that run under a
    // `.worktrees/...` path (Planar is itself developed inside a
    // worktree). The worktree-scope scenario test un-sets the env var
    // to exercise the actual refusal path.
    const test_binary_opt = b.option(bool, "test-binary", "Mark this as a test binary (enables PLANAR_DISABLE_WORKTREE_GATE env-var bypass in worktree_gate)") orelse false;

    const resolved_sha = sha_opt orelse resolveGitSha(b) orelse "unknown";
    const resolved_date = date_opt orelse resolveBuildDate(b) orelse "unknown";
    const resolved_dirty: bool = dirty_opt orelse resolveGitDirty(b);

    const build_options = b.addOptions();
    build_options.addOption([]const u8, "git_sha", resolved_sha);
    build_options.addOption([]const u8, "build_date", resolved_date);
    build_options.addOption(bool, "git_dirty", resolved_dirty);
    build_options.addOption(bool, "test_binary", test_binary_opt);
    const build_options_mod = build_options.createModule();

    // -----------------------------------------------------------------
    // CLI executable.
    // -----------------------------------------------------------------
    const exe = b.addExecutable(.{
        .name = "planar",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/cmd/planar/main.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "planar", .module = mod },
                .{ .name = "db", .module = db_mod },
                .{ .name = "cli", .module = cli_mod },
                .{ .name = "engine", .module = engine_mod },
                .{ .name = "runtime", .module = runtime_mod },
                .{ .name = "build_options", .module = build_options_mod },
                .{ .name = "vaxis", .module = libvaxis_mod },
            },
        }),
    });
    b.installArtifact(exe);

    // -----------------------------------------------------------------
    // `planar-agent` executable (plan 85). Second binary in the
    // three-binary architecture. M1 ships the scaffold (schema-version
    // handshake + a single `version` verb); M2 wires the 13-verb
    // atomic / claim / action / reconcile / abort surface on top.
    //
    // Links runtime, db, cli, engine, build_options. Does NOT link
    // the planar package (operator handlers); the binary is its own
    // command tree under `src/cmd/planar-agent/`.
    //
    // The migrations module rides in via `db` (the migrate runner
    // consumes it). planar-agent uses `runtime.ensureDbConsumer`
    // which does NOT apply migrations — only `planar init` does.
    // -----------------------------------------------------------------
    const agent_exe = b.addExecutable(.{
        .name = "planar-agent",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/cmd/planar-agent/main.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "db", .module = db_mod },
                .{ .name = "cli", .module = cli_mod },
                .{ .name = "engine", .module = engine_mod },
                .{ .name = "runtime", .module = runtime_mod },
                .{ .name = "build_options", .module = build_options_mod },
            },
        }),
    });
    b.installArtifact(agent_exe);

    // -----------------------------------------------------------------
    // `planar-watch` executable (plan 85 M8). Third binary in the
    // three-binary architecture — the human-facing read-only viewer.
    // Opens the DB via `runtime.ensureDbStrictReadOnly`, which uses
    // `sqlite3_open_v2(..., SQLITE_OPEN_READONLY, ...)` so the SQLite
    // driver itself refuses any write SQL. That's the second line of
    // defense behind the capability boundary; the first is that the
    // command tree registers exactly 6 read verbs (feed / ps / claims
    // / actions / plans / log) plus `version` and `completion`.
    //
    // Links runtime, db, cli, engine (read paths only), build_options.
    // Does NOT link the planar package (operator handlers); the binary
    // has its own command tree under `src/cmd/planar-watch/`.
    // -----------------------------------------------------------------
    const watch_exe = b.addExecutable(.{
        .name = "planar-watch",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/cmd/planar-watch/main.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "db", .module = db_mod },
                .{ .name = "cli", .module = cli_mod },
                .{ .name = "engine", .module = engine_mod },
                .{ .name = "runtime", .module = runtime_mod },
                .{ .name = "build_options", .module = build_options_mod },
            },
        }),
    });
    b.installArtifact(watch_exe);

    // -----------------------------------------------------------------
    // `planar-doc` executable (plan 423 M6). Fourth binary in the
    // architecture — repo-state documentation manifest tool. Read-heavy
    // with one small state file (`.planar-manifest`) as its only write.
    // Links cli, engine (docs subsystem), runtime, build_options. Does
    // NOT link db — this binary never opens SQLite. Capability boundary
    // is the verb set: build, verify, diff, cover, nodoc, lint.
    // -----------------------------------------------------------------
    const doc_exe = b.addExecutable(.{
        .name = "planar-doc",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/cmd/planar-doc/main.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "cli", .module = cli_mod },
                .{ .name = "engine", .module = engine_mod },
                .{ .name = "runtime", .module = runtime_mod },
                .{ .name = "build_options", .module = build_options_mod },
            },
        }),
    });
    b.installArtifact(doc_exe);

    // -----------------------------------------------------------------
    // `planar-execute` executable (plan 492 M1). Fifth binary — the Lua
    // script execution harness. Links Lua as a static lib; intentionally
    // does NOT link the db, engine, or runtime modules (no DB handle in
    // M1). Script execution, module loading, and CLI flag parsing are
    // tasks 3163-3166; this M1 target only proves the Lua link works.
    // -----------------------------------------------------------------
    const execute_exe = b.addExecutable(.{
        .name = "planar-execute",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/cmd/planar-execute/main.zig"),
            .target = target,
            .optimize = optimize,
            .link_libc = true,
            .imports = &.{
                .{ .name = "cli", .module = cli_mod },
            },
        }),
    });
    execute_exe.root_module.addIncludePath(b.path(lua_src_dir));
    execute_exe.root_module.linkLibrary(lua_lib);
    b.installArtifact(execute_exe);

    // Vendored-deps drift check runs before the binary is installed, so
    // `zig build` (which depends on the install step) fails loudly on
    // any vendor/<name>/VENDOR.toml mismatch.
    b.getInstallStep().dependOn(&vendor_check_run.step);

    // -----------------------------------------------------------------
    // CLI-usage lint. Dumps each binary's `schema` JSON and validates
    // that authored prose (agents/, skills/src/, docs/) never references
    // a flag the binary does not expose. Wired into `make test-all` /
    // CI via the `cli-usage-check` step.
    // -----------------------------------------------------------------
    const cli_usage_lint_exe = b.addExecutable(.{
        .name = "cli_usage_lint",
        .root_module = b.createModule(.{
            .root_source_file = b.path("tools/cli_usage_lint.zig"),
            .target = b.graph.host,
            .optimize = .Debug,
        }),
    });
    const cli_usage_check_step = b.step("cli-usage-check", "Validate authored CLI invocations against the live command schema");
    const cli_usage_check_run = b.addRunArtifact(cli_usage_lint_exe);
    cli_usage_check_run.step.dependOn(b.getInstallStep());
    cli_usage_check_run.addArg(b.pathFromRoot("."));
    cli_usage_check_run.addArg(b.getInstallPath(.bin, "planar"));
    cli_usage_check_run.addArg(b.getInstallPath(.bin, "planar-agent"));
    cli_usage_check_run.addArg(b.getInstallPath(.bin, "planar-watch"));
    cli_usage_check_run.addArg(b.getInstallPath(.bin, "planar-doc"));
    cli_usage_check_run.addArg(b.getInstallPath(.bin, "planar-execute"));
    cli_usage_check_step.dependOn(&cli_usage_check_run.step);

    const run_step = b.step("run", "Run the app");
    const run_cmd = b.addRunArtifact(exe);
    run_step.dependOn(&run_cmd.step);
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    // -----------------------------------------------------------------
    // Tests. Each test executable only sees its own module's `test`
    // blocks, so we add one per module we want covered.
    //
    // -Dtest-filter="<substring>" runs only tests whose names contain the
    // substring (the default test runner skips the rest). Repeatable:
    //   zig build test -Dtest-filter=parse -Dtest-filter=help
    // matches tests whose name contains EITHER substring.
    // -----------------------------------------------------------------
    const test_filters_opt = b.option(
        []const []const u8,
        "test-filter",
        "Only run tests whose name contains this substring (repeatable)",
    ) orelse &.{};

    const mod_tests = b.addTest(.{ .root_module = mod, .filters = test_filters_opt });
    const run_mod_tests = b.addRunArtifact(mod_tests);

    const exe_tests = b.addTest(.{ .root_module = exe.root_module, .filters = test_filters_opt });
    const run_exe_tests = b.addRunArtifact(exe_tests);

    const agent_exe_tests = b.addTest(.{ .root_module = agent_exe.root_module, .filters = test_filters_opt });
    const run_agent_exe_tests = b.addRunArtifact(agent_exe_tests);

    const watch_exe_tests = b.addTest(.{ .root_module = watch_exe.root_module, .filters = test_filters_opt });
    const run_watch_exe_tests = b.addRunArtifact(watch_exe_tests);

    const doc_exe_tests = b.addTest(.{ .root_module = doc_exe.root_module, .filters = test_filters_opt });
    const run_doc_exe_tests = b.addRunArtifact(doc_exe_tests);

    const execute_exe_tests = b.addTest(.{ .root_module = execute_exe.root_module, .filters = test_filters_opt });
    const run_execute_exe_tests = b.addRunArtifact(execute_exe_tests);

    const cli_usage_lint_tests = b.addTest(.{ .root_module = cli_usage_lint_exe.root_module, .filters = test_filters_opt });
    const run_cli_usage_lint_tests = b.addRunArtifact(cli_usage_lint_tests);

    const db_tests = b.addTest(.{ .root_module = db_mod, .filters = test_filters_opt });
    const run_db_tests = b.addRunArtifact(db_tests);

    const cli_tests = b.addTest(.{ .root_module = cli_mod, .filters = test_filters_opt });
    const run_cli_tests = b.addRunArtifact(cli_tests);

    const engine_tests = b.addTest(.{ .root_module = engine_mod, .filters = test_filters_opt });
    const run_engine_tests = b.addRunArtifact(engine_tests);

    const runtime_tests = b.addTest(.{ .root_module = runtime_mod, .filters = test_filters_opt });
    const run_runtime_tests = b.addRunArtifact(runtime_tests);

    const test_step = b.step("test", "Run tests");
    test_step.dependOn(&run_mod_tests.step);
    test_step.dependOn(&run_exe_tests.step);
    test_step.dependOn(&run_agent_exe_tests.step);
    test_step.dependOn(&run_watch_exe_tests.step);
    test_step.dependOn(&run_doc_exe_tests.step);
    test_step.dependOn(&run_execute_exe_tests.step);
    test_step.dependOn(&run_cli_usage_lint_tests.step);
    test_step.dependOn(&run_db_tests.step);
    test_step.dependOn(&run_cli_tests.step);
    test_step.dependOn(&run_engine_tests.step);
    test_step.dependOn(&run_runtime_tests.step);

    // -----------------------------------------------------------------
    // Integration tests. Separate from `zig build test` (mirrors Go's
    // build-tag separation between unit and integration tiers).
    //
    // The harness resolves the binary path from PLANAR_BIN, which this
    // step sets by exporting the install artifact's path before spawning
    // the test executable. Tests live under integration_tests/ and are
    // pure black-box: they exec the binary against ephemeral DBs.
    //
    // Usage: zig build test-integration
    // -----------------------------------------------------------------
    const harness_mod = b.addModule("harness", .{
        .root_source_file = b.path("integration_tests/harness.zig"),
        .target = target,
        .optimize = optimize,
    });

    // The default integration path uses one umbrella root. This is the
    // common local loop: one test executable, no duplicate smoke-root imports,
    // and the same per-test Suite isolation inside each test block.
    const test_integration_step = b.step("test-integration", "Run integration tests (requires compiled binary)");
    const integration_all = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("integration_tests/all_test.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "harness", .module = harness_mod },
            },
        }),
        .filters = test_filters_opt,
    });
    const run_integration_all = b.addRunArtifact(integration_all);
    run_integration_all.step.dependOn(b.getInstallStep());
    run_integration_all.setEnvironmentVariable(
        "PLANAR_BIN",
        b.getInstallPath(.bin, "planar"),
    );
    run_integration_all.setEnvironmentVariable(
        "PLANAR_AGENT_BIN",
        b.getInstallPath(.bin, "planar-agent"),
    );
    run_integration_all.setEnvironmentVariable(
        "PLANAR_WATCH_BIN",
        b.getInstallPath(.bin, "planar-watch"),
    );
    run_integration_all.setEnvironmentVariable(
        "PLANAR_DOC_BIN",
        b.getInstallPath(.bin, "planar-doc"),
    );
    run_integration_all.setEnvironmentVariable(
        "PLANAR_EXECUTE_BIN",
        b.getInstallPath(.bin, "planar-execute"),
    );
    test_integration_step.dependOn(&run_integration_all.step);

    // Per-file executables remain available when failure isolation is worth
    // the extra build graph overhead.
    const test_integration_files_step = b.step("test-integration-files", "Run integration tests as one executable per test file");

    // Discover top-level integration tests AND scenario tests under
    // integration_tests/scenarios/. Both directories are treated
    // identically; the split is purely organizational (focused per-verb
    // tests at the top, multi-verb workflow scenarios under scenarios/).
    // See CLAUDE.md "Integration test methodology" for the convention.
    const int_test_dirs = [_][]const u8{
        "integration_tests",
        "integration_tests/scenarios",
    };
    for (int_test_dirs) |dir_rel| {
        registerIntegrationTestDir(
            b,
            dir_rel,
            harness_mod,
            target,
            optimize,
            test_filters_opt,
            test_integration_files_step,
        );
    }
}

fn registerIntegrationTestDir(
    b: *std.Build,
    dir_rel: []const u8,
    harness_mod: *std.Build.Module,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    test_filters_opt: []const []const u8,
    test_integration_step: *std.Build.Step,
) void {
    const dir_abs = b.pathFromRoot(dir_rel);
    var dir = std.Io.Dir.openDirAbsolute(b.graph.io, dir_abs, .{ .iterate = true }) catch |e| switch (e) {
        // A missing scenarios/ directory is fine — the bucket is added
        // incrementally. Only the top-level integration_tests/ dir is
        // load-bearing; if that is missing we still panic via the
        // outer enumeration.
        error.FileNotFound => return,
        else => std.debug.panic("build.zig: cannot open '{s}': {s}", .{ dir_abs, @errorName(e) }),
    };
    defer dir.close(b.graph.io);

    var it = dir.iterate();
    while (it.next(b.graph.io) catch |e| std.debug.panic("build.zig: iterate {s}: {s}", .{ dir_abs, @errorName(e) })) |entry| {
        if (entry.kind != .file) continue;
        if (!std.mem.endsWith(u8, entry.name, "_test.zig")) continue;

        const test_rel = std.fs.path.join(b.allocator, &.{ dir_rel, entry.name }) catch @panic("OOM");

        const test_exe = b.addTest(.{
            .root_module = b.createModule(.{
                .root_source_file = b.path(test_rel),
                .target = target,
                .optimize = optimize,
                .imports = &.{
                    .{ .name = "harness", .module = harness_mod },
                },
            }),
            .filters = test_filters_opt,
        });

        const run = b.addRunArtifact(test_exe);
        run.step.dependOn(b.getInstallStep());
        run.setEnvironmentVariable(
            "PLANAR_BIN",
            b.getInstallPath(.bin, "planar"),
        );
        run.setEnvironmentVariable(
            "PLANAR_AGENT_BIN",
            b.getInstallPath(.bin, "planar-agent"),
        );
        run.setEnvironmentVariable(
            "PLANAR_WATCH_BIN",
            b.getInstallPath(.bin, "planar-watch"),
        );
        run.setEnvironmentVariable(
            "PLANAR_DOC_BIN",
            b.getInstallPath(.bin, "planar-doc"),
        );
        run.setEnvironmentVariable(
            "PLANAR_EXECUTE_BIN",
            b.getInstallPath(.bin, "planar-execute"),
        );
        test_integration_step.dependOn(&run.step);
    }
}

/// Register every `*.sql` file under `dir_rel_to_build` as a file
/// input on the gen_migrations run step. Without this, the build
/// cache keys the gen step on the directory path only — adding or
/// removing a migration leaves the manifest stale until the user
/// blows away `.zig-cache/`. We enumerate at build-graph eval time
/// and let `addFileInput` thread each file into the cache key.
///
/// Uses `b.graph.io` since std.fs.openDirAbsolute was removed in Zig 0.16
/// — directory ops live on std.Io.Dir now and need an Io instance.
/// Panics on directory open / iterate errors: a missing migrations
/// dir is a build configuration error worth failing loudly.
/// Register every `*.json` file under `dir_rel_to_build/<system>/` as a file
/// input on the gen_templates run step. Same rationale as
/// `addMigrationDirInputs`: the cache key must flip when any template body
/// changes or a file is added/removed.
fn addTemplateDirInputs(b: *std.Build, run: *std.Build.Step.Run, dir_rel_to_build: []const u8) void {
    const abs = b.pathFromRoot(dir_rel_to_build);
    var root = std.Io.Dir.openDirAbsolute(b.graph.io, abs, .{ .iterate = true }) catch |e| {
        std.debug.panic("build.zig: cannot open templates defaults dir '{s}': {s}", .{ abs, @errorName(e) });
    };
    defer root.close(b.graph.io);

    var sys_it = root.iterate();
    while (sys_it.next(b.graph.io) catch |e| std.debug.panic("build.zig: iterate {s}: {s}", .{ abs, @errorName(e) })) |sys_entry| {
        if (sys_entry.kind != .directory) continue;
        const sys_dir_rel = std.fs.path.join(b.allocator, &.{ dir_rel_to_build, sys_entry.name }) catch @panic("OOM");
        const sys_abs = b.pathFromRoot(sys_dir_rel);
        var sys_dir = std.Io.Dir.openDirAbsolute(b.graph.io, sys_abs, .{ .iterate = true }) catch |e| {
            std.debug.panic("build.zig: cannot open template system dir '{s}': {s}", .{ sys_abs, @errorName(e) });
        };
        defer sys_dir.close(b.graph.io);

        var file_it = sys_dir.iterate();
        while (file_it.next(b.graph.io) catch |e| std.debug.panic("build.zig: iterate {s}: {s}", .{ sys_abs, @errorName(e) })) |file_entry| {
            if (file_entry.kind != .file) continue;
            if (!std.mem.endsWith(u8, file_entry.name, ".json")) continue;
            const file_rel = std.fs.path.join(b.allocator, &.{ sys_dir_rel, file_entry.name }) catch @panic("OOM");
            run.addFileInput(b.path(file_rel));
        }
    }
}

/// resolveGitSha invokes `git -C <repo-root> rev-parse HEAD` at build
/// time and returns the trimmed sha. Returns null when git is missing,
/// the directory is not a checkout, or the command otherwise fails —
/// the caller substitutes "unknown" so the binary still builds outside
/// a git tree (release tarballs, vendored checkouts).
fn resolveGitSha(b: *std.Build) ?[]const u8 {
    const result = std.process.run(b.allocator, b.graph.io, .{
        .argv = &.{ "git", "-C", b.pathFromRoot("."), "rev-parse", "HEAD" },
    }) catch return null;
    defer b.allocator.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        b.allocator.free(result.stdout);
        return null;
    }
    const trimmed = std.mem.trim(u8, result.stdout, " \t\n\r");
    if (trimmed.len == 0) {
        b.allocator.free(result.stdout);
        return null;
    }
    // Copy out of result.stdout so we can free the buffer cleanly.
    const sha = b.allocator.dupe(u8, trimmed) catch {
        b.allocator.free(result.stdout);
        return null;
    };
    b.allocator.free(result.stdout);
    return sha;
}

/// resolveBuildDate invokes `git -C <repo-root> log -1 --format=%cI HEAD`
/// to pull the committer date of HEAD in ISO 8601 form. Null when git
/// isn't available; callers substitute "unknown".
fn resolveBuildDate(b: *std.Build) ?[]const u8 {
    const result = std.process.run(b.allocator, b.graph.io, .{
        .argv = &.{ "git", "-C", b.pathFromRoot("."), "log", "-1", "--format=%cI", "HEAD" },
    }) catch return null;
    defer b.allocator.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        b.allocator.free(result.stdout);
        return null;
    }
    const trimmed = std.mem.trim(u8, result.stdout, " \t\n\r");
    if (trimmed.len == 0) {
        b.allocator.free(result.stdout);
        return null;
    }
    const date = b.allocator.dupe(u8, trimmed) catch {
        b.allocator.free(result.stdout);
        return null;
    };
    b.allocator.free(result.stdout);
    return date;
}

/// resolveGitDirty invokes `git -C <repo-root> status --porcelain` and
/// returns true when the worktree has uncommitted modifications. Failed
/// invocations report `false` (no marker added).
fn resolveGitDirty(b: *std.Build) bool {
    const result = std.process.run(b.allocator, b.graph.io, .{
        .argv = &.{ "git", "-C", b.pathFromRoot("."), "status", "--porcelain" },
    }) catch return false;
    defer b.allocator.free(result.stderr);
    defer b.allocator.free(result.stdout);
    if (result.term != .exited or result.term.exited != 0) return false;
    const trimmed = std.mem.trim(u8, result.stdout, " \t\n\r");
    return trimmed.len > 0;
}

/// Register every `vendor/<name>/VENDOR.toml` as a file input on the
/// vendor-check run step. Without this, editing a stamp wouldn't
/// invalidate the cache and the check would silently pass.
fn addVendorStampInputs(b: *std.Build, run: *std.Build.Step.Run, vendor_rel: []const u8) void {
    const abs = b.pathFromRoot(vendor_rel);
    var root = std.Io.Dir.openDirAbsolute(b.graph.io, abs, .{ .iterate = true }) catch |e| {
        std.debug.panic("build.zig: cannot open vendor dir '{s}': {s}", .{ abs, @errorName(e) });
    };
    defer root.close(b.graph.io);

    var it = root.iterate();
    while (it.next(b.graph.io) catch |e| std.debug.panic("build.zig: iterate {s}: {s}", .{ abs, @errorName(e) })) |entry| {
        if (entry.kind != .directory) continue;
        const stamp_rel = std.fs.path.join(b.allocator, &.{ vendor_rel, entry.name, "VENDOR.toml" }) catch @panic("OOM");
        const stamp_abs = b.pathFromRoot(stamp_rel);
        // Only register if the stamp exists; missing stamps trigger
        // failure via the manifest-driven check at run time, and we
        // don't want addFileInput to choke on a non-existent path.
        std.Io.Dir.accessAbsolute(b.graph.io, stamp_abs, .{}) catch continue;
        run.addFileInput(b.path(stamp_rel));
    }
}

fn addMigrationDirInputs(b: *std.Build, run: *std.Build.Step.Run, dir_rel_to_build: []const u8) void {
    const abs = b.pathFromRoot(dir_rel_to_build);
    var dir = std.Io.Dir.openDirAbsolute(b.graph.io, abs, .{ .iterate = true }) catch |e| {
        std.debug.panic("build.zig: cannot open migrations dir '{s}': {s}", .{ abs, @errorName(e) });
    };
    defer dir.close(b.graph.io);

    var it = dir.iterate();
    while (it.next(b.graph.io) catch |e| std.debug.panic("build.zig: iterate {s}: {s}", .{ abs, @errorName(e) })) |entry| {
        if (entry.kind != .file) continue;
        if (!std.mem.endsWith(u8, entry.name, ".sql")) continue;
        const joined = std.fs.path.join(b.allocator, &.{ dir_rel_to_build, entry.name }) catch @panic("OOM");
        run.addFileInput(b.path(joined));
    }
}
