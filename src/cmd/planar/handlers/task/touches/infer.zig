//! handlers/task/touches/infer — `planar task touches infer <task-id> [--repo <slug>] [--apply]`
//!
//! Proposes path-level `task_touch_paths` rows by extracting path-shaped
//! tokens from the task's title, body, and next_action and resolving them
//! against the repo checkout. PREVIEW BY DEFAULT: without `--apply` nothing
//! is written.
//!
//! Preview-first is load-bearing, not a nicety. Per decision 906 the two
//! error directions are asymmetric: an over-declared touch set costs
//! throughput (the task serializes when it might have run in parallel) and
//! is recoverable, while an under-declared one costs correctness (two tasks
//! marked eligible, fanned into separate worktrees, both editing the same
//! file, colliding at fan-in). Inference cannot tell which it produced —
//! only the operator can — so it proposes and stops.
//!
//! Candidates classified `unresolved` or `too_broad` are printed for review
//! and never written; `--apply` writes only the writable classifications.
//!
//! Repo selection: `--repo <slug>` names the checkout to resolve against.
//! Without it, the repo is derived from the cwd (the project whose
//! `root_path` is a prefix of the working directory), matching planar's
//! cwd-derived scope discipline.
//!
//! JSON shape:
//!   { "task_id": <n>, "repo_id": <n>, "repo_slug": "...", "applied": <bool>,
//!     "written": <n>, "review": <n>,
//!     "candidates": [ { "token": "...", "evidence": "body",
//!                       "classification": "resolved", "paths": ["..."] } ] }

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../../exit.zig");

const touchinfer = engine.planning.touchinfer;

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "task", "touches", "infer" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const task_id = std.fmt.parseInt(i64, args.task_id, 10) catch
        exit.die(ctx, error.InvalidInput, "task id must be an integer, got '{s}'", .{args.task_id});

    const repo = resolveRepo(d, ctx.allocator, args.repo) catch |e| switch (e) {
        error.NotFound => if (args.repo) |slug|
            exit.die(ctx, e, "repo '{s}' not found", .{slug})
        else
            exit.die(
                ctx,
                e,
                "no repo matches the current directory; pass --repo <slug>",
                .{},
            ),
        error.NoRootPath => exit.die(
            ctx,
            error.InvalidInput,
            "repo has no root_path recorded; cannot resolve paths against it",
            .{},
        ),
        else => exit.die(ctx, e, "repo lookup: {s}", .{@errorName(e)}),
    };
    defer {
        ctx.allocator.free(repo.slug);
        ctx.allocator.free(repo.root);
    }

    var inf = touchinfer.infer(d, ctx.allocator, task_id, repo.id, repo.root) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "task {d} not found", .{task_id}),
        else => exit.die(ctx, e, "task touches infer: {s}", .{@errorName(e)}),
    };
    defer inf.deinit();

    // ---- apply ------------------------------------------------------------
    //
    // Both writes per path (the repo-level edge and the path row) must land
    // together, for the same reason `touches add --path` wraps them: a
    // partial commit leaves the coarse repo edge without its path row, and
    // recommend-strategy then falls back to the whole-repo signal and
    // serializes a task that should have been eligible.
    var written: usize = 0;
    if (args.apply and inf.writableCount(args.wide) > 0) {
        const sp_name = "touches_infer";
        d.savepoint(ctx.allocator, sp_name) catch |e|
            exit.die(ctx, e, "task touches infer: savepoint: {s}", .{@errorName(e)});
        var released = false;
        defer if (!released) {
            d.rollbackToSavepoint(ctx.allocator, sp_name) catch {};
            d.releaseSavepoint(ctx.allocator, sp_name) catch {};
        };

        // The path-touch implies the repo-touch; keep the coarse signal
        // consistent with the fine one. A pre-existing edge is a no-op.
        if (engine.entitylink.add(d, ctx.allocator, .{
            .from_kind = .task,
            .from_id = task_id,
            .to_kind = .repo,
            .to_id = repo.id,
            .relationship = .touches,
        })) |link| {
            engine.entitylink.deinit(link, ctx.allocator);
        } else |e| switch (e) {
            error.LinkExists => {},
            // No EndpointNotFound arm: both endpoints are known to exist by
            // this point. `touchinfer.infer` above reads the task and dies
            // with "task {d} not found" if it is missing, and the repo was
            // resolved from cwd or --repo. The generic arm below covers the
            // impossible case without pretending it is expected.
            else => exit.die(ctx, e, "task touches infer: repo edge: {s}", .{@errorName(e)}),
        }

        for (inf.candidates) |c| {
            if (!c.classification.isWritable(args.wide)) continue;
            for (c.paths) |p| {
                // addTouchPath is `insert or ignore` against
                // unique(task_id, repo_id, path), so re-running is a no-op
                // and two candidates proposing the same path collapse.
                engine.planning.task.addTouchPath(d, task_id, repo.id, p) catch |e|
                    exit.die(ctx, e, "task touches infer: write {s}: {s}", .{ p, @errorName(e) });
                written += 1;
            }
        }

        d.releaseSavepoint(ctx.allocator, sp_name) catch |e|
            exit.die(ctx, e, "task touches infer: release savepoint: {s}", .{@errorName(e)});
        released = true;
    }

    // ---- report -----------------------------------------------------------
    if (args.json) {
        try emitJson(ctx, inf, repo, args.apply, written, args.wide);
        return;
    }

    try ctx.stdout.print(
        "task:{d}  repo:{s}  proposed:{d}  review:{d}  {s}\n",
        .{
            task_id,
            repo.slug,
            inf.writableCount(args.wide),
            inf.reviewCount(args.wide),
            if (args.apply) "APPLIED" else "preview (nothing written)",
        },
    );

    for (inf.candidates) |c| {
        if (!c.classification.isWritable(args.wide)) continue;
        for (c.paths) |p| {
            try ctx.stdout.print(
                "  + {s}\n      [{s} via {s}: {s}]\n",
                .{ p, c.classification.toText(), c.evidence.toText(), c.token },
            );
        }
    }

    if (inf.reviewCount(args.wide) > 0) {
        try ctx.stdout.print("\nnot written — review:\n", .{});
        var wide_available: usize = 0;
        for (inf.candidates) |c| {
            if (c.classification.isWritable(args.wide)) continue;
            // Show the expansion size for wide candidates: it is the whole
            // basis for judging one. A token that would declare 35 files is
            // a different proposition from one that would declare 2, and the
            // count is what tells them apart.
            if (c.paths.len > 0) {
                wide_available += 1;
                try ctx.stdout.print(
                    "  ? {s}  [{s} via {s} — would declare {d} path(s)]\n",
                    .{ c.token, c.classification.toText(), c.evidence.toText(), c.paths.len },
                );
            } else {
                try ctx.stdout.print(
                    "  ? {s}  [{s} via {s}]\n",
                    .{ c.token, c.classification.toText(), c.evidence.toText() },
                );
            }
        }
        if (!args.wide and wide_available > 0) {
            try ctx.stdout.print(
                "\n  {d} directory/basename candidate(s) withheld — add --wide to include them.\n" ++
                    "  Wide expansion measured NEGATIVE for eligibility: it intersects peers\n" ++
                    "  and rule 2 drops both, so it can remove tasks that were otherwise fine.\n",
                .{wide_available},
            );
        }
    }

    if (!args.apply and inf.writableCount(args.wide) > 0) {
        try ctx.stdout.print(
            "\napply with: planar task touches infer {d} --apply\n",
            .{task_id},
        );
    }
}

// =========================================================================
// Repo resolution
// =========================================================================

const Repo = struct { id: i64, slug: []const u8, root: []const u8 };

const RepoError = error{ NotFound, NoRootPath, QueryFailed } || std.mem.Allocator.Error;

/// Resolve the repo to resolve paths against: by slug when given, else the
/// project whose `root_path` is a prefix of the cwd. Longest prefix wins so
/// a submodule checkout beats its superproject.
fn resolveRepo(d: anytype, a: std.mem.Allocator, slug_opt: ?[]const u8) RepoError!Repo {
    if (slug_opt) |slug| {
        var stmt = d.prepare("select id, slug, root_path from projects where slug = ?") catch
            return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .text = slug }}) catch return error.QueryFailed;
        return switch (stmt.step() catch return error.QueryFailed) {
            .done => error.NotFound,
            .row => blk: {
                const root = try stmt.columnTextAlloc(2, a);
                if (root.len == 0) {
                    a.free(root);
                    return error.NoRootPath;
                }
                break :blk .{
                    .id = stmt.columnInt(0),
                    .slug = try stmt.columnTextAlloc(1, a),
                    .root = root,
                };
            },
        };
    }

    // PWD-first, via the engine's canonical acquisition — handlers do not
    // choose path spellings themselves, and PWD-vs-realpath matters here
    // because submodule checkouts are exactly the case this resolves.
    const cwd = engine.operatorpath.cwdCurrent(a, runtime.current().io) catch
        return error.NotFound;
    defer a.free(cwd);

    var stmt = d.prepare(
        "select id, slug, root_path from projects where root_path is not null",
    ) catch return error.QueryFailed;
    defer stmt.finalize();

    var best: ?Repo = null;
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const root = try stmt.columnTextAlloc(2, a);
                // Longest matching prefix wins, so a submodule checkout
                // beats its superproject.
                const keep = root.len > 0 and
                    std.mem.startsWith(u8, cwd, root) and
                    (best == null or root.len > best.?.root.len);
                if (!keep) {
                    a.free(root);
                    continue;
                }
                if (best) |b| {
                    a.free(b.slug);
                    a.free(b.root);
                }
                best = .{
                    .id = stmt.columnInt(0),
                    .slug = try stmt.columnTextAlloc(1, a),
                    .root = root,
                };
            },
        }
    }

    return best orelse error.NotFound;
}

// =========================================================================
// JSON
// =========================================================================

fn emitJson(
    ctx: anytype,
    inf: touchinfer.Inference,
    repo: Repo,
    applied: bool,
    written: usize,
    wide: bool,
) !void {
    const w = ctx.stdout;
    try w.print(
        "{{\"task_id\":{d},\"repo_id\":{d},\"repo_slug\":",
        .{ inf.task_id, repo.id },
    );
    try std.json.Stringify.value(repo.slug, .{}, w);
    try w.print(
        ",\"applied\":{s},\"written\":{d},\"review\":{d},\"candidates\":[",
        .{ if (applied) "true" else "false", written, inf.reviewCount(wide) },
    );
    for (inf.candidates, 0..) |c, i| {
        if (i > 0) try w.print(",", .{});
        try w.print("{{\"token\":", .{});
        try std.json.Stringify.value(c.token, .{}, w);
        try w.print(",\"evidence\":", .{});
        try std.json.Stringify.value(c.evidence.toText(), .{}, w);
        try w.print(",\"classification\":", .{});
        try std.json.Stringify.value(c.classification.toText(), .{}, w);
        try w.print(",\"paths\":[", .{});
        for (c.paths, 0..) |p, j| {
            if (j > 0) try w.print(",", .{});
            try std.json.Stringify.value(p, .{}, w);
        }
        try w.print("]}}", .{});
    }
    try w.print("]}}\n", .{});
}
