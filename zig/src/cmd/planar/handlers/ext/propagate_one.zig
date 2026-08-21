//! handlers/ext/propagate-one — `planar ext propagate-one <system> --from <kind:id> [--strategy] --json`
//!
//! Render one entity's template, POST the counterpart, and record the
//! `external_links` row; idempotent (skip with a clear result if a
//! link already exists).
//!
//! This is the per-entity body of `ext propagate`'s tracking-issue path
//! surfaced as a thin primitive verb. Both `propagate-one` and the main
//! `propagate` loop call `propagateOneEntity` to share the logic.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");
const adapter_factory = @import("adapter_factory.zig");
const remote = @import("remote.zig");
const tmpl_common = @import("../templates/common.zig");
const propagate = @import("propagate.zig");

/// EntityRole identifies how an entity sits in the feature tree.
pub const EntityRole = enum {
    plan_anchor,
    plan_child,
    task,
};

/// PropagateOneResult is the outcome of a single-entity propagation.
pub const PropagateOneResult = struct {
    entity_kind: []const u8,
    entity_id: i64,
    title: []const u8,
    /// op is one of: "created", "skipped", "failed"
    op: []const u8,
    external_id: []const u8,
    error_message: []const u8 = "",

    pub fn deinit(self: PropagateOneResult, allocator: std.mem.Allocator) void {
        allocator.free(self.entity_kind);
        allocator.free(self.title);
        allocator.free(self.external_id);
        if (self.error_message.len > 0) allocator.free(self.error_message);
    }
};

/// propagateOneEntity is the shared per-entity propagation body: render +
/// POST + record external_links (idempotent skip when a mirror link already
/// exists). This function is called by both `propagate-one` and the
/// tracking-issue path inside the main `ext propagate` loop.
///
/// Returns an owned PropagateOneResult. Caller must call `.deinit()`.
pub fn propagateOneEntity(
    ctx: *const runtime.Ctx,
    d: anytype,
    sys: engine.external.system.ExternalSystem,
    adp: ?*adapter_factory.Handle,
    entity_kind_str: []const u8,
    entity_id: i64,
    entity_title: []const u8,
    role: EntityRole,
    strategy_kind: []const u8,
    template_kind: []const u8,
    sync_dir: engine.external.link.SyncDirection,
    dry_run: bool,
    templates_root: []const u8,
) !PropagateOneResult {
    // Idempotent skip when a mirror link already exists.
    const existing = try engine.extsync.propagate.loadExistingMirror(
        ctx.allocator,
        d,
        entity_kind_str,
        entity_id,
        sys.id,
    );
    if (existing.len > 0) {
        return .{
            .entity_kind = try ctx.allocator.dupe(u8, entity_kind_str),
            .entity_id = entity_id,
            .title = try ctx.allocator.dupe(u8, entity_title),
            .op = "skipped",
            .external_id = existing, // ownership transferred
        };
    }
    ctx.allocator.free(existing);

    // Build context + load template + render.
    var owned = if (std.mem.eql(u8, entity_kind_str, "task"))
        try engine.templates.builder.buildTaskContext(ctx.allocator, d, entity_id)
    else
        try engine.templates.builder.buildPlanContext(ctx.allocator, d, entity_id);
    defer owned.deinit();

    const tmpl = try engine.templates.load(
        ctx.allocator,
        "default",
        sys.kind.toText(),
        template_kind,
        templates_root,
    );
    defer engine.templates.deinitTemplate(tmpl, ctx.allocator);

    var rendered = try engine.templates.renderTemplate(ctx.allocator, tmpl.fields, owned.ctx);
    defer rendered.deinit();

    const payload = try rendered.toJson(ctx.allocator);
    defer ctx.allocator.free(payload);

    if (dry_run) {
        return .{
            .entity_kind = try ctx.allocator.dupe(u8, entity_kind_str),
            .entity_id = entity_id,
            .title = try ctx.allocator.dupe(u8, entity_title),
            .op = "planned",
            .external_id = try std.fmt.allocPrint(ctx.allocator, "<{s}>", .{template_kind}),
        };
    }

    // POST to remote.
    const created = try remote.createRemote(adp.?, ctx.allocator, sys, payload);
    defer ctx.allocator.free(created.external_url);

    // Record external_links row. The anchor plan also carries the strategy
    // cache in config_json so a future propagate hits the stickiness path.
    const entity_kind_enum = engine.external.link.ExternalEntityKind.fromText(entity_kind_str) orelse {
        ctx.allocator.free(created.external_id);
        return error.InvalidInput;
    };
    const is_anchor = role == .plan_anchor;
    const anchor_config_json: ?[]const u8 = if (is_anchor)
        try std.fmt.allocPrint(ctx.allocator, "{{\"strategy\":\"{s}\"}}", .{strategy_kind})
    else
        null;
    defer if (anchor_config_json) |s| ctx.allocator.free(s);

    const link = try engine.external.link.create(d, ctx.allocator, .{
        .entity_kind = entity_kind_enum,
        .entity_id = entity_id,
        .system_id = sys.id,
        .external_id = created.external_id,
        .external_url = if (created.external_url.len > 0) created.external_url else null,
        .link_role = .mirror,
        .sync_direction = sync_dir,
        .initial_status = .ok,
        .config_json = anchor_config_json,
    });
    defer engine.external.link.deinit(link, ctx.allocator);

    return .{
        .entity_kind = try ctx.allocator.dupe(u8, entity_kind_str),
        .entity_id = entity_id,
        .title = try ctx.allocator.dupe(u8, entity_title),
        .op = "created",
        .external_id = created.external_id, // ownership transferred
    };
}

// ---- CLI handler for `ext propagate-one` ------------------------------------

/// KindID parses a "kind:integer-id" ref string.
const KindID = struct { kind: []const u8, id: i64 };

fn parseKindIDRef(s: []const u8) !KindID {
    var i: usize = s.len;
    while (i > 0) : (i -= 1) {
        if (s[i - 1] == ':') {
            const kind = s[0 .. i - 1];
            const id_str = s[i..];
            if (kind.len == 0 or id_str.len == 0) break;
            const id = std.fmt.parseInt(i64, id_str, 10) catch break;
            return .{ .kind = kind, .id = id };
        }
    }
    return error.InvalidInput;
}

/// determineRole maps a persisted entity kind string to an EntityRole, using
/// the DB to distinguish anchor vs child plans. When the plan has a non-null
/// parent_plan_id it is a child; otherwise it is treated as an anchor.
fn determineRole(d: anytype, entity_kind: []const u8, entity_id: i64, anchor_plan_id: ?i64) !EntityRole {
    if (std.mem.eql(u8, entity_kind, "task")) return .task;
    // entity is a plan — check if it is the anchor
    if (anchor_plan_id) |apid| {
        if (entity_id == apid) return .plan_anchor;
    }
    // Check if the plan has a parent (child plan) via nullable column.
    var stmt = try d.prepare("select parent_plan_id from plans where id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = entity_id }});
    const step = try stmt.step();
    if (step == .done) return error.NotFound;
    const parent_val = stmt.columnIntOpt(0);
    return if (parent_val != null) .plan_child else .plan_anchor;
}

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "ext", "propagate-one" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    // Parse --from kind:id
    const ref = parseKindIDRef(args.from) catch
        exit.die(ctx, error.InvalidInput, "invalid --from value '{s}'; expected kind:integer-id", .{args.from});

    // Validate entity kind
    if (!std.mem.eql(u8, ref.kind, "plan") and !std.mem.eql(u8, ref.kind, "task")) {
        exit.die(ctx, error.InvalidInput, "unsupported entity kind '{s}'; accepted: plan, task", .{ref.kind});
    }

    // Validate --strategy if provided.
    // parent-issue and projects-v2 require the full feature-tree walk performed
    // by `ext propagate --github-strategy`; they are NOT supported by the
    // per-entity `propagate-one` primitive and must be rejected early so the
    // caller gets a clear error instead of a mislabeled mirror link.
    if (args.strategy) |strat| {
        if (std.mem.eql(u8, strat, "parent-issue") or std.mem.eql(u8, strat, "projects-v2")) {
            exit.die(
                ctx,
                error.InvalidInput,
                "strategy '{s}' is not supported by propagate-one; use ext propagate --github-strategy {s}",
                .{ strat, strat },
            );
        }
        if (!std.mem.eql(u8, strat, "tracking-issue")) {
            exit.die(ctx, error.InvalidInput, "invalid --strategy '{s}'; accepted: tracking-issue", .{strat});
        }
    }

    if (args.sync) |sync_text| {
        _ = engine.external.link.SyncDirection.fromText(sync_text) orelse
            exit.die(ctx, error.InvalidInput, "invalid --sync '{s}'; accepted: read-only, write-back, two-way", .{sync_text});
    }

    // Resolve target system.
    const sys = resolveSystem(d, ctx.allocator, args.system) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, error.NotFound, "external system '{s}' not found", .{args.system}),
        error.NoSystemsRegistered => exit.die(ctx, error.NotFound, "no external systems registered; run 'planar ext register' first", .{}),
        else => exit.die(ctx, e, "ext propagate-one: lookup system: {s}", .{@errorName(e)}),
    };
    defer engine.external.system.deinit(sys, ctx.allocator);

    // Resolve entity title and check entity exists.
    const entity_title = loadEntityTitle(d, ctx.allocator, ref.kind, ref.id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, error.NotFound, "{s}:{d} not found", .{ ref.kind, ref.id }),
        else => exit.die(ctx, e, "ext propagate-one: read entity: {s}", .{@errorName(e)}),
    };
    defer ctx.allocator.free(entity_title);

    // Determine entity role (anchor vs child plan vs task).
    const role = determineRole(d, ref.kind, ref.id, null) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, error.NotFound, "{s}:{d} not found", .{ ref.kind, ref.id }),
        else => exit.die(ctx, e, "ext propagate-one: determine role: {s}", .{@errorName(e)}),
    };

    // Determine strategy.
    const strategy_kind: []const u8 = blk: {
        if (args.strategy) |strat| {
            if (std.mem.eql(u8, strat, "parent-issue")) break :blk "github-parent-issue";
            if (std.mem.eql(u8, strat, "projects-v2")) break :blk "github-projects-v2";
            if (std.mem.eql(u8, strat, "tracking-issue")) break :blk "github-tracking-issue";
        }
        // Use strategyForSystem as default (static strings, no alloc needed)
        const strat = engine.extsync.propagate.strategyForSystem(sys.kind.toText()) catch |e|
            exit.die(ctx, e, "ext propagate-one: resolve strategy: {s}", .{@errorName(e)});
        break :blk strat.kind;
    };

    // Map strategy to template kind for this entity.
    const template_kind = templateKindForEntity(sys.kind.toText(), strategy_kind, role) catch |e|
        exit.die(ctx, e, "ext propagate-one: map strategy to template: {s}", .{@errorName(e)});

    // Templates root.
    const templates_root = tmpl_common.resolveTemplatesRoot(ctx) catch |e|
        exit.die(ctx, e, "resolving templates root: {s}", .{@errorName(e)});
    defer ctx.allocator.free(templates_root);

    const sync_text: []const u8 = args.sync orelse "read-only";
    const sync_dir = engine.external.link.SyncDirection.fromText(sync_text).?;

    // Build adapter (skipped under dry-run).
    var adp: ?*adapter_factory.Handle = null;
    defer if (adp) |h| {
        h.deinit();
        ctx.allocator.destroy(h);
    };
    if (!args.dry_run) {
        adp = adapter_factory.build(ctx.allocator, ctx.io, &ctx.environ, sys) catch |e| switch (e) {
            error.TokenEnvVarMissing => exit.die(ctx, error.InvalidInput, "token env var '{s}' is not set", .{sys.auth_ref}),
            error.UnsupportedAuthMethod => exit.die(ctx, error.InvalidInput, "oauth-stored auth not yet supported", .{}),
            error.GhCliNotFound => exit.die(ctx, error.InvalidInput, "gh-cli auth: `gh` binary not on PATH", .{}),
            error.GhCliFailed => exit.die(ctx, error.InvalidInput, "gh-cli auth: `gh auth token` returned non-zero (run `gh auth login`)", .{}),
            error.GhCliEmptyToken => exit.die(ctx, error.InvalidInput, "gh-cli auth: `gh auth token` returned empty output", .{}),
            error.UnsupportedSystemKind => exit.die(ctx, error.InvalidInput, "system kind '{s}' is not supported", .{sys.kind.toText()}),
            else => exit.die(ctx, e, "ext propagate-one: build adapter: {s}", .{@errorName(e)}),
        };
    }

    var result = propagateOneEntity(
        ctx,
        d,
        sys,
        adp,
        ref.kind,
        ref.id,
        entity_title,
        role,
        strategy_kind,
        template_kind,
        sync_dir,
        args.dry_run,
        templates_root,
    ) catch |e|
        exit.die(ctx, e, "ext propagate-one: {s}", .{@errorName(e)});
    defer result.deinit(ctx.allocator);

    const ok = !std.mem.eql(u8, result.op, "failed");

    if (args.json) {
        try ctx.stdout.print("{{\"ok\":{s},\"entity_kind\":", .{if (ok) "true" else "false"});
        try output.writeJsonString(ctx.stdout, result.entity_kind);
        try ctx.stdout.print(",\"entity_id\":{d},\"title\":", .{result.entity_id});
        try output.writeJsonString(ctx.stdout, result.title);
        try ctx.stdout.print(",\"op\":", .{});
        try output.writeJsonString(ctx.stdout, result.op);
        try ctx.stdout.print(",\"external_id\":", .{});
        try output.writeJsonString(ctx.stdout, result.external_id);
        try ctx.stdout.print(",\"system\":", .{});
        try output.writeJsonString(ctx.stdout, sys.slug);
        try ctx.stdout.print(",\"strategy\":", .{});
        try output.writeJsonString(ctx.stdout, strategy_kind);
        if (result.error_message.len > 0) {
            try ctx.stdout.print(",\"error\":", .{});
            try output.writeJsonString(ctx.stdout, result.error_message);
        }
        try ctx.stdout.print("}}\n", .{});
    } else {
        const prefix: []const u8 = if (args.dry_run) "(dry-run) " else "";
        try ctx.stdout.print("{s}{s} {s}:{d} ({s}) -> {s}\n", .{
            prefix, result.op, result.entity_kind, result.entity_id, result.title, result.external_id,
        });
    }

    if (!ok) {
        exit.die(ctx, error.InvalidInput, "propagate-one failed for {s}:{d}: {s}", .{ ref.kind, ref.id, result.error_message });
    }
}

// ---- helpers ----------------------------------------------------------------

fn loadEntityTitle(d: anytype, allocator: std.mem.Allocator, kind: []const u8, id: i64) ![]const u8 {
    const sql: [:0]const u8 = if (std.mem.eql(u8, kind, "task"))
        "select coalesce(title,'') from tasks where id = ?"
    else
        "select coalesce(title,'') from plans where id = ?";
    var stmt = try d.prepare(sql);
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = id }});
    const step = try stmt.step();
    if (step == .done) return error.NotFound;
    return try stmt.columnTextAlloc(0, allocator);
}

/// templateKindForEntity maps (system_kind, strategy_kind, role) to the
/// template kind string used for rendering.
pub fn templateKindForEntity(system_kind: []const u8, strategy_kind: []const u8, role: EntityRole) ![]const u8 {
    if (std.mem.eql(u8, system_kind, "jira")) {
        return switch (role) {
            .plan_anchor => "epic",
            .plan_child => "story",
            .task => "sub-task",
        };
    }
    if (std.mem.eql(u8, system_kind, "github-issues")) {
        _ = strategy_kind; // all GitHub strategies share the same template kinds
        return switch (role) {
            .plan_anchor => "parent-issue",
            .plan_child => "issue",
            .task => "sub-task",
        };
    }
    return error.UnsupportedSystemKind;
}

fn resolveSystem(
    d: anytype,
    allocator: std.mem.Allocator,
    slug: []const u8,
) !engine.external.system.ExternalSystem {
    if (slug.len > 0) {
        return engine.external.system.showBySlug(d, allocator, slug);
    }
    const systems = try engine.external.system.list(d, allocator);
    defer engine.external.system.deinitMany(systems, allocator);
    if (systems.len == 0) return error.NoSystemsRegistered;
    return engine.external.system.showBySlug(d, allocator, systems[0].slug);
}
