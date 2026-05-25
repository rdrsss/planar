//! handlers/task/link — `planar task link <task-id> <ref> --relationship <rel>`
//!
//! Creates an entity_links row from a task to another entity. The from_kind
//! is always 'task'; the to-kind and to-id come from the <ref> positional
//! ("kind:integer-id" form only — slug refs not supported per Go parity).
//!
//! Link verbs are documented UNGUARDED (CLAUDE.md §Cross-scope guard). No
//! scope_guard.check call is made here. --scope is accepted for CLI parity.
//!
//! JSON shape (mirrors Go's taskLinkJSON):
//!   { "ok": true, "id": <link-id>, "task_id": <n>, "to_kind": "...",
//!     "to_id": <n>, "relationship": "..." }

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");

const EntityLinkResult = struct {
    ok: bool,
    id: i64,
    task_id: i64,
    to_kind: []const u8,
    to_id: i64,
    relationship: []const u8,
};

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "task", "link" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const task_id = std.fmt.parseInt(i64, args.task_id, 10) catch
        exit.die(ctx, error.InvalidInput, "task id must be an integer, got '{s}'", .{args.task_id});

    const rel_text = args.relationship orelse
        exit.die(ctx, error.InvalidInput, "--relationship is required", .{});

    const relationship = engine.entitylink.Relationship.fromText(rel_text) orelse
        exit.die(ctx, error.InvalidInput, "unknown relationship '{s}'", .{rel_text});

    const parsed_ref = engine.entitylink.parseRef(args.ref) catch |e| switch (e) {
        error.InvalidRef => exit.die(ctx, error.InvalidInput, "invalid ref '{s}': expected kind:integer-id", .{args.ref}),
        else => return e,
    };
    const to_kind = switch (parsed_ref) {
        .id => |r| r.kind,
        .slug => exit.die(ctx, error.InvalidInput, "slug refs are not supported; use kind:integer-id (e.g. plan:42)", .{}),
    };
    const to_id = switch (parsed_ref) {
        .id => |r| r.id,
        .slug => unreachable,
    };

    _ = args.scope;

    const link = engine.entitylink.add(d, ctx.allocator, .{
        .from_kind = .task,
        .from_id = task_id,
        .to_kind = to_kind,
        .to_id = to_id,
        .relationship = relationship,
    }) catch |e| switch (e) {
        error.LinkExists => exit.die(ctx, e, "link task:{d} -> {s}:{d} [{s}] already exists", .{ task_id, to_kind.toText(), to_id, rel_text }),
        error.UnsupportedScope => exit.die(ctx, e, "scoped entity links not yet supported (M3)", .{}),
        else => exit.die(ctx, e, "task link: {s}", .{@errorName(e)}),
    };
    defer engine.entitylink.deinit(link, ctx.allocator);

    if (args.json) {
        const result = EntityLinkResult{
            .ok = true,
            .id = link.id,
            .task_id = task_id,
            .to_kind = to_kind.toText(),
            .to_id = to_id,
            .relationship = rel_text,
        };
        try std.json.Stringify.value(result, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        try ctx.stdout.print("linked task:{d} -> {s}:{d}  [{s}]  (link id: {d})\n", .{
            task_id, to_kind.toText(), to_id, rel_text, link.id,
        });
    }
}
