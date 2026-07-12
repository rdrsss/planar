const std = @import("std");
const db = @import("db");

pub const FindingKind = enum { task, question };
pub const Severity = enum { info, low, medium, high, critical };
pub const Disposition = enum { untriaged, @"needs-reproduction", accepted, @"retained-question", dismissed, @"reported-external", duplicate };
pub const Reproduction = enum { @"not-run", reproduced, @"not-reproduced", inconclusive };

pub const Ref = struct { kind: FindingKind, id: i64 };
pub const Triage = struct {
    id: i64,
    finding: []const u8,
    plan_id: ?i64,
    severity: Severity,
    disposition: Disposition,
    reproduction_status: Reproduction,
    duplicate_of: ?[]const u8,
    evidence_summary: ?[]const u8,
    created_at: []const u8,
    updated_at: []const u8,
};
pub const Filter = struct { plan_id: ?i64 = null, severity: ?Severity = null, disposition: ?Disposition = null };
pub const SetArgs = struct { severity: Severity, disposition: Disposition, reproduction: Reproduction, duplicate_of: ?Ref = null, evidence: ?[]const u8 = null };
pub const Error = error{ NotFound, InvalidInput, MissingFeedbackPlan, AmbiguousFeedbackPlan, DifferentFeedbackPlan, DuplicateCycle, QueryFailed } || std.mem.Allocator.Error;

pub fn parseRef(raw: []const u8) Error!Ref {
    const colon = std.mem.indexOfScalar(u8, raw, ':') orelse return Error.InvalidInput;
    const kind: FindingKind = if (std.mem.eql(u8, raw[0..colon], "task")) .task else if (std.mem.eql(u8, raw[0..colon], "question")) .question else return Error.InvalidInput;
    const id = std.fmt.parseInt(i64, raw[colon + 1 ..], 10) catch return Error.InvalidInput;
    if (id < 1) return Error.InvalidInput;
    return .{ .kind = kind, .id = id };
}
pub fn parseSeverity(s: []const u8) ?Severity {
    return std.meta.stringToEnum(Severity, s);
}
pub fn parseDisposition(s: []const u8) ?Disposition {
    return std.meta.stringToEnum(Disposition, s);
}
pub fn parseReproduction(s: []const u8) ?Reproduction {
    return std.meta.stringToEnum(Reproduction, s);
}

pub fn deinit(v: Triage, a: std.mem.Allocator) void {
    a.free(v.finding);
    if (v.duplicate_of) |s| a.free(s);
    if (v.evidence_summary) |s| a.free(s);
    a.free(v.created_at);
    a.free(v.updated_at);
}
pub fn deinitMany(v: []const Triage, a: std.mem.Allocator) void {
    for (v) |x| deinit(x, a);
    a.free(v);
}

fn findingPlan(d: *db.sqlite.Db, a: std.mem.Allocator, r: Ref) Error!i64 {
    const sql: [:0]const u8 = switch (r.kind) {
        .task => "select t.plan_id,p.slug from tasks t left join plans p on p.id=t.plan_id where t.id=?",
        .question => "select count(el.to_id),min(el.to_id),min(p.slug) from questions q left join entity_links el on el.from_kind='question' and el.from_id=q.id and el.to_kind='plan' and el.relationship='derives-from' left join plans p on p.id=el.to_id where q.id=? group by q.id",
    };
    var st = d.prepare(sql) catch return Error.QueryFailed;
    defer st.finalize();
    st.bind(&.{.{ .int = r.id }}) catch return Error.QueryFailed;
    if ((st.step() catch return Error.QueryFailed) != .row) return Error.NotFound;
    const link_count = if (r.kind == .question) st.columnInt(0) else @as(i64, 1);
    if (link_count == 0 or st.columnIntOpt(if (r.kind == .question) 1 else 0) == null) return Error.MissingFeedbackPlan;
    if (link_count > 1) return Error.AmbiguousFeedbackPlan;
    const slug = (try st.columnTextOpt(if (r.kind == .question) 2 else 1, a)) orelse return Error.MissingFeedbackPlan;
    defer a.free(slug);
    if (!std.mem.eql(u8, slug, "planar-feedback")) return Error.DifferentFeedbackPlan;
    return st.columnInt(if (r.kind == .question) 1 else 0);
}

pub fn entityScope(d: *db.sqlite.Db, a: std.mem.Allocator, r: Ref) Error!?[]const u8 {
    const table = if (r.kind == .task) "tasks" else "questions";
    var buf: [128]u8 = undefined;
    const sql = std.fmt.bufPrintZ(&buf, "select scope_kind, scope_id from {s} where id=?", .{table}) catch return Error.QueryFailed;
    var st = d.prepare(sql) catch return Error.QueryFailed;
    defer st.finalize();
    st.bind(&.{.{ .int = r.id }}) catch return Error.QueryFailed;
    if ((st.step() catch return Error.QueryFailed) != .row) return Error.NotFound;
    const kind = try st.columnTextAlloc(0, a);
    defer a.free(kind);
    if (std.mem.eql(u8, kind, "global")) return null;
    const id = st.columnInt(1);
    const ref_table = if (std.mem.eql(u8, kind, "repo")) "projects" else "associations";
    const prefix = if (std.mem.eql(u8, kind, "repo")) "repo:" else "assoc:";
    const q = std.fmt.bufPrintZ(&buf, "select slug from {s} where id=?", .{ref_table}) catch return Error.QueryFailed;
    var ss = d.prepare(q) catch return Error.QueryFailed;
    defer ss.finalize();
    ss.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    if ((ss.step() catch return Error.QueryFailed) != .row) return Error.QueryFailed;
    const slug = try ss.columnTextAlloc(0, a);
    defer a.free(slug);
    return try std.fmt.allocPrint(a, "{s}{s}", .{ prefix, slug });
}

pub fn set(d: *db.sqlite.Db, a: std.mem.Allocator, finding: Ref, args: SetArgs) Error!Triage {
    const plan = try findingPlan(d, a, finding);
    if ((args.disposition == .duplicate) != (args.duplicate_of != null)) return Error.InvalidInput;
    var duplicate_id: ?i64 = null;
    if (args.duplicate_of) |target| {
        if (target.kind == finding.kind and target.id == finding.id) return Error.DuplicateCycle;
        if ((try findingPlan(d, a, target)) != plan) return Error.DifferentFeedbackPlan;
        const target_row = show(d, a, target) catch |e| switch (e) {
            Error.NotFound => return Error.InvalidInput,
            else => return e,
        };
        defer deinit(target_row, a);
        duplicate_id = target_row.id;
        var cursor = target_row.duplicate_of;
        while (cursor) |raw| {
            const r = try parseRef(raw);
            if (r.kind == finding.kind and r.id == finding.id) return Error.DuplicateCycle;
            const row = try show(d, a, r);
            defer deinit(row, a);
            cursor = row.duplicate_of;
        }
    }
    const task_id: db.sqlite.Param = if (finding.kind == .task) .{ .int = finding.id } else .{ .null = {} };
    const question_id: db.sqlite.Param = if (finding.kind == .question) .{ .int = finding.id } else .{ .null = {} };
    _ = d.execParams(
        "insert into feedback_triage (finding_task_id,finding_question_id,severity,disposition,reproduction_status,duplicate_of_triage_id,evidence_summary) values (?,?,?,?,?,?,?) on conflict do update set severity=excluded.severity, disposition=excluded.disposition, reproduction_status=excluded.reproduction_status, duplicate_of_triage_id=excluded.duplicate_of_triage_id, evidence_summary=excluded.evidence_summary, updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now')",
        &.{ task_id, question_id, .{ .text = @tagName(args.severity) }, .{ .text = @tagName(args.disposition) }, .{ .text = @tagName(args.reproduction) }, if (duplicate_id) |id| .{ .int = id } else .{ .null = {} }, if (args.evidence) |s| .{ .text = s } else .{ .null = {} } },
    ) catch return Error.QueryFailed;
    return try show(d, a, finding);
}

pub fn show(d: *db.sqlite.Db, a: std.mem.Allocator, r: Ref) Error!Triage {
    const sql: [:0]const u8 = switch (r.kind) {
        .task => select_sql ++ " where ft.finding_task_id=?",
        .question => select_sql ++ " where ft.finding_question_id=?",
    };
    var st = d.prepare(sql) catch return Error.QueryFailed;
    defer st.finalize();
    st.bind(&.{.{ .int = r.id }}) catch return Error.QueryFailed;
    if ((st.step() catch return Error.QueryFailed) != .row) return Error.NotFound;
    return readRow(&st, a);
}
pub fn list(d: *db.sqlite.Db, a: std.mem.Allocator, f: Filter) Error![]Triage {
    var sql: std.ArrayList(u8) = .empty;
    defer sql.deinit(a);
    try sql.appendSlice(a, select_sql ++ " where 1=1");
    var vals: std.ArrayList(db.sqlite.Param) = .empty;
    defer vals.deinit(a);
    if (f.plan_id) |x| {
        try sql.appendSlice(a, " and coalesce(t.plan_id,qp.to_id)=?");
        try vals.append(a, .{ .int = x });
    }
    if (f.severity) |x| {
        try sql.appendSlice(a, " and ft.severity=?");
        try vals.append(a, .{ .text = @tagName(x) });
    }
    if (f.disposition) |x| {
        try sql.appendSlice(a, " and ft.disposition=?");
        try vals.append(a, .{ .text = @tagName(x) });
    }
    try sql.appendSlice(a, " order by coalesce(t.plan_id,qp.to_id), ft.updated_at desc, ft.id");
    try sql.append(a, 0);
    var st = d.prepare(sql.items[0 .. sql.items.len - 1 :0]) catch return Error.QueryFailed;
    defer st.finalize();
    st.bind(vals.items) catch return Error.QueryFailed;
    var out: std.ArrayList(Triage) = .empty;
    errdefer {
        for (out.items) |x| deinit(x, a);
        out.deinit(a);
    }
    while ((st.step() catch return Error.QueryFailed) == .row) try out.append(a, try readRow(&st, a));
    return try out.toOwnedSlice(a);
}

const select_sql = "select ft.id, case when ft.finding_task_id is not null then 'task:'||ft.finding_task_id else 'question:'||ft.finding_question_id end, coalesce(t.plan_id,qp.to_id), ft.severity,ft.disposition,ft.reproduction_status, case when dup.finding_task_id is not null then 'task:'||dup.finding_task_id when dup.finding_question_id is not null then 'question:'||dup.finding_question_id end,ft.evidence_summary,ft.created_at,ft.updated_at from feedback_triage ft left join tasks t on t.id=ft.finding_task_id left join entity_links qp on qp.from_kind='question' and qp.from_id=ft.finding_question_id and qp.to_kind='plan' and qp.relationship='derives-from' left join feedback_triage dup on dup.id=ft.duplicate_of_triage_id";
fn readRow(st: *db.sqlite.Stmt, a: std.mem.Allocator) Error!Triage {
    const s = try st.columnTextAlloc(3, a);
    defer a.free(s);
    const di = try st.columnTextAlloc(4, a);
    defer a.free(di);
    const r = try st.columnTextAlloc(5, a);
    defer a.free(r);
    return .{ .id = st.columnInt(0), .finding = try st.columnTextAlloc(1, a), .plan_id = st.columnIntOpt(2), .severity = parseSeverity(s) orelse return Error.QueryFailed, .disposition = parseDisposition(di) orelse return Error.QueryFailed, .reproduction_status = parseReproduction(r) orelse return Error.QueryFailed, .duplicate_of = try st.columnTextOpt(6, a), .evidence_summary = try st.columnTextOpt(7, a), .created_at = try st.columnTextAlloc(8, a), .updated_at = try st.columnTextAlloc(9, a) };
}
pub fn renderText(v: Triage, w: *std.Io.Writer) !void {
    try w.print("{s}\n  severity: {s}\n  disposition: {s}\n  reproduction: {s}\n  duplicate-of: {s}\n  evidence: {s}\n", .{ v.finding, @tagName(v.severity), @tagName(v.disposition), @tagName(v.reproduction_status), v.duplicate_of orelse "-", v.evidence_summary orelse "-" });
}
pub fn renderListText(v: []const Triage, w: *std.Io.Writer) !void {
    if (v.len == 0) {
        try w.print("(no feedback triage)\n", .{});
        return;
    }
    for (v) |x| try w.print("{s:<18} {s:<8} {s:<20} {s}\n", .{ x.finding, @tagName(x.severity), @tagName(x.disposition), @tagName(x.reproduction_status) });
}

test "parse finding references" {
    try std.testing.expectEqual(FindingKind.task, (try parseRef("task:42")).kind);
    try std.testing.expectError(Error.InvalidInput, parseRef("plan:1"));
}

test "set show list structured task and question triage" {
    const a = std.testing.allocator;
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, a);
    _ = try d.execParams("insert into plans(scope_kind,scope_id,title,slug,status) values('global',null,'Feedback','planar-feedback','draft')", &.{});
    const plan_id = try d.intQuery("select id from plans where slug='planar-feedback'");
    _ = try d.execParams("insert into tasks(scope_kind,scope_id,plan_id,title) values('global',null,?,'Task finding')", &.{.{ .int = plan_id }});
    const task_id = try d.intQuery("select id from tasks where title='Task finding'");
    _ = try d.execParams("insert into questions(scope_kind,scope_id,title) values('global',null,'Question finding')", &.{});
    const question_id = try d.intQuery("select id from questions where title='Question finding'");
    _ = try d.execParams("insert into entity_links(from_kind,from_id,to_kind,to_id,relationship) values('question',?,'plan',?,'derives-from')", &.{ .{ .int = question_id }, .{ .int = plan_id } });

    const task_row = try set(&d, a, .{ .kind = .task, .id = task_id }, .{ .severity = .high, .disposition = .accepted, .reproduction = .reproduced, .evidence = "redacted evidence" });
    defer deinit(task_row, a);
    try std.testing.expectEqual(plan_id, task_row.plan_id.?);
    try std.testing.expectEqualStrings("redacted evidence", task_row.evidence_summary.?);

    const question_row = try set(&d, a, .{ .kind = .question, .id = question_id }, .{ .severity = .medium, .disposition = .@"retained-question", .reproduction = .inconclusive });
    defer deinit(question_row, a);
    const rows = try list(&d, a, .{ .plan_id = plan_id });
    defer deinitMany(rows, a);
    try std.testing.expectEqual(@as(usize, 2), rows.len);
}

test "duplicate validation rejects self and cross-plan targets" {
    const a = std.testing.allocator;
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, a);
    _ = try d.execParams("insert into plans(scope_kind,scope_id,title,slug,status) values('global',null,'One','planar-feedback','draft'),('global',null,'Two','two','draft')", &.{});
    _ = try d.execParams("insert into tasks(scope_kind,scope_id,plan_id,title) values('global',null,1,'One'),('global',null,2,'Two')", &.{});
    const first = try set(&d, a, .{ .kind = .task, .id = 1 }, .{ .severity = .low, .disposition = .accepted, .reproduction = .@"not-run" });
    defer deinit(first, a);
    try std.testing.expectError(Error.DuplicateCycle, set(&d, a, .{ .kind = .task, .id = 1 }, .{ .severity = .low, .disposition = .duplicate, .reproduction = .@"not-run", .duplicate_of = .{ .kind = .task, .id = 1 } }));
    try std.testing.expectError(Error.DifferentFeedbackPlan, set(&d, a, .{ .kind = .task, .id = 2 }, .{ .severity = .low, .disposition = .duplicate, .reproduction = .@"not-run", .duplicate_of = .{ .kind = .task, .id = 1 } }));
}

test "duplicate validation traverses multi-hop chains and rejects cycles" {
    const a = std.testing.allocator;
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, a);
    _ = try d.execParams("insert into plans(scope_kind,scope_id,title,slug,status) values('global',null,'Feedback','planar-feedback','draft')", &.{});
    _ = try d.execParams("insert into tasks(scope_kind,scope_id,plan_id,title) values('global',null,1,'One'),('global',null,1,'Two'),('global',null,1,'Three'),('global',null,1,'Four')", &.{});

    const one = try set(&d, a, .{ .kind = .task, .id = 1 }, .{ .severity = .low, .disposition = .accepted, .reproduction = .@"not-run" });
    deinit(one, a);
    const two = try set(&d, a, .{ .kind = .task, .id = 2 }, .{ .severity = .low, .disposition = .duplicate, .reproduction = .@"not-run", .duplicate_of = .{ .kind = .task, .id = 1 } });
    deinit(two, a);
    const three = try set(&d, a, .{ .kind = .task, .id = 3 }, .{ .severity = .low, .disposition = .duplicate, .reproduction = .@"not-run", .duplicate_of = .{ .kind = .task, .id = 2 } });
    deinit(three, a);

    const four = try set(&d, a, .{ .kind = .task, .id = 4 }, .{ .severity = .low, .disposition = .duplicate, .reproduction = .@"not-run", .duplicate_of = .{ .kind = .task, .id = 3 } });
    defer deinit(four, a);
    try std.testing.expectEqualStrings("task:3", four.duplicate_of.?);

    try std.testing.expectError(Error.DuplicateCycle, set(&d, a, .{ .kind = .task, .id = 1 }, .{ .severity = .low, .disposition = .duplicate, .reproduction = .@"not-run", .duplicate_of = .{ .kind = .task, .id = 3 } }));
}
