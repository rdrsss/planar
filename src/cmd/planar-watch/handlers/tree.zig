//! handlers/tree — `planar-watch tree [--root-session <id>] [--follow] [--interval D]`
//!
//! Renders the orchestrator → sub-agent forest by walking
//! `agent_actions.parent_action_id` chains. Root actions are rows where
//! `parent_action_id IS NULL`; each child is indented by depth.
//!
//! Plan 467 M4 additions (tasks 3064–3067):
//!   3064 — verb wiring; tree walk via WITH RECURSIVE CTE.
//!   3065 — row format mirrors ps columns; unicode tree characters.
//!   3066 — `--root-session <id>` scopes output to one session's subtree.
//!   3067 — `--follow` reuses the Tier-2 wake from feed/ps.
//!
//! Text row format (depth 0 = root, each child indented by tree chars):
//!   scope:<label>  vendor:<v>  activity:<summary>  worktree:<wt>
//!   branch:<b>  last_hb:<rel>
//!
//! Tree characters:
//!   Root actions: no prefix.
//!   Non-last child: "├── "
//!   Last child:    "└── "
//!   Vertical guide prefix for descendants: "│   "
//!
//! DB: read-only (planar-watch three-binary boundary).

const std = @import("std");
const cli = @import("cli");
const db = @import("db");
const engine = @import("engine");
const runtime = @import("runtime");

const main = @import("../main.zig");
const exit = @import("../exit.zig");
const follow = @import("follow.zig");
const format = @import("format.zig");
const ps = @import("ps.zig");

const agentactivity = engine.runtime.agentactivity;

pub const verb: cli.Cmd = .{
    .name = "tree",
    .desc = "Render the orchestrator → sub-agent action forest.",
    .long_desc = "Walks agent_actions.parent_action_id chains and renders the\n" ++
        "  orchestrator → sub-agent forest. Root rows have parent_action_id IS NULL.\n" ++
        "  Each child is indented with unicode tree characters (├── / └── / │).\n\n" ++
        "  --root-session <id>  scope to one session's subtree (error if unknown).\n" ++
        "  --follow             stream; re-renders on WAL change (Tier-2 wake).\n" ++
        "  --interval           maximum poll cadence for --follow (default 1s).\n\n" ++
        "  Each row shows the claim's: scope vendor activity worktree branch last_hb.",
    .flags = &.{
        .{ .long = "--root-session", .kind = .int, .desc = "Scope output to one session's subtree (session id)" },
        .{ .long = "--follow", .kind = .bool, .default = .{ .bool = false }, .desc = "Stream re-renders until SIGINT" },
        .{ .long = "--interval", .kind = .string, .desc = "Poll interval for --follow (default 1s; e.g. 100ms)" },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"tree"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbStrictReadOnly() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    // Validate --root-session if supplied.
    if (args.root_session) |sid| {
        if (sid < 1) exit.die(ctx, error.InvalidValue, "tree: --root-session: must be a positive integer", .{});
        // Validate the session exists in the DB.
        const exists = sessionExists(d, sid) catch |e| exit.die(ctx, e, "tree: {s}", .{@errorName(e)});
        if (!exists) exit.die(ctx, error.NotFound, "tree: --root-session {d}: session not found", .{sid});
    }

    const interval_ns = ps.parseIntervalOrDefault(args.interval);
    if (args.follow) follow.installSigintHandler();

    var live_d = d;
    while (true) {
        emitOnce(ctx.stdout, live_d, ctx.allocator, args) catch |e|
            exit.die(ctx, e, "tree: {s}", .{@errorName(e)});
        try ctx.stdout.flush();

        if (!args.follow) return;
        if (follow.shouldStop()) return;
        follow.interruptibleSleep(interval_ns);
        if (follow.shouldStop()) return;
        live_d = runtime.ensureDbStrictReadOnly() catch |e|
            exit.die(ctx, e, "{s}", .{@errorName(e)});
    }
}

// =========================================================================
// Session existence check
// =========================================================================

fn sessionExists(d: *db.sqlite.Db, session_id: i64) !bool {
    var stmt = d.prepare("select 1 from sessions where id = ? limit 1") catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = session_id }}) catch return error.QueryFailed;
    return switch (stmt.step() catch return error.QueryFailed) {
        .done => false,
        .row => true,
    };
}

// =========================================================================
// Forest walk
// =========================================================================

/// One node in the flattened tree walk result.
const TreeNode = struct {
    action_id: i64,
    parent_action_id: ?i64,
    session_id: i64,
    /// The claim row linked to this action (may be null for un-linked actions).
    claim_id: ?i64,
    depth: i64,
    /// Position among siblings: 1-based, and whether this is the last
    /// sibling. Used to choose ├── vs └── tree characters.
    is_last_sibling: bool,

    pub fn deinit(_: TreeNode, _: std.mem.Allocator) void {}
};

/// Walk the action tree using a WITH RECURSIVE CTE. Returns all nodes
/// in pre-order (root first, children follow their parent), enriched
/// with depth and sibling-position information.
///
/// When `root_session_id` is non-null, only walks roots from that session.
/// When null, walks ALL root actions.
///
/// SQL explanation:
///   The WITH RECURSIVE anchor selects root actions (parent_action_id IS NULL),
///   filtered by session if --root-session is supplied. The recursive term
///   extends via `parent_action_id = prev.action_id`. We join against a
///   sibling-count subquery to compute `is_last_sibling`.
fn walkTree(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    root_session_id: ?i64,
) ![]TreeNode {
    var nodes: std.ArrayList(TreeNode) = .empty;
    errdefer nodes.deinit(allocator);

    // First: collect (action_id, parent_action_id, session_id, claim_id, depth)
    // via WITH RECURSIVE.
    if (root_session_id) |sid| {
        var stmt = d.prepare(
            \\with recursive tree(action_id, parent_action_id, session_id, claim_id, depth) as (
            \\  select id, parent_action_id, session_id, claim_id, 0
            \\  from agent_actions
            \\  where parent_action_id is null and session_id = ?
            \\  union all
            \\  select a.id, a.parent_action_id, a.session_id, a.claim_id, t.depth + 1
            \\  from agent_actions a
            \\  join tree t on a.parent_action_id = t.action_id
            \\)
            \\select action_id, parent_action_id, session_id, claim_id, depth
            \\from tree
            \\order by action_id asc
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = sid }}) catch return error.QueryFailed;
        try collectNodes(&stmt, &nodes, allocator);
    } else {
        var stmt = d.prepare(
            \\with recursive tree(action_id, parent_action_id, session_id, claim_id, depth) as (
            \\  select id, parent_action_id, session_id, claim_id, 0
            \\  from agent_actions
            \\  where parent_action_id is null
            \\  union all
            \\  select a.id, a.parent_action_id, a.session_id, a.claim_id, t.depth + 1
            \\  from agent_actions a
            \\  join tree t on a.parent_action_id = t.action_id
            \\)
            \\select action_id, parent_action_id, session_id, claim_id, depth
            \\from tree
            \\order by action_id asc
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        try collectNodes(&stmt, &nodes, allocator);
    }

    const result = try nodes.toOwnedSlice(allocator);

    // Annotate is_last_sibling: for each node, check if it's the last
    // among its siblings (same parent). We do this in a second pass.
    annotateLastSibling(result);

    return result;
}

fn collectNodes(
    stmt: *db.sqlite.Stmt,
    out: *std.ArrayList(TreeNode),
    allocator: std.mem.Allocator,
) !void {
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const action_id = stmt.columnInt(0);
                const parent_id = stmt.columnIntOpt(1);
                const session_id = stmt.columnInt(2);
                const claim_id = stmt.columnIntOpt(3);
                const depth = stmt.columnInt(4);
                try out.append(allocator, .{
                    .action_id = action_id,
                    .parent_action_id = parent_id,
                    .session_id = session_id,
                    .claim_id = claim_id,
                    .depth = depth,
                    .is_last_sibling = false, // filled by annotateLastSibling
                });
            },
        }
    }
}

/// Annotate `is_last_sibling` for each node. A node is the last sibling
/// if no later node in the array shares the same `parent_action_id`.
/// Since we order by action_id asc, we walk backwards and track which
/// parent IDs we've already seen.
fn annotateLastSibling(nodes: []TreeNode) void {
    if (nodes.len == 0) return;
    // Walk backwards. First occurrence of a (parent, depth) pair from the
    // end is the last sibling.
    var i = nodes.len;
    while (i > 0) {
        i -= 1;
        // Check if any later node has the same parent.
        var found_later = false;
        for (nodes[i + 1 ..]) |later| {
            const same_parent: bool = if (nodes[i].parent_action_id) |pid|
                if (later.parent_action_id) |lpid| lpid == pid else false
            else
                later.parent_action_id == null and later.session_id == nodes[i].session_id;
            if (same_parent) {
                found_later = true;
                break;
            }
        }
        nodes[i].is_last_sibling = !found_later;
    }
}

// =========================================================================
// Rendering
// =========================================================================

fn emitOnce(
    w: *std.Io.Writer,
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    args: anytype,
) !void {
    const nodes = try walkTree(d, allocator, args.root_session);
    defer allocator.free(nodes);

    if (nodes.len == 0) {
        try w.print("(no action chains)\n", .{});
        return;
    }

    // We need to track which ancestor levels are still "open" (have
    // further siblings below) so we can draw the │ continuation lines.
    // We keep a boolean array indexed by depth: open_at_depth[d] == true
    // means some ancestor at depth d still has later siblings.
    var open_at_depth = try allocator.alloc(bool, 64); // generous upper bound
    defer allocator.free(open_at_depth);
    @memset(open_at_depth, false);

    for (nodes) |node| {
        // Update the open-at-depth table.
        const depth_usize: usize = @intCast(if (node.depth >= 0) node.depth else 0);
        if (depth_usize < open_at_depth.len) {
            // The current node is open for its own depth if it is NOT the
            // last sibling (meaning later siblings exist at same level).
            open_at_depth[depth_usize] = !node.is_last_sibling;
            // Clear deeper levels (they belong to a different subtree now).
            if (depth_usize + 1 < open_at_depth.len) {
                @memset(open_at_depth[depth_usize + 1 ..], false);
            }
        }

        // Build the line prefix for this node.
        try renderTreeLine(w, d, allocator, node, open_at_depth);
    }
}

/// Render one tree node as a text line with tree-character prefix.
///
/// Format:
///   [depth-prefix][claim columns]
///
/// Depth-prefix (for depth > 0):
///   For each ancestor level 1..depth-1: "│   " if open, "    " if closed.
///   For level depth: "├── " if not last sibling, "└── " if last.
///
/// Claim columns (same set as ps):
///   scope:<label>  vendor:<v>  activity:<summary>  worktree:<wt>
///   branch:<b>  last_hb:<rel>
fn renderTreeLine(
    w: *std.Io.Writer,
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    node: TreeNode,
    open_at_depth: []const bool,
) !void {
    const depth_usize: usize = @intCast(if (node.depth >= 0) node.depth else 0);

    // Build tree prefix.
    if (depth_usize == 0) {
        // Root: no prefix, just the action id as a label.
        try w.print("action:{d}  ", .{node.action_id});
    } else {
        // Draw ancestor guides for levels 1..depth-1.
        var level: usize = 1;
        while (level < depth_usize) : (level += 1) {
            if (level < open_at_depth.len and open_at_depth[level]) {
                try w.print("\xE2\x94\x82   ", .{}); // │ + 3 spaces
            } else {
                try w.print("    ", .{});
            }
        }
        // Draw the branch character at this depth.
        if (node.is_last_sibling) {
            try w.print("\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 ", .{}); // └──
        } else {
            try w.print("\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 ", .{}); // ├──
        }
    }

    // Render claim columns if this action has a linked claim.
    if (node.claim_id) |cid| {
        const claim_opt = agentactivity.store.getClaimById(d, allocator, cid) catch null;
        defer if (claim_opt) |c| c.deinit(allocator);

        if (claim_opt) |c| {
            const scope = agentactivity.store.resolveClaimScope(d, allocator, c);
            defer scope.deinit(allocator);

            const action_row = agentactivity.store.latestActionForClaim(d, allocator, c.id) catch null;
            defer if (action_row) |a| a.deinit(allocator);
            const raw_summary: ?[]const u8 = if (action_row) |a| a.summary else null;

            const activity_buf = try format.renderActivitySummary(allocator, raw_summary);
            defer allocator.free(activity_buf);
            const worktree_buf = try format.renderWorktreeColumn(allocator, c.worktree_path);
            defer allocator.free(worktree_buf);
            const hb_buf = try format.renderRelativeHeartbeat(allocator, c.last_heartbeat_at);
            defer allocator.free(hb_buf);

            const branch = c.branch orelse "?";

            try w.print(
                "scope:{s}  vendor:{s}  activity:{s}  worktree:{s}  branch:{s}  last_hb:{s}\n",
                .{
                    scope.label(),
                    c.vendor,
                    activity_buf,
                    worktree_buf,
                    branch,
                    hb_buf,
                },
            );
            return;
        }
    }

    // No linked claim: render a minimal line showing action id and session.
    try w.print("session:{d}  (no claim)\n", .{node.session_id});
}

// =========================================================================
// Tests
// =========================================================================

test "annotateLastSibling: single root is last sibling" {
    var nodes = [_]TreeNode{
        .{ .action_id = 1, .parent_action_id = null, .session_id = 1, .claim_id = null, .depth = 0, .is_last_sibling = false },
    };
    annotateLastSibling(&nodes);
    try std.testing.expect(nodes[0].is_last_sibling);
}

test "annotateLastSibling: two roots — first not last, second is last" {
    var nodes = [_]TreeNode{
        .{ .action_id = 1, .parent_action_id = null, .session_id = 1, .claim_id = null, .depth = 0, .is_last_sibling = false },
        .{ .action_id = 2, .parent_action_id = null, .session_id = 1, .claim_id = null, .depth = 0, .is_last_sibling = false },
    };
    annotateLastSibling(&nodes);
    try std.testing.expect(!nodes[0].is_last_sibling);
    try std.testing.expect(nodes[1].is_last_sibling);
}

test "annotateLastSibling: parent with two children" {
    var nodes = [_]TreeNode{
        .{ .action_id = 1, .parent_action_id = null, .session_id = 1, .claim_id = null, .depth = 0, .is_last_sibling = false },
        .{ .action_id = 2, .parent_action_id = 1, .session_id = 1, .claim_id = null, .depth = 1, .is_last_sibling = false },
        .{ .action_id = 3, .parent_action_id = 1, .session_id = 1, .claim_id = null, .depth = 1, .is_last_sibling = false },
    };
    annotateLastSibling(&nodes);
    // Root is last in its sibling group (only root).
    try std.testing.expect(nodes[0].is_last_sibling);
    // Child 2 is not last (child 3 follows with same parent).
    try std.testing.expect(!nodes[1].is_last_sibling);
    // Child 3 is last.
    try std.testing.expect(nodes[2].is_last_sibling);
}
