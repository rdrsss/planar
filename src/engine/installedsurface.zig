//! Installed vendor-projection status and manifest-owned repair.
//!
//! The versioned install manifest is the only ownership authority. Directory
//! discovery is used solely to label destination-only entries `unmanaged`;
//! it never promotes such entries into the managed repair set.

const std = @import("std");

pub const supported_vendors = [_][]const u8{ "claude", "codex", "copilot" };
pub const manifest_version: u32 = 1;

pub const State = enum { fresh, stale, missing, unmanaged };
pub const ManifestState = enum { current, missing, legacy, invalid, unsupported };
pub const VendorState = enum { selected, unselected };

const Manifest = struct {
    version: u32,
    build_id: []const u8,
    install_mode: []const u8,
    vendors: []const []const u8,
    projections: []const ManifestRow,
};

const ManifestRow = struct {
    vendor: []const u8,
    kind: []const u8,
    name: []const u8,
    staged_path: []const u8,
    installed_path: []const u8,
    install_kind: []const u8,
    source_digest: []const u8,
    projection_digest: []const u8,
};

pub const Options = struct {
    planar_home: []const u8,
    home: []const u8,
    codex_home: []const u8,
    vendor: ?[]const u8 = null,
};

pub const VendorStatus = struct {
    vendor: []const u8,
    status: VendorState,
    managed_count: usize,
};

pub const ProjectionStatus = struct {
    vendor: []const u8,
    kind: []const u8,
    name: []const u8,
    staged_path: []const u8,
    installed_path: []const u8,
    install_kind: []const u8,
    status: State,
    reason: []const u8,
    repair_command: ?[]const u8,
};

pub const Summary = struct {
    fresh: usize = 0,
    stale: usize = 0,
    missing: usize = 0,
    unmanaged: usize = 0,
    unselected_vendors: usize = 0,
};

pub const StatusResult = struct {
    manifest_status: ManifestState,
    manifest_version: ?u32,
    build_id: ?[]const u8,
    install_mode: ?[]const u8,
    manifest_path: []const u8,
    reason: ?[]const u8,
    repair_command: ?[]const u8,
    vendors: []const VendorStatus,
    projections: []const ProjectionStatus,
    summary: Summary,
    _arena: std.heap.ArenaAllocator,

    pub fn deinit(self: *StatusResult) void {
        self._arena.deinit();
    }
};

pub fn status(backing: std.mem.Allocator, opts: Options) !StatusResult {
    if (opts.planar_home.len == 0 or opts.home.len == 0 or opts.codex_home.len == 0) return error.InvalidInput;
    if (opts.vendor) |vendor| if (!isVendor(vendor)) return error.InvalidVendor;

    var arena = std.heap.ArenaAllocator.init(backing);
    errdefer arena.deinit();
    const a = arena.allocator();
    const manifest_path = try std.fs.path.join(a, &.{ opts.planar_home, "install-manifest.json" });
    const bootstrap = try bootstrapCommand(a, opts.planar_home);

    const raw = std.Io.Dir.cwd().readFileAlloc(fsIo(), manifest_path, a, std.Io.Limit.limited(16 * 1024 * 1024)) catch |e| switch (e) {
        error.FileNotFound => {
            const stamp_path = try std.fs.path.join(a, &.{ opts.planar_home, ".planar-install" });
            const state: ManifestState = if (pathExists(stamp_path)) .legacy else .missing;
            const reason = if (state == .legacy)
                "legacy ownership stamp exists but the versioned install manifest is missing"
            else
                "install manifest is missing";
            return bootstrapResult(arena, manifest_path, bootstrap, state, reason, opts.vendor);
        },
        else => return e,
    };
    const probe = std.json.parseFromSliceLeaky(struct { version: u32 }, a, raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch return bootstrapResult(arena, manifest_path, bootstrap, .invalid, "install manifest is invalid", opts.vendor);
    if (probe.version != manifest_version) {
        return bootstrapResult(arena, manifest_path, bootstrap, .unsupported, "install manifest version is unsupported", opts.vendor);
    }
    const manifest = std.json.parseFromSliceLeaky(Manifest, a, raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = false,
    }) catch return bootstrapResult(arena, manifest_path, bootstrap, .invalid, "install manifest is invalid", opts.vendor);
    if (!validManifest(manifest)) {
        return bootstrapResult(arena, manifest_path, bootstrap, .invalid, "install manifest is invalid", opts.vendor);
    }

    var vendors: std.ArrayList(VendorStatus) = .empty;
    var projections: std.ArrayList(ProjectionStatus) = .empty;
    var summary: Summary = .{};
    for (supported_vendors) |vendor| {
        if (opts.vendor) |filter| if (!std.mem.eql(u8, filter, vendor)) continue;
        const selected = contains(manifest.vendors, vendor);
        var count: usize = 0;
        if (selected) for (manifest.projections) |row| {
            if (std.mem.eql(u8, row.vendor, vendor)) count += 1;
        };
        try vendors.append(a, .{ .vendor = vendor, .status = if (selected) .selected else .unselected, .managed_count = count });
        if (!selected) summary.unselected_vendors += 1;
    }

    for (manifest.projections) |row| {
        if (opts.vendor) |filter| if (!std.mem.eql(u8, filter, row.vendor)) continue;
        const classified = try classifyRow(a, row);
        try projections.append(a, classified);
        increment(&summary, classified.status);
    }

    // Destination-only entries are visible, but absence from the manifest is
    // never treated as evidence that Planar owns or may repair them.
    for (vendors.items) |vendor| {
        if (vendor.status == .unselected) continue;
        try discoverUnmanaged(a, opts, manifest, vendor.vendor, &projections, &summary);
    }
    std.mem.sort(ProjectionStatus, projections.items, {}, lessProjection);

    var requires_bootstrap = false;
    for (projections.items) |projection| {
        if ((projection.status == .stale or projection.status == .missing) and projection.repair_command == null) {
            requires_bootstrap = true;
            break;
        }
    }
    const repair_command: ?[]const u8 = if (summary.stale + summary.missing == 0)
        null
    else if (requires_bootstrap)
        bootstrap
    else if (opts.vendor) |vendor|
        try std.fmt.allocPrint(a, "planar skills repair --vendor {s} --apply", .{vendor})
    else
        "planar skills repair --apply";

    return .{
        .manifest_status = .current,
        .manifest_version = manifest.version,
        .build_id = manifest.build_id,
        .install_mode = manifest.install_mode,
        .manifest_path = manifest_path,
        .reason = null,
        .repair_command = repair_command,
        .vendors = try vendors.toOwnedSlice(a),
        .projections = try projections.toOwnedSlice(a),
        .summary = summary,
        ._arena = arena,
    };
}

fn bootstrapResult(
    arena_in: std.heap.ArenaAllocator,
    manifest_path: []const u8,
    bootstrap: []const u8,
    manifest_state: ManifestState,
    reason: []const u8,
    vendor_filter: ?[]const u8,
) StatusResult {
    var arena = arena_in;
    const a = arena.allocator();
    var vendors: std.ArrayList(VendorStatus) = .empty;
    var summary: Summary = .{};
    for (supported_vendors) |vendor| {
        if (vendor_filter) |filter| if (!std.mem.eql(u8, filter, vendor)) continue;
        vendors.append(a, .{ .vendor = vendor, .status = .unselected, .managed_count = 0 }) catch unreachable;
        summary.unselected_vendors += 1;
    }
    return .{
        .manifest_status = manifest_state,
        .manifest_version = null,
        .build_id = null,
        .install_mode = null,
        .manifest_path = manifest_path,
        .reason = reason,
        .repair_command = bootstrap,
        .vendors = vendors.toOwnedSlice(a) catch unreachable,
        .projections = &.{},
        .summary = summary,
        ._arena = arena,
    };
}

fn validManifest(manifest: Manifest) bool {
    if (manifest.build_id.len == 0) return false;
    if (!std.mem.eql(u8, manifest.install_mode, "copy") and !std.mem.eql(u8, manifest.install_mode, "link")) return false;
    for (manifest.vendors, 0..) |vendor, i| {
        if (!isVendor(vendor)) return false;
        for (manifest.vendors[0..i]) |prior| if (std.mem.eql(u8, prior, vendor)) return false;
    }
    for (manifest.projections, 0..) |row, i| {
        if (!isVendor(row.vendor) or !contains(manifest.vendors, row.vendor)) return false;
        if ((!std.mem.eql(u8, row.kind, "skill") and !std.mem.eql(u8, row.kind, "agent")) or row.name.len == 0) return false;
        if (row.staged_path.len == 0 or row.installed_path.len == 0) return false;
        if (!std.mem.eql(u8, row.install_kind, "copy") and !std.mem.eql(u8, row.install_kind, "link")) return false;
        if (!isDigest(row.source_digest) or !isDigest(row.projection_digest)) return false;
        for (manifest.projections[0..i]) |prior| {
            if (std.mem.eql(u8, prior.installed_path, row.installed_path)) return false;
            if (std.mem.eql(u8, prior.vendor, row.vendor) and
                std.mem.eql(u8, prior.kind, row.kind) and
                std.mem.eql(u8, prior.name, row.name)) return false;
        }
    }
    return true;
}

fn classifyRow(a: std.mem.Allocator, row: ManifestRow) !ProjectionStatus {
    const repair_cmd = try std.fmt.allocPrint(a, "planar skills repair {s} --vendor {s} --apply", .{ row.name, row.vendor });
    const base: ProjectionStatus = .{
        .vendor = row.vendor,
        .kind = row.kind,
        .name = row.name,
        .staged_path = row.staged_path,
        .installed_path = row.installed_path,
        .install_kind = row.install_kind,
        .status = .stale,
        .reason = "staged projection is unavailable",
        .repair_command = repair_cmd,
    };
    const staged = readProjection(a, row.staged_path) catch |e| switch (e) {
        error.FileNotFound => return base,
        error.InvalidProjectionDigest => {
            var out = base;
            out.reason = "staged projection has invalid digest metadata; reinstall required";
            out.repair_command = null;
            return out;
        },
        else => return e,
    };
    defer a.free(staged.bytes);
    if (!std.mem.eql(u8, staged.source_digest, row.source_digest) or
        !std.mem.eql(u8, staged.projection_digest, row.projection_digest))
    {
        var out = base;
        out.reason = "staged projection digests differ from the install manifest; reinstall required";
        out.repair_command = null;
        return out;
    }

    var link_buf: [std.fs.max_path_bytes]u8 = undefined;
    const link_len = std.Io.Dir.cwd().readLink(fsIo(), row.installed_path, &link_buf) catch null;
    const installed = readProjection(a, row.installed_path) catch |e| switch (e) {
        error.FileNotFound => {
            var out = base;
            out.status = .missing;
            out.reason = "managed installed projection is missing";
            return out;
        },
        error.InvalidProjectionDigest => {
            var out = base;
            out.reason = "managed installed projection has invalid digest metadata";
            return out;
        },
        error.IsDir => {
            var out = base;
            out.reason = "managed installed destination is a directory and cannot be replaced safely";
            return out;
        },
        else => return e,
    };
    defer a.free(installed.bytes);

    if (std.mem.eql(u8, row.install_kind, "link")) {
        if (link_len == null or !std.mem.eql(u8, link_buf[0..link_len.?], row.staged_path)) {
            var out = base;
            out.reason = "managed link target differs from the install manifest";
            return out;
        }
    } else if (link_len != null or !std.mem.eql(u8, staged.bytes, installed.bytes)) {
        var out = base;
        out.reason = "managed installed bytes differ from the staged projection";
        return out;
    }
    if (!std.mem.eql(u8, installed.source_digest, row.source_digest) or
        !std.mem.eql(u8, installed.projection_digest, row.projection_digest))
    {
        var out = base;
        out.reason = "managed installed digests differ from the install manifest";
        return out;
    }
    var out = base;
    out.status = .fresh;
    out.reason = "manifest, staged projection, and installed projection agree";
    out.repair_command = null;
    return out;
}

const Projection = struct { bytes: []u8, source_digest: []const u8, projection_digest: []const u8 };

fn readProjection(a: std.mem.Allocator, path: []const u8) !Projection {
    const bytes = try std.Io.Dir.cwd().readFileAlloc(fsIo(), path, a, std.Io.Limit.limited(16 * 1024 * 1024));
    errdefer a.free(bytes);
    const source = digestField(bytes, "x-planar-source-digest") orelse return error.InvalidProjectionDigest;
    const projection = digestField(bytes, "x-planar-projection-digest") orelse return error.InvalidProjectionDigest;
    return .{ .bytes = bytes, .source_digest = source, .projection_digest = projection };
}

fn digestField(bytes: []const u8, key: []const u8) ?[]const u8 {
    var lines = std.mem.splitScalar(u8, bytes, '\n');
    const first_raw = lines.next() orelse return null;
    const first = std.mem.trim(u8, first_raw, " \t\r");
    const markdown = std.mem.eql(u8, first, "---");
    if (!markdown and !std.mem.startsWith(u8, first, "#")) return null;
    if (!markdown) {
        if (digestFromLine(first, key, true)) |value| return value;
    }
    while (lines.next()) |raw_line| {
        const line = std.mem.trim(u8, raw_line, " \t\r");
        if (markdown) {
            if (std.mem.eql(u8, line, "---")) return null;
            if (digestFromLine(line, key, false)) |value| return value;
        } else {
            if (line.len == 0) continue;
            if (!std.mem.startsWith(u8, line, "#")) return null;
            if (digestFromLine(line, key, true)) |value| return value;
        }
    }
    return null;
}

fn digestFromLine(raw_line: []const u8, key: []const u8, comment: bool) ?[]const u8 {
    var line = raw_line;
    if (comment) {
        if (!std.mem.startsWith(u8, line, "#")) return null;
        line = std.mem.trimStart(u8, line[1..], " \t");
    }
    if (!std.mem.startsWith(u8, line, key)) return null;
    line = line[key.len..];
    if (line.len == 0 or line[0] != ':') return null;
    const value = std.mem.trim(u8, line[1..], " \t\r");
    return if (isDigest(value)) value else null;
}

fn discoverUnmanaged(
    a: std.mem.Allocator,
    opts: Options,
    manifest: Manifest,
    vendor: []const u8,
    projections: *std.ArrayList(ProjectionStatus),
    summary: *Summary,
) !void {
    const roots = try vendorRoots(a, opts, vendor);
    for (roots) |root| {
        var dir = std.Io.Dir.cwd().openDir(fsIo(), root.path, .{ .iterate = true }) catch |e| switch (e) {
            error.FileNotFound => continue,
            else => return e,
        };
        defer dir.close(fsIo());
        var it = dir.iterate();
        while (try it.next(fsIo())) |entry| {
            if (std.mem.eql(u8, entry.name, ".") or std.mem.eql(u8, entry.name, "..")) continue;
            const installed = if (root.directory_shape)
                try std.fs.path.join(a, &.{ root.path, entry.name, "SKILL.md" })
            else
                try std.fs.path.join(a, &.{ root.path, entry.name });
            if (manifestOwns(manifest, installed)) continue;
            if (root.directory_shape and !pathExists(installed)) continue;
            const name = projectionName(a, entry.name);
            try projections.append(a, .{
                .vendor = vendor,
                .kind = root.kind,
                .name = name,
                .staged_path = "",
                .installed_path = installed,
                .install_kind = "unmanaged",
                .status = .unmanaged,
                .reason = "destination entry has no install-manifest row",
                .repair_command = null,
            });
            summary.unmanaged += 1;
        }
    }
}

const Root = struct { path: []const u8, kind: []const u8, directory_shape: bool };

fn vendorRoots(a: std.mem.Allocator, opts: Options, vendor: []const u8) ![]const Root {
    const roots = try a.alloc(Root, 2);
    if (std.mem.eql(u8, vendor, "claude")) {
        roots[0] = .{ .path = try std.fs.path.join(a, &.{ opts.home, ".claude", "commands" }), .kind = "skill", .directory_shape = false };
        roots[1] = .{ .path = try std.fs.path.join(a, &.{ opts.home, ".claude", "agents" }), .kind = "agent", .directory_shape = false };
    } else if (std.mem.eql(u8, vendor, "codex")) {
        roots[0] = .{ .path = try std.fs.path.join(a, &.{ opts.codex_home, "skills" }), .kind = "skill", .directory_shape = true };
        roots[1] = .{ .path = try std.fs.path.join(a, &.{ opts.codex_home, "agents" }), .kind = "agent", .directory_shape = false };
    } else {
        roots[0] = .{ .path = try std.fs.path.join(a, &.{ opts.home, ".copilot", "skills" }), .kind = "skill", .directory_shape = true };
        roots[1] = .{ .path = try std.fs.path.join(a, &.{ opts.home, ".copilot", "agents" }), .kind = "agent", .directory_shape = false };
    }
    return roots;
}

fn manifestOwns(manifest: Manifest, path: []const u8) bool {
    for (manifest.projections) |row| if (std.mem.eql(u8, row.installed_path, path)) return true;
    return false;
}

fn projectionName(a: std.mem.Allocator, entry_name: []const u8) []const u8 {
    var name = entry_name;
    for ([_][]const u8{ ".agent.md", ".toml", ".md" }) |suffix| {
        if (std.mem.endsWith(u8, name, suffix)) {
            name = name[0 .. name.len - suffix.len];
            break;
        }
    }
    return a.dupe(u8, name) catch unreachable;
}

pub const RepairOptions = struct {
    status: Options,
    names: []const []const u8 = &.{},
    apply: bool = false,
};

pub const RepairAction = struct {
    vendor: []const u8,
    kind: []const u8,
    name: []const u8,
    installed_path: []const u8,
    before: State,
    action: []const u8,
    post_status: State,
    error_name: ?[]const u8,
    next_action: ?[]const u8,
};

pub const RepairResult = struct {
    mode: []const u8,
    outcome: []const u8,
    attempted: usize,
    applied: usize,
    skipped: usize,
    failed: usize,
    actions: []const RepairAction,
    next_action: ?[]const u8,
    manifest_status: ManifestState,
    _arena: std.heap.ArenaAllocator,

    pub fn deinit(self: *RepairResult) void {
        self._arena.deinit();
    }
};

pub fn repair(backing: std.mem.Allocator, opts: RepairOptions) !RepairResult {
    var before = try status(backing, opts.status);
    defer before.deinit();
    var arena = std.heap.ArenaAllocator.init(backing);
    errdefer arena.deinit();
    const a = arena.allocator();
    if (before.manifest_status != .current) {
        return .{
            .mode = if (opts.apply) "apply" else "preview",
            .outcome = "error",
            .attempted = 0,
            .applied = 0,
            .skipped = 0,
            .failed = 1,
            .actions = &.{},
            .next_action = try a.dupe(u8, before.repair_command.?),
            .manifest_status = before.manifest_status,
            ._arena = arena,
        };
    }
    for (opts.names) |name| {
        var found = false;
        for (before.projections) |row| {
            if (row.status != .unmanaged and std.mem.eql(u8, row.name, name)) found = true;
        }
        if (!found) return error.UnmanagedOrUnknownProjection;
    }

    var actions: std.ArrayList(RepairAction) = .empty;
    var attempted: usize = 0;
    var applied: usize = 0;
    var skipped: usize = 0;
    var failed: usize = 0;
    for (before.projections) |row| {
        if (row.status == .unmanaged or !selectedName(opts.names, row.name)) continue;
        if (row.status == .fresh) {
            skipped += 1;
            try actions.append(a, try cloneAction(a, row, "unchanged", .fresh, null, null));
            continue;
        }
        attempted += 1;
        if (!opts.apply) {
            try actions.append(a, try cloneAction(a, row, "would-repair", row.status, null, row.repair_command));
            continue;
        }
        if (row.repair_command == null) {
            failed += 1;
            try actions.append(a, try cloneAction(a, row, "failed", row.status, "StagedAuthorityMismatch", try bootstrapCommand(a, opts.status.planar_home)));
            continue;
        }
        applyOne(row) catch |e| {
            failed += 1;
            try actions.append(a, try cloneAction(a, row, "failed", row.status, @errorName(e), row.repair_command));
            continue;
        };
        applied += 1;
        try actions.append(a, try cloneAction(a, row, "repaired", .fresh, null, null));
    }
    const outcome = if (failed > 0 and applied > 0) "partial" else if (failed > 0) "error" else "ok";
    const next = if (failed > 0) "planar skills status" else if (opts.apply) "planar skills status" else "planar skills repair --apply";
    return .{
        .mode = if (opts.apply) "apply" else "preview",
        .outcome = outcome,
        .attempted = attempted,
        .applied = applied,
        .skipped = skipped,
        .failed = failed,
        .actions = try actions.toOwnedSlice(a),
        .next_action = next,
        .manifest_status = .current,
        ._arena = arena,
    };
}

fn cloneAction(a: std.mem.Allocator, row: ProjectionStatus, action: []const u8, post: State, err: ?[]const u8, next: ?[]const u8) !RepairAction {
    return .{
        .vendor = try a.dupe(u8, row.vendor),
        .kind = try a.dupe(u8, row.kind),
        .name = try a.dupe(u8, row.name),
        .installed_path = try a.dupe(u8, row.installed_path),
        .before = row.status,
        .action = action,
        .post_status = post,
        .error_name = if (err) |v| try a.dupe(u8, v) else null,
        .next_action = if (next) |v| try a.dupe(u8, v) else null,
    };
}

fn applyOne(row: ProjectionStatus) !void {
    const parent = std.fs.path.dirname(row.installed_path) orelse return error.InvalidInput;
    try std.Io.Dir.cwd().createDirPath(fsIo(), parent);
    var tmp_buf: [std.fs.max_path_bytes]u8 = undefined;
    const tmp = try std.fmt.bufPrint(&tmp_buf, "{s}.planar-repair-tmp", .{row.installed_path});
    std.Io.Dir.cwd().deleteFile(fsIo(), tmp) catch {};
    errdefer std.Io.Dir.cwd().deleteFile(fsIo(), tmp) catch {};
    if (std.mem.eql(u8, row.install_kind, "link")) {
        try std.Io.Dir.cwd().symLink(fsIo(), row.staged_path, tmp, .{});
    } else {
        const body = try std.Io.Dir.cwd().readFileAlloc(fsIo(), row.staged_path, std.heap.smp_allocator, std.Io.Limit.limited(16 * 1024 * 1024));
        defer std.heap.smp_allocator.free(body);
        try std.Io.Dir.cwd().writeFile(fsIo(), .{ .sub_path = tmp, .data = body });
    }
    if (pathIsDirectory(row.installed_path)) return error.UnsafeDestination;
    std.Io.Dir.cwd().rename(tmp, std.Io.Dir.cwd(), row.installed_path, fsIo()) catch |e| switch (e) {
        error.AccessDenied, error.PermissionDenied, error.IsDir, error.NotDir => {
            std.Io.Dir.cwd().deleteFile(fsIo(), row.installed_path) catch {};
            try std.Io.Dir.cwd().rename(tmp, std.Io.Dir.cwd(), row.installed_path, fsIo());
        },
        else => return e,
    };
    const verified = try readProjection(std.heap.smp_allocator, row.installed_path);
    defer std.heap.smp_allocator.free(verified.bytes);
    const staged = try readProjection(std.heap.smp_allocator, row.staged_path);
    defer std.heap.smp_allocator.free(staged.bytes);
    if (!std.mem.eql(u8, staged.source_digest, verified.source_digest) or
        !std.mem.eql(u8, staged.projection_digest, verified.projection_digest)) return error.VerificationFailed;
}

fn selectedName(names: []const []const u8, name: []const u8) bool {
    if (names.len == 0) return true;
    return contains(names, name);
}

fn bootstrapCommand(a: std.mem.Allocator, planar_home: []const u8) ![]const u8 {
    // Paths are JSON-safe elsewhere; this shell form also handles apostrophes.
    var out: std.ArrayList(u8) = .empty;
    try out.appendSlice(a, "./install.sh --prefix '");
    for (planar_home) |ch| {
        if (ch == '\'') try out.appendSlice(a, "'\\''") else try out.append(a, ch);
    }
    try out.append(a, '\'');
    return out.toOwnedSlice(a);
}

fn isVendor(value: []const u8) bool {
    return contains(&supported_vendors, value);
}

fn contains(values: []const []const u8, needle: []const u8) bool {
    for (values) |value| if (std.mem.eql(u8, value, needle)) return true;
    return false;
}

fn isDigest(value: []const u8) bool {
    if (value.len != 64) return false;
    for (value) |ch| if (!std.ascii.isDigit(ch) and !(ch >= 'a' and ch <= 'f')) return false;
    return true;
}

fn increment(summary: *Summary, state: State) void {
    switch (state) {
        .fresh => summary.fresh += 1,
        .stale => summary.stale += 1,
        .missing => summary.missing += 1,
        .unmanaged => summary.unmanaged += 1,
    }
}

fn lessProjection(_: void, lhs: ProjectionStatus, rhs: ProjectionStatus) bool {
    const vendor_order = std.mem.order(u8, lhs.vendor, rhs.vendor);
    if (vendor_order != .eq) return vendor_order == .lt;
    const kind_order = std.mem.order(u8, lhs.kind, rhs.kind);
    if (kind_order != .eq) return kind_order == .lt;
    const name_order = std.mem.order(u8, lhs.name, rhs.name);
    if (name_order != .eq) return name_order == .lt;
    return std.mem.lessThan(u8, lhs.installed_path, rhs.installed_path);
}

fn pathExists(path: []const u8) bool {
    std.Io.Dir.cwd().access(fsIo(), path, .{}) catch return false;
    return true;
}

fn pathIsDirectory(path: []const u8) bool {
    const stat = std.Io.Dir.cwd().statFile(fsIo(), path, .{}) catch return false;
    return stat.kind == .directory;
}

fn fsIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}

test "status classifies managed copy and unmanaged extension" {
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &tmp.sub_path });
    defer gpa.free(root);
    const planar_home = try std.fs.path.join(gpa, &.{ root, ".planar" });
    defer gpa.free(planar_home);
    const staged = try std.fs.path.join(gpa, &.{ planar_home, "codex-skills", "pl-a", "SKILL.md" });
    defer gpa.free(staged);
    const installed = try std.fs.path.join(gpa, &.{ root, ".codex", "skills", "pl-a", "SKILL.md" });
    defer gpa.free(installed);
    const unmanaged = try std.fs.path.join(gpa, &.{ root, ".codex", "skills", "mine", "SKILL.md" });
    defer gpa.free(unmanaged);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, std.fs.path.dirname(staged).?);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, std.fs.path.dirname(installed).?);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, std.fs.path.dirname(unmanaged).?);
    const digest = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const body = "---\nx-planar-source-digest: " ++ digest ++ "\nx-planar-projection-digest: " ++ digest ++ "\n---\nbody\n";
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = staged, .data = body });
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = installed, .data = body });
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = unmanaged, .data = "personal\n" });
    const manifest_path = try std.fs.path.join(gpa, &.{ planar_home, "install-manifest.json" });
    defer gpa.free(manifest_path);
    const manifest = try std.fmt.allocPrint(gpa, "{{\"version\":1,\"build_id\":\"test\",\"install_mode\":\"copy\",\"vendors\":[\"codex\"],\"projections\":[{{\"vendor\":\"codex\",\"kind\":\"skill\",\"name\":\"pl-a\",\"staged_path\":\"{s}\",\"installed_path\":\"{s}\",\"install_kind\":\"copy\",\"source_digest\":\"{s}\",\"projection_digest\":\"{s}\"}}]}}", .{ staged, installed, digest, digest });
    defer gpa.free(manifest);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = manifest_path, .data = manifest });
    const codex_home = try std.fs.path.join(gpa, &.{ root, ".codex" });
    defer gpa.free(codex_home);
    var result = try status(gpa, .{ .planar_home = planar_home, .home = root, .codex_home = codex_home, .vendor = "codex" });
    defer result.deinit();
    try std.testing.expectEqual(@as(usize, 1), result.summary.fresh);
    try std.testing.expectEqual(@as(usize, 1), result.summary.unmanaged);
}

test "missing manifest is one bootstrap result" {
    const gpa = std.testing.allocator;
    var result = try status(gpa, .{ .planar_home = "/tmp/no-such-planar-prefix-for-status-test", .home = "/tmp", .codex_home = "/tmp/.codex" });
    defer result.deinit();
    try std.testing.expectEqual(ManifestState.missing, result.manifest_status);
    try std.testing.expectEqual(@as(usize, 0), result.projections.len);
    try std.testing.expect(std.mem.startsWith(u8, result.repair_command.?, "./install.sh --prefix"));
}

test "legacy invalid and unsupported manifests stay aggregate bootstrap states" {
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &tmp.sub_path });
    defer gpa.free(root);
    const planar_home = try std.fs.path.join(gpa, &.{ root, ".planar" });
    defer gpa.free(planar_home);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, planar_home);
    const stamp = try std.fs.path.join(gpa, &.{ planar_home, ".planar-install" });
    defer gpa.free(stamp);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = stamp, .data = "planar-install 1\n" });

    var legacy = try status(gpa, .{ .planar_home = planar_home, .home = root, .codex_home = root });
    defer legacy.deinit();
    try std.testing.expectEqual(ManifestState.legacy, legacy.manifest_status);
    try std.testing.expectEqual(@as(usize, 0), legacy.projections.len);

    const manifest_path = try std.fs.path.join(gpa, &.{ planar_home, "install-manifest.json" });
    defer gpa.free(manifest_path);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = manifest_path, .data = "not json\n" });
    var invalid = try status(gpa, .{ .planar_home = planar_home, .home = root, .codex_home = root });
    defer invalid.deinit();
    try std.testing.expectEqual(ManifestState.invalid, invalid.manifest_status);

    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = manifest_path, .data = "{\"version\":2}\n" });
    var unsupported = try status(gpa, .{ .planar_home = planar_home, .home = root, .codex_home = root });
    defer unsupported.deinit();
    try std.testing.expectEqual(ManifestState.unsupported, unsupported.manifest_status);
}
