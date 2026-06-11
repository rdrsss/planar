const std = @import("std");
const Io = std.Io;

const planar = @import("planar");
const cli = @import("cli");
const runtime = @import("runtime");
const exit = @import("exit.zig");
const cli_log = @import("cli_log.zig");
const worktree_gate = @import("worktree_gate.zig");

// Verb groups. Leaf verbs (no subverbs) live at `handlers/<verb>.zig`;
// group verbs (with subverbs) live at `handlers/<verb>/cmd.zig` with
// one sibling file per subverb. Both shapes export `pub const verb:
// cli.Cmd`, so main.zig composes uniformly.
const init_h = @import("handlers/init.zig");
const scope_h = @import("handlers/scope/cmd.zig");
const association_h = @import("handlers/association/cmd.zig");
const plan_h = @import("handlers/plan/cmd.zig");
const task_h = @import("handlers/task/cmd.zig");
const question_h = @import("handlers/question/cmd.zig");
const scenario_h = @import("handlers/scenario/cmd.zig");
const decision_h = @import("handlers/decision/cmd.zig");
const artifact_h = @import("handlers/artifact/cmd.zig");
const annotate_h = @import("handlers/annotate/cmd.zig");
const promote_h = @import("handlers/promote.zig");
const demote_h = @import("handlers/demote.zig");
const workbench_h = @import("handlers/workbench/cmd.zig");
const workspace_h = @import("handlers/workspace/cmd.zig");
const ext_h = @import("handlers/ext/cmd.zig");
const link_h = @import("handlers/link.zig");
const unlink_h = @import("handlers/unlink.zig");
const links_h = @import("handlers/links/cmd.zig");
const sync_h = @import("handlers/sync/cmd.zig");
const resume_h = @import("handlers/resume/cmd.zig");
const handoff_h = @import("handlers/handoff/cmd.zig");
const capture_h = @import("handlers/capture/cmd.zig");
const audit_h = @import("handlers/audit/cmd.zig");
const health_h = @import("handlers/health.zig");
const models_h = @import("handlers/models.zig");
const dashboard_h = @import("handlers/dashboard.zig");
const spec_h = @import("handlers/spec/cmd.zig");
const test_spec_h = @import("handlers/test_spec/cmd.zig");
const config_h = @import("handlers/config/cmd.zig");
const templates_h = @import("handlers/templates/cmd.zig");
const tree_h = @import("handlers/tree.zig");
const search_h = @import("handlers/search.zig");
const local_h = @import("handlers/local/cmd.zig");
const skills_h = @import("handlers/skills/cmd.zig");
const import_h = @import("handlers/import.zig");
const synthesize_h = @import("handlers/synthesize.zig");
const version_h = @import("handlers/version.zig");
const completion_h = @import("handlers/completion.zig");
const schema_h = @import("handlers/schema.zig");
const report_h = @import("handlers/report.zig");

/// Root command tree. `pub` because each `handlers/*.zig` imports it to
/// derive its typed args via `cli.castArgs(main.root, &.{…}, ptr)`.
///
/// No global flags right now — `--scope`, `--json`, `--verbose`, etc.
/// are declared per-verb to avoid name collisions during inheritance.
/// `--db` is the natural candidate to elevate to global once the DB
/// layer needs a uniform override.
pub const root: cli.Cmd = .{
    .name = "planar",
    .desc = "Planning + agent operations CLI.",
    .cmds = &.{
        init_h.verb,
        scope_h.verb,
        association_h.verb,
        plan_h.verb,
        task_h.verb,
        question_h.verb,
        scenario_h.verb,
        decision_h.verb,
        artifact_h.verb,
        annotate_h.verb,
        promote_h.verb,
        demote_h.verb,
        workbench_h.verb,
        workspace_h.verb,
        ext_h.verb,
        link_h.verb,
        unlink_h.verb,
        links_h.verb,
        sync_h.verb,
        resume_h.verb,
        handoff_h.verb,
        capture_h.verb,
        audit_h.verb,
        health_h.verb,
        models_h.verb,
        dashboard_h.verb,
        spec_h.verb,
        test_spec_h.verb,
        config_h.verb,
        templates_h.verb,
        tree_h.verb,
        search_h.verb,
        local_h.verb,
        skills_h.verb,
        import_h.verb,
        synthesize_h.verb,
        version_h.verb,
        completion_h.verb,
        schema_h.verb,
        report_h.verb,
    },
};

comptime {
    // Comptime cost grows with verbs × subverbs × flags. The 30-verb tree
    // needs a much larger budget than the default 1000.
    @setEvalBranchQuota(200_000);
    cli.validate(root);
}

// Writer buffers live at module scope so the runtime.Ctx pointers
// remain valid for the lifetime of the process.
var stdout_buffer: [4096]u8 = undefined;
var stderr_buffer: [1024]u8 = undefined;

pub fn main(init: std.process.Init) !void {
    const arena: std.mem.Allocator = init.arena.allocator();
    const args = try init.minimal.args.toSlice(arena);

    const db_path = try runtime.resolveDbPath(arena, init.minimal.environ);
    runtime.init(arena, init.io, &stdout_buffer, &stderr_buffer, db_path, init.minimal.environ, args);
    defer runtime.shutdown();

    // Capture the process start timestamp as early as meaningful (runtime
    // is live, stderr is available). Both the success path below and the
    // death path (exit.die) read this via cli_log.startNs().
    cli_log.setStartNs(cli_log.nowNanosPublic());

    // Plan 297 M3: refuse planning verbs invoked from inside a git
    // worktree. Runs AFTER runtime.init so the gate can stderr.print,
    // but BEFORE cli.dispatch so the refusal short-circuits the
    // handler's per-verb work. `--scope` does NOT override; see
    // `worktree_gate.check` and `docs/.../tech-spec § Scope handling
    // for worktrees`.
    worktree_gate.check(root, args);

    cli.dispatch(root, args, runtime.current().stdout) catch |e| switch (e) {
        cli.Parse.UnknownFlag,
        cli.Parse.MissingValue,
        cli.Parse.InvalidValue,
        cli.Parse.MissingRequired,
        cli.Parse.MissingRequiredPositional,
        cli.Parse.TooManyPositionals,
        cli.Parse.UnknownSubcommand,
        cli.Parse.UnexpectedArgument,
        cli.Parse.DuplicateFlag,
        => exit.die(runtime.current(), e, "{s}", .{@errorName(e)}),
        error.NotImplemented => exit.die(runtime.current(), e, "not implemented yet", .{}),
        // Other errors propagate; the deferred runtime.shutdown still
        // flushes + closes the DB before Zig's main wraps up.
        else => return e,
    };

    try runtime.flush();

    // Capture the successful invocation (fail-open — any error is swallowed).
    cli_log.record(0, null, cli_log.startNs());

    // Keep the planar lib import load-bearing for now.
    _ = planar;
}

test "simple test" {
    const gpa = std.testing.allocator;
    var list: std.ArrayList(i32) = .empty;
    defer list.deinit(gpa);
    try list.append(gpa, 42);
    try std.testing.expectEqual(@as(i32, 42), list.pop());
}
