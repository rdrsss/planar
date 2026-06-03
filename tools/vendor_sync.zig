//! Build-time vendored-deps manager. Reads a declarative
//! `vendor/manifest.zon`, compares each entry against the
//! `vendor/<name>/VENDOR.toml` provenance stamp, and either:
//!
//!   - `check`: exits 0 if every dep's recorded version+sha matches the
//!     manifest. Exits 1 (with an actionable message) on the first
//!     mismatch or missing stamp. Read-only; no network.
//!   - `sync`:  for each dep, fetches the URL, verifies the archive
//!     SHA-256, hands the local archive off to `zig fetch` for content-
//!     hashing and normalisation, then copies the whitelisted files from
//!     `zig fetch`'s global-cache output into `vendor/<name>/`, and
//!     writes a fresh `VENDOR.toml`.
//!
//! ## Extraction design (decision 364)
//!
//! `zig fetch` is the canonical download/extract mechanism for ALL archive
//! formats (.zip, .tar.gz, .tar.xz, .tar.zst).  vendor_sync opts for
//! design choice **A**: it downloads the archive itself, verifies the
//! archive SHA-256 against the manifest pin, then passes the local file to
//! `zig fetch <path>` for format-neutral extraction and content-hashing.
//! The archive SHA-256 remains the integrity pin in `manifest.zon` and
//! `VENDOR.toml`.
//!
//! After `zig fetch <archive>`, the normalised package is stored at
//! `<global-cache>/p/<content-hash>.tar.gz`.  The files inside that
//! archive are prefixed with `<content-hash>/`.  Zig normalises BOTH zip
//! and tar archives by stripping the single top-level directory, so for a
//! dep originally packaged as `foo-1.2/file.c` the cached copy contains
//! only `<hash>/file.c`.  The `strip_prefix` field in `manifest.zon`
//! therefore means the path **within the zig-normalised tree** (after
//! `<hash>/`):
//!
//!   - zip deps:    `strip_prefix = ""` (zig stripped the top-level)
//!   - tar.gz deps: `strip_prefix = ""` (zig also strips the top-level)
//!
//! The build wires `check` as a configure-time fail-fast step so a stale
//! checkout cannot silently compile against the wrong upstream; `sync`
//! runs only when an operator invokes `zig build vendor-sync`.
//!
//! Usage:
//!   vendor_sync check <manifest-path> <vendor-dir>
//!   vendor_sync sync  <manifest-path> <vendor-dir>
const std = @import("std");
const Io = std.Io;

const Archive = enum { zip, tar_gz };

const Dep = struct {
    name: []const u8,
    version: []const u8,
    url: []const u8,
    sha256: []const u8,
    archive: Archive,
    /// Path prefix within the zig-normalised cached tree (after `<hash>/`).
    /// Zig strips the single top-level directory from both zip and tar.gz
    /// archives during normalisation, so this is typically empty string for
    /// both archive formats.  Set to a non-empty sub-path only when you want
    /// to import files from a specific subdirectory of the normalised tree.
    strip_prefix: []const u8,
    keep: []const []const u8,
};

const Manifest = struct {
    deps: []const Dep,
};

const Recorded = struct {
    version: []const u8,
    sha256: []const u8,
};

pub fn main(init: std.process.Init) !void {
    const arena = init.arena.allocator();
    const io = init.io;

    const args = try init.minimal.args.toSlice(arena);
    if (args.len != 4) {
        std.debug.print(
            "usage: {s} <check|sync> <manifest-path> <vendor-dir>\n",
            .{args[0]},
        );
        std.process.exit(2);
    }
    const sub = args[1];
    const manifest_path = args[2];
    const vendor_dir_path = args[3];

    const manifest = try loadManifest(arena, io, manifest_path);

    if (std.mem.eql(u8, sub, "check")) {
        try cmdCheck(arena, io, manifest, vendor_dir_path);
    } else if (std.mem.eql(u8, sub, "sync")) {
        try cmdSync(arena, io, &init, manifest, manifest_path, vendor_dir_path);
    } else {
        std.debug.print("vendor_sync: unknown subcommand '{s}'\n", .{sub});
        std.process.exit(2);
    }
}

fn loadManifest(arena: std.mem.Allocator, io: Io, path: []const u8) !Manifest {
    var cwd = Io.Dir.cwd();
    const limit: Io.Limit = .limited(1 * 1024 * 1024);
    // ZON parsing wants a sentinel-terminated slice.
    const raw = try cwd.readFileAlloc(io, path, arena, limit);
    const z = try arena.allocSentinel(u8, raw.len, 0);
    @memcpy(z, raw);

    var diag: std.zon.parse.Diagnostics = .{};
    defer diag.deinit(arena);
    return std.zon.parse.fromSliceAlloc(Manifest, arena, z, &diag, .{}) catch |err| {
        std.debug.print("vendor_sync: failed to parse {s}: {s}\n", .{ path, @errorName(err) });
        var it = diag.iterateErrors();
        while (it.next()) |e| {
            const loc = e.getLocation(&diag);
            std.debug.print("  {s}:{d}:{d}: {f}\n", .{
                path,
                loc.line + 1,
                loc.column + 1,
                e.fmtMessage(&diag),
            });
        }
        std.process.exit(1);
    };
}

fn cmdCheck(
    arena: std.mem.Allocator,
    io: Io,
    manifest: Manifest,
    vendor_dir_path: []const u8,
) !void {
    var any_mismatch = false;
    var cwd = Io.Dir.cwd();

    for (manifest.deps) |dep| {
        const stamp_rel = try std.fs.path.join(arena, &.{ vendor_dir_path, dep.name, "VENDOR.toml" });
        const limit: Io.Limit = .limited(64 * 1024);
        const stamp_bytes = cwd.readFileAlloc(io, stamp_rel, arena, limit) catch |err| switch (err) {
            error.FileNotFound => {
                std.debug.print(
                    \\vendor_sync: missing {s}
                    \\  manifest pins {s} {s}
                    \\  run: zig build vendor-sync
                    \\
                ,
                    .{ stamp_rel, dep.name, dep.version },
                );
                any_mismatch = true;
                continue;
            },
            else => return err,
        };

        const rec = parseRecorded(stamp_bytes) catch {
            std.debug.print("vendor_sync: malformed {s}; run: zig build vendor-sync\n", .{stamp_rel});
            any_mismatch = true;
            continue;
        };

        if (!std.mem.eql(u8, rec.version, dep.version) or !std.mem.eql(u8, rec.sha256, dep.sha256)) {
            std.debug.print(
                \\vendor_sync: {s} drift
                \\  on disk:  version={s} sha256={s}
                \\  manifest: version={s} sha256={s}
                \\  run: zig build vendor-sync
                \\
            ,
                .{ dep.name, rec.version, rec.sha256, dep.version, dep.sha256 },
            );
            any_mismatch = true;
        }
    }

    if (any_mismatch) std.process.exit(1);
}

fn cmdSync(
    arena: std.mem.Allocator,
    io: Io,
    init: *const std.process.Init,
    manifest: Manifest,
    manifest_path: []const u8,
    vendor_dir_path: []const u8,
) !void {
    var cwd = Io.Dir.cwd();
    try cwd.createDirPath(io, vendor_dir_path);

    // Derive the build root from the manifest path.  build.zig passes
    // vendor/manifest.zon as an absolute path via addFileArg(b.path(...)),
    // so dirname(dirname(manifest_path)) == the directory containing build.zig.
    // zig fetch must be run from there (it walks up from cwd looking for
    // build.zig).
    //
    // If manifest_path is relative (e.g. in tests), fall back to ".".
    const build_root: []const u8 = blk: {
        const p1 = std.fs.path.dirname(manifest_path) orelse break :blk ".";
        break :blk std.fs.path.dirname(p1) orelse ".";
    };

    var client: std.http.Client = .{ .allocator = arena, .io = io };
    defer client.deinit();

    for (manifest.deps) |dep| {
        std.debug.print("vendor_sync: fetching {s} {s}\n  {s}\n", .{ dep.name, dep.version, dep.url });

        var body: Io.Writer.Allocating = .init(arena);
        defer body.deinit();

        const result = client.fetch(.{
            .location = .{ .url = dep.url },
            .response_writer = &body.writer,
        }) catch |err| {
            std.debug.print("vendor_sync: fetch failed for {s}: {s}\n", .{ dep.name, @errorName(err) });
            std.process.exit(1);
        };
        if (@intFromEnum(result.status) != 200) {
            std.debug.print(
                "vendor_sync: fetch {s} returned HTTP {d}\n",
                .{ dep.url, @intFromEnum(result.status) },
            );
            std.process.exit(1);
        }

        const data = body.written();

        // --- 1. Verify archive SHA-256 against the manifest pin. ---
        var hash_buf: [std.crypto.hash.sha2.Sha256.digest_length]u8 = undefined;
        std.crypto.hash.sha2.Sha256.hash(data, &hash_buf, .{});
        const hex_buf = std.fmt.bytesToHex(hash_buf, .lower);
        const actual_sha: []const u8 = try arena.dupe(u8, &hex_buf);

        if (!std.mem.eql(u8, actual_sha, dep.sha256)) {
            std.debug.print(
                \\vendor_sync: sha256 mismatch for {s}
                \\  url:      {s}
                \\  expected: {s}
                \\  actual:   {s}
                \\
            ,
                .{ dep.name, dep.url, dep.sha256, actual_sha },
            );
            std.process.exit(1);
        }

        // --- 2. Write archive to a temp file and hand it to zig fetch. ---
        //
        // zig fetch downloads + normalises the archive into the global Zig
        // cache and prints the content hash.  We use a per-dep subdirectory
        // as the global-cache-dir so the fetch artefacts are self-contained
        // and can be cleaned up deterministically.  zig fetch requires a
        // `tmp/` subdirectory inside the cache dir.
        const dep_dir_rel = try std.fs.path.join(arena, &.{ vendor_dir_path, dep.name });
        const cache_dir_rel = try std.fs.path.join(arena, &.{ vendor_dir_path, dep.name, ".sync-cache" });
        const cache_tmp_rel = try std.fs.path.join(arena, &.{ cache_dir_rel, "tmp" });
        const archive_ext = switch (dep.archive) {
            .zip => "zip",
            .tar_gz => "tar.gz",
        };
        const archive_rel = try std.fmt.allocPrint(arena, "{s}/archive.{s}", .{ cache_dir_rel, archive_ext });

        // Clean up any leftover from a previous interrupted run.
        cwd.deleteTree(io, cache_dir_rel) catch {};
        try cwd.createDirPath(io, cache_tmp_rel);

        // Write the downloaded bytes to disk.
        {
            var blob = try cwd.createFile(io, archive_rel, .{});
            defer blob.close(io);
            try blob.writeStreamingAll(io, data);
        }

        // build.zig passes vendor_dir_path as an absolute path via
        // addDirectoryArg(b.path(...)), so archive_rel and cache_dir_rel are
        // already absolute.  We use them directly for subprocess arguments.

        // Snapshot the zig-pkg/ subdirectory names inside the build root
        // before running zig fetch.  zig fetch writes a zig-pkg/<hash>/
        // directory in its cwd (the build root) as a side-effect even with
        // --global-cache-dir redirecting the package cache.  After the fetch
        // we diff and deleteTree only the newly created entries so the build
        // root stays clean without touching any pre-existing zig-pkg/<hash>/
        // dirs from other processes or prior runs.
        var build_root_dir = cwd.openDir(io, build_root, .{}) catch null;
        defer if (build_root_dir) |*d| d.close(io);

        // Snapshot the zig-pkg/ entries that already exist, if any.
        // We store the names as a sorted list so we can diff after the fetch.
        var pre_zig_pkg = std.ArrayList([]const u8).empty;
        defer {
            for (pre_zig_pkg.items) |n| arena.free(n);
            pre_zig_pkg.deinit(arena);
        }
        var zig_pkg_existed_before = false;
        if (build_root_dir) |*brd| {
            var zpd = brd.openDir(io, "zig-pkg", .{ .iterate = true }) catch null;
            if (zpd) |*d| {
                zig_pkg_existed_before = true;
                defer d.close(io);
                var it = d.iterate();
                while (it.next(io) catch null) |entry| {
                    if (entry.kind != .directory) continue;
                    try pre_zig_pkg.append(arena, try arena.dupe(u8, entry.name));
                }
            }
        }

        // Run: zig fetch --global-cache-dir <cache_dir> <archive_path>
        // Must be run from a directory containing build.zig (the build root).
        const fetch_result = std.process.run(arena, io, .{
            .argv = &.{ "zig", "fetch", "--global-cache-dir", cache_dir_rel, archive_rel },
            .cwd = .{ .path = build_root },
            .stdout_limit = .limited(1024),
            .stderr_limit = .limited(64 * 1024),
        }) catch |err| {
            std.debug.print("vendor_sync: zig fetch failed for {s}: {s}\n", .{ dep.name, @errorName(err) });
            std.process.exit(1);
        };
        if (fetch_result.term != .exited or fetch_result.term.exited != 0) {
            std.debug.print(
                "vendor_sync: zig fetch exited with error for {s}:\n{s}\n",
                .{ dep.name, fetch_result.stderr },
            );
            std.process.exit(1);
        }

        // Clean up any zig-pkg/<hash>/ dirs that zig fetch created in the
        // build root.  Delete only entries that are new (not in the snapshot);
        // if zig-pkg/ itself was not present before and is now empty after
        // removing the new hashes, remove the zig-pkg/ dir too.
        if (build_root_dir) |*brd| {
            var zpd_after = brd.openDir(io, "zig-pkg", .{ .iterate = true }) catch null;
            if (zpd_after) |*d| {
                defer d.close(io);
                var new_hashes = std.ArrayList([]const u8).empty;
                defer new_hashes.deinit(arena);
                var it = d.iterate();
                while (it.next(io) catch null) |entry| {
                    if (entry.kind != .directory) continue;
                    // Is this entry new (not in the pre-fetch snapshot)?
                    var was_pre: bool = false;
                    for (pre_zig_pkg.items) |pre| {
                        if (std.mem.eql(u8, pre, entry.name)) {
                            was_pre = true;
                            break;
                        }
                    }
                    if (!was_pre) {
                        try new_hashes.append(arena, try arena.dupe(u8, entry.name));
                    }
                }
                // Delete the newly created zig-pkg/<hash>/ subdirs.
                for (new_hashes.items) |hash_name| {
                    const rel = try std.fmt.allocPrint(arena, "zig-pkg/{s}", .{hash_name});
                    brd.deleteTree(io, rel) catch {};
                }
                // If zig-pkg/ was not present before this fetch and is now
                // empty (all new entries removed), delete the empty dir too.
                if (!zig_pkg_existed_before) {
                    brd.deleteDir(io, "zig-pkg") catch {};
                }
            }
        }

        const content_hash = std.mem.trim(u8, fetch_result.stdout, " \t\n\r");
        std.debug.print("vendor_sync: zig fetch content hash: {s}\n", .{content_hash});

        // --- 3. Extract the normalised cached archive (in-process). ---
        //
        // The cached archive lives at <cache_dir>/p/<hash>.tar.gz.
        // Files inside are prefixed with `<hash>/`, then dep.strip_prefix.
        // We extract into a dedicated subdirectory of the cache dir entirely
        // in-process via std.compress.flate + std.tar — no external `tar`
        // subprocess, making extraction hermetic and cross-platform.
        const cached_tar = try std.fmt.allocPrint(arena, "{s}/p/{s}.tar.gz", .{ cache_dir_rel, content_hash });
        const extract_dir = try std.fs.path.join(arena, &.{ cache_dir_rel, "extract" });
        try cwd.createDirPath(io, extract_dir);

        {
            const tar_limit: Io.Limit = .limited(512 * 1024 * 1024);
            const tar_bytes = cwd.readFileAlloc(io, cached_tar, arena, tar_limit) catch |err| {
                std.debug.print("vendor_sync: failed to read cached archive for {s}: {s}\n", .{ dep.name, @errorName(err) });
                std.process.exit(1);
            };
            var dest = cwd.openDir(io, extract_dir, .{}) catch |err| {
                std.debug.print("vendor_sync: failed to open extract dir for {s}: {s}\n", .{ dep.name, @errorName(err) });
                std.process.exit(1);
            };
            defer dest.close(io);
            extractTarGz(io, dest, tar_bytes) catch |err| {
                std.debug.print("vendor_sync: in-process tar extract failed for {s}: {s}\n", .{ dep.name, @errorName(err) });
                std.process.exit(1);
            };
        }

        // --- 4. Copy the whitelisted files into vendor/<name>/. ---
        //
        // Source path within the extract dir:
        //   <extract_dir>/<content_hash>/<strip_prefix>/<fname>
        const src_prefix = try std.fs.path.join(arena, &.{
            extract_dir,
            content_hash,
            dep.strip_prefix,
        });
        try replaceDepFiles(arena, io, &cwd, dep, src_prefix, dep_dir_rel);

        // Clean up the temporary cache dir (archive + zig cache + extract tree).
        cwd.deleteTree(io, cache_dir_rel) catch {};

        try writeStamp(arena, io, &cwd, init, dep, actual_sha, dep_dir_rel);
        std.debug.print("vendor_sync: {s} synced (archive-sha256={s})\n", .{ dep.name, actual_sha });
    }
}

/// Extract a gzip-compressed tar archive from `data` into `dest`.
///
/// Uses std.compress.flate.Decompress (gzip container) layered over
/// std.tar.extract — entirely in-process, no subprocess. The archive
/// sha256 is verified by the caller before this function is invoked;
/// this function handles only decompression + extraction.
///
/// The tar content is extracted verbatim (strip_components = 0), so
/// the top-level directory from the tarball (the strip_prefix value
/// in the manifest) lands as a subdirectory of `dest`, matching the
/// behaviour of the .zip path.
fn extractTarGz(io: Io, dest: Io.Dir, data: []const u8) !void {
    // Build a Reader over the raw gzipped bytes.
    var raw_reader: std.Io.Reader = .fixed(data);
    // Decompress window: gzip requires flate.max_window_len bytes.
    var decomp_buf: [std.compress.flate.max_window_len]u8 = undefined;
    var decomp: std.compress.flate.Decompress = .init(
        &raw_reader,
        .gzip,
        &decomp_buf,
    );
    // Extract the uncompressed tar stream into dest.
    try std.tar.extract(io, dest, &decomp.reader, .{});
}

fn replaceDepFiles(
    arena: std.mem.Allocator,
    io: Io,
    cwd: *Io.Dir,
    dep: Dep,
    src_prefix: []const u8,
    dep_dir: []const u8,
) !void {
    for (dep.keep) |fname| {
        const src_rel = try std.fs.path.join(arena, &.{ src_prefix, fname });
        const dst_rel = try std.fs.path.join(arena, &.{ dep_dir, fname });

        // Ensure the destination parent directory exists (keep entries may
        // include subdirectory paths like "src/hello.txt").
        if (std.fs.path.dirname(dst_rel)) |parent| {
            try cwd.createDirPath(io, parent);
        }

        const limit: Io.Limit = .limited(256 * 1024 * 1024);
        const bytes = cwd.readFileAlloc(io, src_rel, arena, limit) catch |err| {
            std.debug.print(
                "vendor_sync: cached tree for {s} is missing expected file '{s}{s}': {s}\n",
                .{ dep.name, dep.strip_prefix, fname, @errorName(err) },
            );
            std.process.exit(1);
        };
        try cwd.writeFile(io, .{ .sub_path = dst_rel, .data = bytes });
    }
}

fn writeStamp(
    arena: std.mem.Allocator,
    io: Io,
    cwd: *Io.Dir,
    init: *const std.process.Init,
    dep: Dep,
    actual_sha: []const u8,
    dep_dir_rel: []const u8,
) !void {
    const fetched_at = isoUtcNow(arena, init) catch "unknown";
    const stamp_rel = try std.fs.path.join(arena, &.{ dep_dir_rel, "VENDOR.toml" });
    const body = try std.fmt.allocPrint(arena,
        \\# Auto-generated by tools/vendor_sync.zig. Do not edit by hand.
        \\# Regenerate via `zig build vendor-sync`. The `check` step in
        \\# build.zig compares the fields below against vendor/manifest.zon
        \\# and fails the build on drift.
        \\name = "{s}"
        \\version = "{s}"
        \\url = "{s}"
        \\sha256 = "{s}"
        \\fetched_at = "{s}"
        \\
    , .{ dep.name, dep.version, dep.url, actual_sha, fetched_at });
    try cwd.writeFile(io, .{ .sub_path = stamp_rel, .data = body });
}

/// Hand-rolled TOML reader: VENDOR.toml is mechanically written by this
/// tool and only ever contains `key = "value"` pairs plus comments. We
/// pull `version` and `sha256` out and ignore the rest.
fn parseRecorded(body: []const u8) !Recorded {
    var version: ?[]const u8 = null;
    var sha: ?[]const u8 = null;

    var it = std.mem.splitScalar(u8, body, '\n');
    while (it.next()) |raw_line| {
        const line = std.mem.trim(u8, raw_line, " \t\r");
        if (line.len == 0 or line[0] == '#') continue;
        const eq = std.mem.indexOfScalar(u8, line, '=') orelse continue;
        const key = std.mem.trim(u8, line[0..eq], " \t");
        const rhs = std.mem.trim(u8, line[eq + 1 ..], " \t");
        if (rhs.len < 2 or rhs[0] != '"' or rhs[rhs.len - 1] != '"') continue;
        const val = rhs[1 .. rhs.len - 1];
        if (std.mem.eql(u8, key, "version")) version = val;
        if (std.mem.eql(u8, key, "sha256")) sha = val;
    }

    return .{
        .version = version orelse return error.MissingVersion,
        .sha256 = sha orelse return error.MissingSha,
    };
}

/// `date -u +%Y-%m-%dT%H:%M:%SZ` — shelling out matches the pattern
/// already used in build.zig for git metadata, and avoids wrestling
/// with the in-flux Io.Timestamp surface.
fn isoUtcNow(arena: std.mem.Allocator, init: *const std.process.Init) ![]const u8 {
    const result = try std.process.run(arena, init.io, .{
        .argv = &.{ "date", "-u", "+%Y-%m-%dT%H:%M:%SZ" },
    });
    if (result.term != .exited or result.term.exited != 0) return error.DateFailed;
    return std.mem.trim(u8, result.stdout, " \t\n\r");
}
