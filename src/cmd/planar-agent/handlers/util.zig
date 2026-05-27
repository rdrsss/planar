//! handlers/util — shared helpers for planar-agent verb handlers.
//!
//! - `parseEntityRef` parses `task:<id>` / `plan:<id>` / `plan_step:<id>`
//!   for `planar-agent claim --entity <ref>`. Strict refusal of any
//!   other prefix.
//! - `resolveLocality` wires the `--repo-root` and `--no-locality-probe`
//!   flag pair to the engine's `locality.probe`. Returns `Locality.skipped`
//!   when the probe is skipped or fails to determine a repo root.
//! - `currentDirAlloc` resolves the process cwd so the locality probe
//!   can fall back to it when the operator didn't supply `--repo-root`.

const std = @import("std");
const engine = @import("engine");
const runtime = @import("runtime");

const types = engine.runtime.agentactivity.types;
const locality_engine = engine.runtime.agentactivity.locality;

pub const EntityRef = struct {
    kind: types.EntityKind,
    id: i64,
};

pub const ParseError = error{
    InvalidEntityRef,
    UnsupportedEntityKind,
};

/// Parse `task:<id>` / `plan:<id>` / `plan_step:<id>`. Any other prefix
/// is `UnsupportedEntityKind`. Missing colon / non-integer id is
/// `InvalidEntityRef`. Strict-by-design — the spec § "claim --entity
/// <ref> parser" pins the contract.
pub fn parseEntityRef(s: []const u8) ParseError!EntityRef {
    const colon = std.mem.indexOfScalar(u8, s, ':') orelse return ParseError.InvalidEntityRef;
    const kind_str = s[0..colon];
    const id_str = s[colon + 1 ..];
    if (id_str.len == 0) return ParseError.InvalidEntityRef;
    const id = std.fmt.parseInt(i64, id_str, 10) catch return ParseError.InvalidEntityRef;
    const kind = types.EntityKind.fromText(kind_str) orelse return ParseError.UnsupportedEntityKind;
    return .{ .kind = kind, .id = id };
}

/// Resolve locality per the tech-spec contract:
///   --no-locality-probe → return Locality.skipped (all-NULL, dirty=unknown)
///   --repo-root <path>  → probe at that path
///   otherwise           → fall back to process cwd (best-effort; on
///                         failure to determine cwd, return Locality.skipped)
///
/// Per-action-kind defaults are the CALLER's responsibility — they
/// inspect `types.ActionKind.probeDefault(kind)` before invoking this.
/// This helper only resolves the cwd / repo-root path argument.
pub fn resolveLocality(
    ctx: *const runtime.Ctx,
    repo_root_arg: ?[]const u8,
    no_probe: bool,
) types.Locality {
    if (no_probe) return types.Locality.skipped;

    var root_buf: ?[]u8 = null;
    defer if (root_buf) |b| ctx.allocator.free(b);

    const root_path: []const u8 = blk: {
        if (repo_root_arg) |p| break :blk p;
        // Fall back to cwd. Best-effort — failure → skipped.
        const cwd = currentDirAlloc(ctx) catch return types.Locality.skipped;
        root_buf = cwd;
        break :blk cwd;
    };

    const loc = locality_engine.probe(.{
        .allocator = ctx.allocator,
        .io = ctx.io,
        .repo_root = root_path,
    }) catch return types.Locality.skipped;
    return loc;
}

/// Resolve the process cwd as an absolute path. Allocator-owned.
pub fn currentDirAlloc(ctx: *const runtime.Ctx) ![]u8 {
    return try std.Io.Dir.realPathFileAlloc(.cwd(), ctx.io, ".", ctx.allocator);
}
