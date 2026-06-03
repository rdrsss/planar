//! Build-time vendored-deps manager. Reads a declarative
//! `vendor/manifest.zon`, compares each entry against the
//! `vendor/<name>/VENDOR.toml` provenance stamp, and either:
//!
//!   - `check`: exits 0 if every dep's recorded version+sha matches the
//!     manifest. Exits 1 (with an actionable message) on the first
//!     mismatch or missing stamp. Read-only; no network.
//!   - `sync`:  for each dep, fetches the URL, verifies the SHA-256,
//!     extracts the archive, copies the whitelisted files into
//!     `vendor/<name>/`, and writes a fresh `VENDOR.toml`.
//!
//! The build wires `check` as a configure-time fail-fast step so a
//! stale checkout cannot silently compile against the wrong upstream;
//! `sync` runs only when an operator invokes `zig build vendor-sync`.
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
        try cmdSync(arena, io, &init, manifest, vendor_dir_path);
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
    vendor_dir_path: []const u8,
) !void {
    var cwd = Io.Dir.cwd();
    try cwd.createDirPath(io, vendor_dir_path);

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

        // Extract the archive into vendor/<name>/.sync-tmp/, then copy the
        // whitelisted files out and replace the existing dep directory.
        const dep_dir_rel = try std.fs.path.join(arena, &.{ vendor_dir_path, dep.name });
        const tmp_dir_rel = try std.fs.path.join(arena, &.{ vendor_dir_path, dep.name, ".sync-tmp" });
        cwd.deleteTree(io, tmp_dir_rel) catch {};
        try cwd.createDirPath(io, tmp_dir_rel);

        try extractArchive(io, &cwd, dep, data, tmp_dir_rel);
        try replaceDepFiles(arena, io, &cwd, dep, tmp_dir_rel, dep_dir_rel);

        cwd.deleteTree(io, tmp_dir_rel) catch {};

        try writeStamp(arena, io, &cwd, init, dep, actual_sha, dep_dir_rel);
        std.debug.print("vendor_sync: {s} synced (sha={s})\n", .{ dep.name, actual_sha });
    }
}

fn extractArchive(
    io: Io,
    cwd: *Io.Dir,
    dep: Dep,
    data: []const u8,
    tmp_dir_rel: []const u8,
) !void {
    switch (dep.archive) {
        .zip => {
            // std.zip.extract reads via a File.Reader, so stash the bytes in
            // a tmp file we can open back. Keeping the file alongside the
            // extract dir means a single deleteTree on the parent cleans up.
            const blob_rel = try std.fs.path.join(std.heap.page_allocator, &.{ tmp_dir_rel, "archive.zip" });
            // ^ tiny allocation that lives as long as this scope; arena would
            // be cleaner but we'd have to thread it. Keep it local.
            defer std.heap.page_allocator.free(blob_rel);

            {
                var blob = try cwd.createFile(io, blob_rel, .{});
                defer blob.close(io);
                try blob.writeStreamingAll(io, data);
            }

            var blob_ro = try cwd.openFile(io, blob_rel, .{ .mode = .read_only });
            defer blob_ro.close(io);

            var read_buf: [16 * 1024]u8 = undefined;
            var fr = blob_ro.reader(io, &read_buf);

            var dest = try cwd.openDir(io, tmp_dir_rel, .{});
            defer dest.close(io);

            try std.zip.extract(dest, &fr, .{});
        },
        .tar_gz => {
            var dest = try cwd.openDir(io, tmp_dir_rel, .{});
            defer dest.close(io);
            try extractTarGz(io, dest, data);
        },
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
    tmp_dir_rel: []const u8,
    dep_dir_rel: []const u8,
) !void {
    for (dep.keep) |fname| {
        const src_rel = try std.fs.path.join(arena, &.{ tmp_dir_rel, dep.strip_prefix, fname });
        const dst_rel = try std.fs.path.join(arena, &.{ dep_dir_rel, fname });

        const limit: Io.Limit = .limited(256 * 1024 * 1024);
        const bytes = cwd.readFileAlloc(io, src_rel, arena, limit) catch |err| {
            std.debug.print(
                "vendor_sync: archive for {s} is missing expected file '{s}{s}': {s}\n",
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

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

test "extractTarGz: extract fixture into tmpdir and verify files" {
    // Fixture: a pre-built testpkg-1.0.tar.gz containing:
    //   testpkg-1.0/src/hello.txt  ("hello\n")
    //   testpkg-1.0/src/world.txt  ("world\n")
    const fixture = @embedFile("testdata/testpkg-1.0.tar.gz");

    const io = std.testing.io;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    // tmp.dir is already an Io.Dir — pass it directly to extractTarGz.
    try extractTarGz(io, tmp.dir, fixture);

    // Verify the top-level dir was created and files exist with expected content.
    const limit: std.Io.Limit = .limited(256);

    const hello = try tmp.dir.readFileAlloc(io, "testpkg-1.0/src/hello.txt", std.testing.allocator, limit);
    defer std.testing.allocator.free(hello);
    try std.testing.expectEqualStrings("hello\n", hello);

    const world = try tmp.dir.readFileAlloc(io, "testpkg-1.0/src/world.txt", std.testing.allocator, limit);
    defer std.testing.allocator.free(world);
    try std.testing.expectEqualStrings("world\n", world);
}
