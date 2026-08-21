const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const common = @import("common.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "local", "import" }, args_ptr);
    const ctx = runtime.current();

    const hr = common.resolveHomeAndRoot(ctx) catch |e|
        exit.die(ctx, e, "resolving sandbox root: {s}", .{@errorName(e)});
    defer {
        ctx.allocator.free(hr.home_dir);
        ctx.allocator.free(hr.sandbox_root);
    }

    const kind = common.parseKind(args.kind) catch
        exit.die(ctx, error.InvalidInput, "--kind must be skill or agent, got \"{s}\"", .{args.kind orelse ""});

    const res = engine.local.import.import(
        .{
            .home_dir = hr.home_dir,
            .source_path = args.path,
            .kind = kind,
            .force = args.force,
            .dry_run = args.dry_run,
        },
        ctx.allocator,
    ) catch |e| exit.die(ctx, e, "importing {s} failed: {s}", .{ args.path, @errorName(e) });
    defer engine.local.import.deinitResult(res, ctx.allocator);

    if (args.json) {
        try emitGoImportJSON(ctx, res);
    } else {
        renderImportResult(ctx, res, args.dry_run) catch |e|
            exit.die(ctx, e, "rendering import result failed: {s}", .{@errorName(e)});
    }

    if (args.no_link or args.dry_run or res.imported.len == 0) return;

    if (!args.json) {
        try ctx.stdout.print("\nLinking imported files into vendor surfaces:\n", .{});
    }

    for (res.imported) |rec| {
        const file = engine.local.manifest.parseFile(rec.target_path, kind, ctx.allocator) catch |e|
            exit.die(ctx, e, "re-parsing imported file {s} failed: {s}", .{ rec.target_path, @errorName(e) });
        defer engine.local.manifest.deinitSandboxFile(file, ctx.allocator);

        const linked = engine.local.link.link(file, .{ .home_dir = hr.home_dir }, ctx.allocator) catch |e|
            exit.die(ctx, e, "linking {s} failed: {s}", .{ file.name, @errorName(e) });
        defer engine.local.link.deinitLinkResult(linked, ctx.allocator);

        if (args.json) {
            try common.emitGoLinkJSON(ctx, file, linked);
            continue;
        }

        try ctx.stdout.print("{s} ({s})\n", .{ linked.name, @tagName(linked.kind) });
        for (linked.records) |lrec| {
            if (lrec.mode) |mode| {
                try ctx.stdout.print("  {s:<7}  {s} [{s}]  ->  {s}\n", .{ lrec.vendor, lrec.action, @tagName(mode), lrec.target_path });
            } else {
                try ctx.stdout.print("  {s:<7}  {s}  ->  {s}\n", .{ lrec.vendor, lrec.action, lrec.target_path });
            }
            if (lrec.warning.len > 0) try ctx.stdout.print("           warning: {s}\n", .{lrec.warning});
        }
    }
}

fn renderImportResult(ctx: *const runtime.Ctx, res: engine.local.import.Result, dry_run: bool) !void {
    if (res.imported.len == 0 and res.skipped.len == 0) {
        try ctx.stdout.print("no files matched for import\n", .{});
        return;
    }
    _ = dry_run;
    for (res.imported) |rec| {
        try ctx.stdout.print("{s:<12}  {s:<12}  <-  {s}\n", .{ rec.name, rec.action, rec.source_path });
    }
    for (res.skipped) |rec| {
        try ctx.stdout.print("{s:<12}  skipped       reason: {s}\n", .{ rec.name, rec.reason });
    }
    for (res.warnings) |w| {
        try ctx.stdout.print("warning [{s}.{s}] {s}\n", .{ w.name, w.field, w.message });
    }
    try ctx.stdout.print("\nimported {d} file(s); skipped {d}\n", .{ res.imported.len, res.skipped.len });
}

fn emitGoImportJSON(ctx: *const runtime.Ctx, res: engine.local.import.Result) !void {
    const GoRecord = struct {
        Name: []const u8,
        SourcePath: []const u8,
        TargetPath: []const u8,
        Action: []const u8,
        Reason: []const u8,
    };
    const GoWarning = struct {
        Name: []const u8,
        Field: []const u8,
        Message: []const u8,
    };
    var imported: std.ArrayList(GoRecord) = .empty;
    defer imported.deinit(ctx.allocator);
    var skipped: std.ArrayList(GoRecord) = .empty;
    defer skipped.deinit(ctx.allocator);
    var warnings: std.ArrayList(GoWarning) = .empty;
    defer warnings.deinit(ctx.allocator);

    for (res.imported) |r| {
        try imported.append(ctx.allocator, .{
            .Name = r.name,
            .SourcePath = r.source_path,
            .TargetPath = r.target_path,
            .Action = r.action,
            .Reason = r.reason,
        });
    }
    for (res.skipped) |r| {
        try skipped.append(ctx.allocator, .{
            .Name = r.name,
            .SourcePath = r.source_path,
            .TargetPath = r.target_path,
            .Action = r.action,
            .Reason = r.reason,
        });
    }
    for (res.warnings) |w| {
        try warnings.append(ctx.allocator, .{
            .Name = w.name,
            .Field = w.field,
            .Message = w.message,
        });
    }

    try std.json.Stringify.value(.{
        .Imported = imported.items,
        .Skipped = skipped.items,
        .Warnings = warnings.items,
    }, .{}, ctx.stdout);
    try ctx.stdout.print("\n", .{});
}
