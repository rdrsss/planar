const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const common = @import("common.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "local", "link" }, args_ptr);
    const ctx = runtime.current();

    if (args.reconcile) {
        if (args.name != null) {
            exit.die(ctx, error.InvalidInput, "--reconcile takes no positional arguments", .{});
        }
        try runReconcile(ctx, args.dry_run, args.json);
        return;
    }

    const hr = common.resolveHomeAndRoot(ctx) catch |e|
        exit.die(ctx, e, "resolving sandbox root: {s}", .{@errorName(e)});
    defer {
        ctx.allocator.free(hr.home_dir);
        ctx.allocator.free(hr.sandbox_root);
    }

    const walked = engine.local.manifest.walkSandbox(hr.sandbox_root, ctx.allocator) catch |e|
        exit.die(ctx, e, "walking sandbox failed: {s}", .{@errorName(e)});
    defer engine.local.manifest.deinitWalkResult(walked, ctx.allocator);

    if (!args.json) {
        for (walked.walk_errors) |we| {
            try ctx.stdout.print("warning: {s}: {s}\n", .{ we.path, we.message });
        }
    }

    const target_name = args.name;

    var picked: std.ArrayList(engine.local.manifest.SandboxFile) = .empty;
    defer picked.deinit(ctx.allocator);
    for (walked.files) |f| {
        if (target_name) |name| {
            if (!std.mem.eql(u8, f.name, name)) continue;
        }
        try picked.append(ctx.allocator, f);
    }

    if (target_name != null and picked.items.len == 0) {
        exit.die(ctx, error.NotFound, "no sandbox source named \"{s}\" under {s}", .{ target_name.?, hr.sandbox_root });
    }
    if (picked.items.len == 0) {
        try ctx.stdout.print("no sandbox sources to link under {s}\n", .{hr.sandbox_root});
        return;
    }

    var total_linked: usize = 0;
    var total_unchanged: usize = 0;
    var total_skipped: usize = 0;
    var total_dry_run: usize = 0;

    for (picked.items) |f| {
        if (!args.json) {
            const issues = engine.local.manifest.lint(f.frontmatter, f.name, ctx.allocator) catch |e|
                exit.die(ctx, e, "linting {s} failed: {s}", .{ f.name, @errorName(e) });
            defer engine.local.manifest.deinitLint(issues, ctx.allocator);
            for (issues) |issue| {
                try ctx.stdout.print(
                    "lint [{s}] {s}/{s}.{s}: {s}\n",
                    .{ @tagName(issue.severity), @tagName(f.kind), f.name, issue.field, issue.message },
                );
            }
        }

        const linked = engine.local.link.link(
            f,
            .{
                .home_dir = hr.home_dir,
                .dry_run = args.dry_run,
                .vendor_filter = args.vendor,
            },
            ctx.allocator,
        ) catch |e| exit.die(ctx, e, "linking {s} failed: {s}", .{ f.name, @errorName(e) });
        defer engine.local.link.deinitLinkResult(linked, ctx.allocator);

        if (args.json) {
            try common.emitGoLinkJSON(ctx, f, linked);
        } else {
            try ctx.stdout.print("{s} ({s})\n", .{ linked.name, @tagName(linked.kind) });
            for (linked.records) |rec| {
                if (rec.mode) |mode| {
                    try ctx.stdout.print("  {s:<7}  {s} [{s}]  ->  {s}\n", .{ rec.vendor, rec.action, @tagName(mode), rec.target_path });
                } else {
                    try ctx.stdout.print("  {s:<7}  {s}  ->  {s}\n", .{ rec.vendor, rec.action, rec.target_path });
                }
                if (rec.warning.len > 0) {
                    try ctx.stdout.print("           warning: {s}\n", .{rec.warning});
                }
            }
        }

        for (linked.records) |rec| {
            if (std.mem.eql(u8, rec.action, "created") or std.mem.eql(u8, rec.action, "updated")) total_linked += 1 else if (std.mem.eql(u8, rec.action, "unchanged")) total_unchanged += 1 else if (std.mem.eql(u8, rec.action, "skipped")) total_skipped += 1 else if (std.mem.eql(u8, rec.action, "dry-run")) total_dry_run += 1;
        }
    }

    if (!args.json) {
        try ctx.stdout.print("\n", .{});
        if (args.dry_run) {
            try ctx.stdout.print("dry-run: {d} would-be installs across {d} source(s)\n", .{ total_dry_run, picked.items.len });
        } else {
            try ctx.stdout.print(
                "done: {d} linked, {d} unchanged, {d} skipped across {d} source(s)\n",
                .{ total_linked, total_unchanged, total_skipped, picked.items.len },
            );
        }
    }
}

fn runReconcile(ctx: *const runtime.Ctx, dry_run: bool, json: bool) !void {
    const hr = common.resolveHomeAndRoot(ctx) catch |e|
        exit.die(ctx, e, "resolving sandbox root: {s}", .{@errorName(e)});
    defer {
        ctx.allocator.free(hr.home_dir);
        ctx.allocator.free(hr.sandbox_root);
    }

    const rec = engine.local.link.reconcile(.{
        .home_dir = hr.home_dir,
        .dry_run = dry_run,
    }, ctx.allocator) catch |e| exit.die(ctx, e, "reconcile failed: {s}", .{@errorName(e)});
    defer engine.local.link.deinitReconcileResult(rec, ctx.allocator);

    if (json) {
        for (rec.actions) |a| {
            const row = .{
                .name = a.name,
                .kind = @tagName(a.kind),
                .reason = a.reason,
                .source_path = a.source_path,
                .removed_targets = a.removed_targets,
            };
            try std.json.Stringify.value(row, .{ .emit_null_optional_fields = false }, ctx.stdout);
            try ctx.stdout.print("\n", .{});
        }
        return;
    }

    if (rec.actions.len == 0) {
        try ctx.stdout.print("reconcile: manifest already consistent with the filesystem\n", .{});
        return;
    }

    for (rec.actions) |a| {
        const verb = if (dry_run) "would remove" else "removed";
        try ctx.stdout.print("{s} ({s}) - {s}; {s} {d} install(s)\n", .{ a.name, @tagName(a.kind), a.reason, verb, a.removed_targets.len });
        for (a.removed_targets) |p| {
            try ctx.stdout.print("  {s}\n", .{p});
        }
    }
    if (dry_run) {
        try ctx.stdout.print("\ndry-run: {d} stale entry(ies) would be cleaned\n", .{rec.actions.len});
    } else {
        try ctx.stdout.print("\ndone: {d} stale entry(ies) cleaned\n", .{rec.actions.len});
    }
}
