//! engine/ingestor/parse — Markdown extraction for tech-spec, roadmap, test-spec.
//!
//! Pure data-transformation: no DB, no IO. Mirrors Go's
//! `src/internal/ingestor/parse.go` regex + headings logic exactly so the
//! preview diff and apply path produce structurally identical output to
//! the Go binary against the same input.
//!
//! Conventions (locked decisions, see plan 314 M9 tech-spec):
//!
//!   * Tech-spec H2 `## Decisions` → each `### <title>` H3 is one Decision.
//!   * Tech-spec H2 `## Open Questions` → each `### <title>` H3 is one
//!     Question; a body that begins with `Resolution:` (case-sensitive,
//!     first non-blank line) populates `Question.resolution`.
//!   * Roadmap H2 `## <milestone>` → each bullet (`- ` or `* `) becomes
//!     a WorkItem. `[touches: a, b]` and `[slug: foo-bar]` annotations
//!     anywhere in the bullet are stripped from the display title and
//!     stored separately. `[slug: ...]` is sanitized (lowercase ASCII +
//!     digits + `-`).
//!   * Test-spec H2 `## Scenarios` → each `### Scenario: <title>` H3
//!     (the `Scenario: ` prefix is optional) becomes a Scenario, with
//!     three field lines lifted out of the body:
//!         **Verifies:** task:<slug-or-id>[, ...]
//!         **Kind:**     <free-form>
//!         **Acceptance:** <one-line>
//!     The comma-split, trim, and slug-validate steps for `**Verifies:**`
//!     are load-bearing — the coverage gate depends on them.
//!
//! All output slices and string fields are owned by the allocator passed
//! to the function that produced them. Each top-level type carries a
//! matching `deinit` / `deinitMany`.

const std = @import("std");

// =========================================================================
// Types
// =========================================================================

/// One parsed `### <title>` block under tech-spec `## Decisions`.
pub const Decision = struct {
    title: []const u8,
    body: []const u8,
};

pub fn deinitDecision(d: Decision, allocator: std.mem.Allocator) void {
    allocator.free(d.title);
    allocator.free(d.body);
}

pub fn deinitDecisions(items: []const Decision, allocator: std.mem.Allocator) void {
    for (items) |d| deinitDecision(d, allocator);
    allocator.free(items);
}

/// One parsed `### <title>` block under tech-spec `## Open Questions`.
/// `resolution` is the text after the literal `Resolution:` marker (case-
/// sensitive) when the body's first non-blank line carries it; empty
/// otherwise. `body` always retains the full H3 body including the
/// resolution line when present.
pub const Question = struct {
    title: []const u8,
    body: []const u8,
    resolution: []const u8,
};

pub fn deinitQuestion(q: Question, allocator: std.mem.Allocator) void {
    allocator.free(q.title);
    allocator.free(q.body);
    allocator.free(q.resolution);
}

pub fn deinitQuestions(items: []const Question, allocator: std.mem.Allocator) void {
    for (items) |q| deinitQuestion(q, allocator);
    allocator.free(items);
}

/// One roadmap bullet. `touches` and `slug` are extracted from the
/// optional `[touches: a, b]` and `[slug: foo-bar]` annotations.
pub const WorkItem = struct {
    title: []const u8,
    touches: []const []const u8,
    slug: []const u8,
};

pub fn deinitWorkItem(w: WorkItem, allocator: std.mem.Allocator) void {
    allocator.free(w.title);
    for (w.touches) |s| allocator.free(s);
    allocator.free(w.touches);
    allocator.free(w.slug);
}

/// One H2 milestone in a roadmap.
pub const Milestone = struct {
    name: []const u8,
    intent: []const u8,
    work_items: []const WorkItem,
};

pub fn deinitMilestone(m: Milestone, allocator: std.mem.Allocator) void {
    allocator.free(m.name);
    allocator.free(m.intent);
    for (m.work_items) |w| deinitWorkItem(w, allocator);
    allocator.free(m.work_items);
}

pub fn deinitMilestones(items: []const Milestone, allocator: std.mem.Allocator) void {
    for (items) |m| deinitMilestone(m, allocator);
    allocator.free(items);
}

/// One `**Verifies:** <ref>[, <ref>, ...]` entry from a test-spec scenario.
///
/// `id` and `slug` are mutually exclusive: exactly one is set on every
/// successfully-parsed ref. Slug-form refs feed the coverage gate;
/// numeric refs are ignored for coverage but still produce a
/// `verifies` entity_links edge at apply time.
pub const TaskRef = struct {
    kind: []const u8,
    id: i64,
    slug: []const u8,
};

pub fn deinitTaskRef(r: TaskRef, allocator: std.mem.Allocator) void {
    allocator.free(r.kind);
    allocator.free(r.slug);
}

/// One H3 under test-spec `## Scenarios`.
pub const Scenario = struct {
    title: []const u8,
    kind: []const u8,
    acceptance: []const u8,
    verifies: []const TaskRef,
    body: []const u8,
};

pub fn deinitScenario(s: Scenario, allocator: std.mem.Allocator) void {
    allocator.free(s.title);
    allocator.free(s.kind);
    allocator.free(s.acceptance);
    for (s.verifies) |r| deinitTaskRef(r, allocator);
    allocator.free(s.verifies);
    allocator.free(s.body);
}

pub fn deinitScenarios(items: []const Scenario, allocator: std.mem.Allocator) void {
    for (items) |s| deinitScenario(s, allocator);
    allocator.free(items);
}

// =========================================================================
// Public parsers
// =========================================================================

/// Parse the `## Decisions` H2 section of a tech-spec body.
///
/// Two grammars are recognized (H3 preferred):
///   * `### <title>` H3 headings — each H3 block becomes one Decision.
///   * `- **Title.** body` or `* **Title.** body` bullets — used as a
///     fallback when the section has NO H3 headings at all. The title is
///     the bolded lead (first `**…**` run or, absent that, the first
///     sentence up to `.`/`?`/`!`); the body is the full bullet text.
///
/// Returns an empty slice when no `## Decisions` section exists.
/// Body text is trimmed of surrounding blank lines.
pub fn parseTechSpecDecisions(
    allocator: std.mem.Allocator,
    body: []const u8,
) std.mem.Allocator.Error![]const Decision {
    const lines = try splitLines(allocator, body);
    defer allocator.free(lines);

    const section = sliceSection(lines, "## Decisions") orelse return &.{};

    // First pass: check whether any H3 headings exist in the section.
    // If so, the H3 path takes precedence and bullets are ignored.
    var has_h3 = false;
    for (section) |line| {
        if (std.mem.startsWith(u8, trimRight(line), "### ")) {
            has_h3 = true;
            break;
        }
    }

    var out: std.ArrayList(Decision) = .empty;
    errdefer {
        for (out.items) |d| deinitDecision(d, allocator);
        out.deinit(allocator);
    }

    if (has_h3) {
        // --- H3 path (original behavior) ---
        var current_title: ?[]const u8 = null;
        var body_lines: std.ArrayList([]const u8) = .empty;
        defer body_lines.deinit(allocator);

        for (section) |line| {
            const trimmed = trimRight(line);
            if (std.mem.startsWith(u8, trimmed, "### ")) {
                // Flush prior block.
                if (current_title) |t| {
                    const decided_body = try joinTrimmedBody(allocator, body_lines.items);
                    try out.append(allocator, .{ .title = t, .body = decided_body });
                    current_title = null;
                }
                const title = std.mem.trim(u8, trimmed[4..], " \t");
                current_title = try allocator.dupe(u8, title);
                body_lines.clearRetainingCapacity();
                continue;
            }
            if (current_title != null) {
                try body_lines.append(allocator, line);
            }
        }
        if (current_title) |t| {
            const decided_body = try joinTrimmedBody(allocator, body_lines.items);
            try out.append(allocator, .{ .title = t, .body = decided_body });
        }
    } else {
        // --- Bullet fallback path ---
        // Each `- ` or `* ` bullet becomes one Decision. The title is
        // extracted from the bolded lead (`**Title.**`) when present;
        // otherwise the first sentence (up to the first `.`, `?`, or `!`
        // in the bullet text). The body is the full bullet text after
        // stripping the leading `- ` / `* ` marker.
        for (section) |line| {
            const trimmed = trimRight(line);
            if (!isBullet(trimmed)) continue;

            // Strip the bullet marker.
            var raw = std.mem.trimStart(u8, trimmed, " \t");
            if (std.mem.startsWith(u8, raw, "- ")) {
                raw = raw[2..];
            } else if (std.mem.startsWith(u8, raw, "* ")) {
                raw = raw[2..];
            }
            raw = std.mem.trim(u8, raw, " \t");
            if (raw.len == 0) continue;

            const title = try extractBulletDecisionTitle(allocator, raw);
            const body_text = try allocator.dupe(u8, raw);
            try out.append(allocator, .{ .title = title, .body = body_text });
        }
    }

    return try out.toOwnedSlice(allocator);
}

/// extractBulletDecisionTitle extracts a short title from a decision bullet.
///
/// Priority:
///   1. First `**…**` bold run (the Markdown convention for bolded leads).
///   2. First sentence ending with `.`, `?`, or `!`.
///   3. The full bullet text (trimmed) as a fallback.
fn extractBulletDecisionTitle(
    allocator: std.mem.Allocator,
    bullet_text: []const u8,
) std.mem.Allocator.Error![]const u8 {
    // Look for **bold** lead.
    if (std.mem.startsWith(u8, bullet_text, "**")) {
        if (std.mem.indexOfPos(u8, bullet_text, 2, "**")) |close| {
            const inner = bullet_text[2..close];
            const t = std.mem.trim(u8, inner, " \t");
            if (t.len > 0) return try allocator.dupe(u8, t);
        }
    }
    // First sentence heuristic: up to (and including) the first `.`, `?`, `!`.
    for (bullet_text, 0..) |c, i| {
        if (c == '.' or c == '?' or c == '!') {
            const sentence = std.mem.trim(u8, bullet_text[0 .. i + 1], " \t");
            if (sentence.len > 0) return try allocator.dupe(u8, sentence);
        }
    }
    // Full text fallback.
    return try allocator.dupe(u8, std.mem.trim(u8, bullet_text, " \t"));
}

/// Parse the `## Open Questions` H2 section of a tech-spec body. Each
/// H3 becomes a Question; a body starting with `Resolution:` populates
/// the resolution field. Returns an empty slice when no section exists.
pub fn parseTechSpecOpenQuestions(
    allocator: std.mem.Allocator,
    body: []const u8,
) std.mem.Allocator.Error![]const Question {
    const lines = try splitLines(allocator, body);
    defer allocator.free(lines);

    const section = sliceSection(lines, "## Open Questions") orelse return &.{};

    var out: std.ArrayList(Question) = .empty;
    errdefer {
        for (out.items) |q| deinitQuestion(q, allocator);
        out.deinit(allocator);
    }

    var current_title: ?[]const u8 = null;
    var body_lines: std.ArrayList([]const u8) = .empty;
    defer body_lines.deinit(allocator);

    const flush = struct {
        fn call(
            list: *std.ArrayList(Question),
            alloc: std.mem.Allocator,
            title_owned: *?[]const u8,
            blines: *std.ArrayList([]const u8),
        ) std.mem.Allocator.Error!void {
            if (title_owned.*) |t| {
                const full_body = try joinTrimmedBody(alloc, blines.items);
                const resolution = try extractResolution(alloc, blines.items);
                try list.append(alloc, .{ .title = t, .body = full_body, .resolution = resolution });
                title_owned.* = null;
            }
            blines.clearRetainingCapacity();
        }
    }.call;

    for (section) |line| {
        const trimmed = trimRight(line);
        if (std.mem.startsWith(u8, trimmed, "### ")) {
            try flush(&out, allocator, &current_title, &body_lines);
            const title = std.mem.trim(u8, trimmed[4..], " \t");
            current_title = try allocator.dupe(u8, title);
            continue;
        }
        if (current_title != null) {
            try body_lines.append(allocator, line);
        }
    }
    try flush(&out, allocator, &current_title, &body_lines);

    return try out.toOwnedSlice(allocator);
}

/// Parse the full roadmap body into a flat list of milestones. Each H2
/// starts a new milestone; the first non-blank paragraph below the H2 is
/// the intent; bulleted lines are work items.
pub fn parseRoadmap(
    allocator: std.mem.Allocator,
    body: []const u8,
) std.mem.Allocator.Error![]const Milestone {
    const lines = try splitLines(allocator, body);
    defer allocator.free(lines);

    var out: std.ArrayList(Milestone) = .empty;
    errdefer {
        for (out.items) |m| deinitMilestone(m, allocator);
        out.deinit(allocator);
    }

    var name: ?[]const u8 = null;
    var intent_buf: std.ArrayList(u8) = .empty;
    defer intent_buf.deinit(allocator);
    var work_items: std.ArrayList(WorkItem) = .empty;
    errdefer {
        for (work_items.items) |w| deinitWorkItem(w, allocator);
        work_items.deinit(allocator);
    }
    var in_intent = false;
    var intent_done = false;

    const flush = struct {
        fn call(
            list: *std.ArrayList(Milestone),
            alloc: std.mem.Allocator,
            name_owned: *?[]const u8,
            intent_owned: *std.ArrayList(u8),
            items_owned: *std.ArrayList(WorkItem),
            in_intent_p: *bool,
            intent_done_p: *bool,
        ) std.mem.Allocator.Error!void {
            if (name_owned.*) |n| {
                const intent_copy = try alloc.dupe(u8, intent_owned.items);
                const items_slice = try items_owned.toOwnedSlice(alloc);
                try list.append(alloc, .{ .name = n, .intent = intent_copy, .work_items = items_slice });
                name_owned.* = null;
            }
            intent_owned.clearRetainingCapacity();
            in_intent_p.* = false;
            intent_done_p.* = false;
        }
    }.call;

    var idx: usize = 0;
    while (idx < lines.len) : (idx += 1) {
        const line = lines[idx];
        const trimmed = trimRight(line);
        // H2 (not H3) starts a new milestone.
        if (std.mem.startsWith(u8, trimmed, "## ") and !std.mem.startsWith(u8, trimmed, "### ")) {
            try flush(&out, allocator, &name, &intent_buf, &work_items, &in_intent, &intent_done);
            const n = std.mem.trim(u8, trimmed[3..], " \t");
            name = try allocator.dupe(u8, n);
            in_intent = true;
            intent_done = false;
            continue;
        }
        if (name == null) continue;

        if (isBullet(trimmed)) {
            intent_done = true;
            in_intent = false;
            // Fold continuation lines (indented, non-bullet,
            // non-empty) onto the bullet's text before parsing.
            // Markdown convention: a bullet's body continues until
            // the next bullet, the next H1/H2/H3, or a blank line.
            // Continuation lines preserve any [touches:] /
            // [slug:] annotations the operator wrote on a wrapped
            // line. Without folding, those annotations are
            // silently dropped (plan 352 task 2442).
            var merged: std.ArrayList(u8) = .empty;
            defer merged.deinit(allocator);
            try merged.appendSlice(allocator, trimmed);

            while (idx + 1 < lines.len) {
                const next = trimRight(lines[idx + 1]);
                const next_trimmed_start = std.mem.trimStart(u8, next, " \t");
                // Stop at: blank line, another bullet, or any
                // header line. Bullet detection uses the same
                // start-trimmed form so nested bullets also stop
                // the merge (they're a separate item).
                if (next_trimmed_start.len == 0) break;
                if (std.mem.startsWith(u8, next_trimmed_start, "#")) break;
                if (std.mem.startsWith(u8, next_trimmed_start, "- ") or
                    std.mem.startsWith(u8, next_trimmed_start, "* "))
                    break;
                // Require at least one leading whitespace char to
                // count as a continuation. A non-indented line
                // ends the bullet even if it has text.
                if (next.len == 0 or (next[0] != ' ' and next[0] != '\t')) break;

                try merged.append(allocator, ' ');
                try merged.appendSlice(allocator, next_trimmed_start);
                idx += 1;
            }

            const wi = try parseBullet(allocator, merged.items);
            try work_items.append(allocator, wi);
            continue;
        }

        if (in_intent and !intent_done) {
            const t = std.mem.trim(u8, trimmed, " \t");
            if (t.len == 0) {
                if (intent_buf.items.len > 0) {
                    intent_done = true;
                    in_intent = false;
                }
                continue;
            }
            if (intent_buf.items.len > 0) try intent_buf.append(allocator, ' ');
            try intent_buf.appendSlice(allocator, t);
            continue;
        }
    }
    try flush(&out, allocator, &name, &intent_buf, &work_items, &in_intent, &intent_done);

    return try out.toOwnedSlice(allocator);
}

/// Parse the `## Scenarios` H2 section of a test-spec body.
///
/// Two grammars are recognized:
///   * **Flat H3** — `### Scenario: <title>` or `### <title>` (with or
///     without `**Verifies:**`). Always emitted as scenarios (backward-compat
///     with all existing specs). The `Scenario: ` prefix is optional.
///   * **Bucketed H4** — when a `### <group>` H3 (no `Scenario:` prefix,
///     no `**Verifies:**` line) is immediately followed by at least one
///     `#### Scenario: <title>` (or `#### <title>` with `**Verifies:**`)
///     child H4, the H3 is treated as a bucket-group header and SKIPPED;
///     its H4 children are emitted as scenarios instead.
///
/// The distinguishing rule: an H3 is a **bucket** only when it has H4
/// children AND neither the `Scenario:` prefix nor `**Verifies:**`. An H3
/// with no H4 children is always emitted as a scenario (backward-compat).
///
/// Kind / Acceptance / Verifies field lines are lifted from the body of
/// whichever heading (H3 or H4) owns the scenario.
///
/// Returns an empty slice when no `## Scenarios` section exists.
pub fn parseTestSpec(
    allocator: std.mem.Allocator,
    body: []const u8,
) std.mem.Allocator.Error![]const Scenario {
    const lines = try splitLines(allocator, body);
    defer allocator.free(lines);

    const section = sliceSection(lines, "## Scenarios") orelse return &.{};

    var out: std.ArrayList(Scenario) = .empty;
    errdefer {
        for (out.items) |s| deinitScenario(s, allocator);
        out.deinit(allocator);
    }

    // State machine overview:
    //
    // We process the section in one forward pass. The pending-slot holds
    // the tentative state for one open heading (H3 or H4). When we hit the
    // next H3/H4 we decide whether to emit or discard the pending slot:
    //
    //   • An H3 slot is emitted unconditionally UNLESS:
    //       - it has no `Scenario:` prefix, AND
    //       - it has no `**Verifies:**`, AND
    //       - we encountered at least one H4 child while open (= it is a
    //         bucket header).
    //     In the bucket case the H3 slot is discarded and `in_bucket_h3`
    //     stays true so subsequent H4 children know they are top-level
    //     scenarios for the output.
    //
    //   • An H4 slot is emitted if it has the `Scenario:` prefix OR has
    //     `**Verifies:**`; otherwise discarded (bare H4 with no signal is
    //     an unrecognized sub-item, not a scenario).

    var current_title: ?[]const u8 = null; // owned
    var current_kind: []const u8 = &.{}; // owned
    var current_acceptance: []const u8 = &.{}; // owned
    var current_verifies: std.ArrayList(TaskRef) = .empty;
    errdefer {
        for (current_verifies.items) |r| deinitTaskRef(r, allocator);
        current_verifies.deinit(allocator);
    }
    var body_lines: std.ArrayList([]const u8) = .empty;
    defer body_lines.deinit(allocator);

    // Whether the currently-open heading carried the `Scenario: ` prefix.
    var current_has_scenario_prefix: bool = false;
    // Whether the currently-open slot is an H3 (true) or H4 (false).
    var current_is_h3: bool = false;
    // Whether we are inside a bucket-group H3 body. H4 items encountered
    // here open their own tentative slot.
    var in_bucket_h3: bool = false;

    // flushPending: emit the pending slot if it qualifies, otherwise discard.
    // `triggered_by_h4` is true when the flush was triggered because we
    // encountered an H4 child — this is needed to correctly decide whether
    // the pending H3 is a bucket.
    const FlushCtx = struct {
        list: *std.ArrayList(Scenario),
        alloc: std.mem.Allocator,
        title_p: *?[]const u8,
        kind_p: *[]const u8,
        acceptance_p: *[]const u8,
        verifies_p: *std.ArrayList(TaskRef),
        blines: *std.ArrayList([]const u8),
        has_prefix_p: *bool,
        is_h3_p: *bool,
        in_bucket_p: *bool,

        fn flush(ctx: @This(), triggered_by_h4: bool) std.mem.Allocator.Error!void {
            if (ctx.title_p.*) |t| {
                // Determine whether the pending slot should be emitted.
                // For H3: emit always, EXCEPT when it is a bucket (no prefix,
                // no Verifies, AND has H4 children — i.e., flush was triggered
                // by the first H4 child).
                // For H4: emit when it has prefix OR Verifies.
                const emit = blk: {
                    if (ctx.is_h3_p.*) {
                        const is_bucket = triggered_by_h4 and
                            !ctx.has_prefix_p.* and
                            ctx.verifies_p.items.len == 0;
                        break :blk !is_bucket;
                    } else {
                        break :blk ctx.has_prefix_p.* or ctx.verifies_p.items.len > 0;
                    }
                };

                if (emit) {
                    const body_text = try joinTrimmedBody(ctx.alloc, ctx.blines.items);
                    const verifies_slice = try ctx.verifies_p.toOwnedSlice(ctx.alloc);
                    try ctx.list.append(ctx.alloc, .{
                        .title = t,
                        .kind = ctx.kind_p.*,
                        .acceptance = ctx.acceptance_p.*,
                        .verifies = verifies_slice,
                        .body = body_text,
                    });
                    ctx.title_p.* = null;
                    ctx.kind_p.* = &.{};
                    ctx.acceptance_p.* = &.{};
                    // If this was a bucket H3 being emitted (shouldn't happen
                    // but guard anyway), clear bucket flag.
                    ctx.in_bucket_p.* = false;
                } else {
                    // Discard the slot — either a bucket H3 or bare H4.
                    ctx.alloc.free(t);
                    ctx.title_p.* = null;
                    ctx.alloc.free(ctx.kind_p.*);
                    ctx.kind_p.* = &.{};
                    ctx.alloc.free(ctx.acceptance_p.*);
                    ctx.acceptance_p.* = &.{};
                    for (ctx.verifies_p.items) |r| deinitTaskRef(r, ctx.alloc);
                    ctx.verifies_p.clearRetainingCapacity();
                    // When an H3 is discarded as a bucket, mark that state.
                    if (ctx.is_h3_p.* and triggered_by_h4) {
                        ctx.in_bucket_p.* = true;
                    }
                }
            }
            ctx.has_prefix_p.* = false;
            ctx.is_h3_p.* = false;
            ctx.blines.clearRetainingCapacity();
        }
    };

    var fctx: FlushCtx = .{
        .list = &out,
        .alloc = allocator,
        .title_p = &current_title,
        .kind_p = &current_kind,
        .acceptance_p = &current_acceptance,
        .verifies_p = &current_verifies,
        .blines = &body_lines,
        .has_prefix_p = &current_has_scenario_prefix,
        .is_h3_p = &current_is_h3,
        .in_bucket_p = &in_bucket_h3,
    };

    for (section) |line| {
        const trimmed = trimRight(line);

        if (std.mem.startsWith(u8, trimmed, "#### ")) {
            // H4 heading. If there's an open H3 slot, flush it with
            // `triggered_by_h4 = true` so the bucket check fires.
            // If there's an open H4 slot, flush it with triggered_by_h4 = false
            // (H4→H4 handoff is a plain peer flush).
            const trig = current_is_h3;
            try fctx.flush(trig);

            var title = std.mem.trim(u8, trimmed[5..], " \t");
            var has_prefix = false;
            if (std.mem.startsWith(u8, title, "Scenario: ")) {
                title = std.mem.trim(u8, title[10..], " \t");
                has_prefix = true;
            }
            current_title = try allocator.dupe(u8, title);
            current_kind = try allocator.dupe(u8, "");
            current_acceptance = try allocator.dupe(u8, "");
            current_has_scenario_prefix = has_prefix;
            current_is_h3 = false;
            continue;
        }

        if (std.mem.startsWith(u8, trimmed, "### ")) {
            // H3 heading — flush whatever is pending without H4 trigger.
            try fctx.flush(false);
            in_bucket_h3 = false;

            var title = std.mem.trim(u8, trimmed[4..], " \t");
            var has_prefix = false;
            if (std.mem.startsWith(u8, title, "Scenario: ")) {
                title = std.mem.trim(u8, title[10..], " \t");
                has_prefix = true;
            }
            current_title = try allocator.dupe(u8, title);
            current_kind = try allocator.dupe(u8, "");
            current_acceptance = try allocator.dupe(u8, "");
            current_has_scenario_prefix = has_prefix;
            current_is_h3 = true;
            continue;
        }

        // If we have no open slot and we're not in a bucket body, skip.
        if (current_title == null and !in_bucket_h3) continue;
        if (current_title == null) continue; // in bucket but no H4 open yet

        // Field-line extraction; consumed lines do not appear in body.
        if (try stripFieldLine(allocator, trimmed, "**Verifies:**")) |val| {
            defer allocator.free(val);
            for (current_verifies.items) |r| deinitTaskRef(r, allocator);
            current_verifies.clearRetainingCapacity();
            try parseTaskRefs(allocator, val, &current_verifies);
            continue;
        }
        if (try stripFieldLine(allocator, trimmed, "**Kind:**")) |val| {
            allocator.free(current_kind);
            current_kind = val;
            continue;
        }
        if (try stripFieldLine(allocator, trimmed, "**Acceptance:**")) |val| {
            allocator.free(current_acceptance);
            current_acceptance = val;
            continue;
        }
        try body_lines.append(allocator, line);
    }
    // Final flush — not triggered by H4.
    try fctx.flush(false);

    return try out.toOwnedSlice(allocator);
}

// =========================================================================
// Internals
// =========================================================================

fn splitLines(allocator: std.mem.Allocator, s: []const u8) std.mem.Allocator.Error![][]const u8 {
    var out: std.ArrayList([]const u8) = .empty;
    errdefer out.deinit(allocator);
    var start: usize = 0;
    var i: usize = 0;
    while (i < s.len) : (i += 1) {
        if (s[i] == '\n') {
            try out.append(allocator, s[start..i]);
            start = i + 1;
        }
    }
    try out.append(allocator, s[start..]);
    return try out.toOwnedSlice(allocator);
}

fn trimRight(line: []const u8) []const u8 {
    return std.mem.trimEnd(u8, line, " \t");
}

fn sliceSection(lines: [][]const u8, header: []const u8) ?[]const []const u8 {
    var start: ?usize = null;
    var end: usize = lines.len;
    for (lines, 0..) |line, i| {
        const t = trimRight(line);
        if (start == null and std.mem.eql(u8, t, header)) {
            start = i + 1;
            continue;
        }
        if (start != null and std.mem.startsWith(u8, t, "## ")) {
            end = i;
            break;
        }
    }
    if (start) |s| return lines[s..end];
    return null;
}

/// sectionHasContent returns true when the named H2 section exists AND
/// contains at least one non-blank line (i.e., has bullet or paragraph
/// content beyond the section header itself). Used by the ingest layer
/// to detect populated sections that nonetheless produced 0 parsed entities,
/// so it can emit a loud warning instead of silently discarding content.
pub fn sectionHasContent(body: []const u8, header: []const u8) bool {
    // Fast path: section doesn't exist at all.
    if (std.mem.indexOf(u8, body, header) == null) return false;

    // Walk line-by-line to find the section boundary and check for content.
    var in_section = false;
    var start: usize = 0;
    var i: usize = 0;
    while (i <= body.len) : (i += 1) {
        const at_newline = (i == body.len or body[i] == '\n');
        if (!at_newline) continue;

        const line = body[start..i];
        const t = std.mem.trimEnd(u8, line, " \t");
        start = i + 1;

        if (!in_section) {
            if (std.mem.eql(u8, t, header)) {
                in_section = true;
            }
            continue;
        }

        // End of our section when another H2 (but not H3+) starts.
        if (std.mem.startsWith(u8, t, "## ") and !std.mem.startsWith(u8, t, "### ")) {
            return false;
        }

        // Any non-blank line in the section counts as content.
        if (t.len > 0) return true;
    }
    return false;
}

fn joinTrimmedBody(allocator: std.mem.Allocator, lines: []const []const u8) std.mem.Allocator.Error![]const u8 {
    if (lines.len == 0) return try allocator.dupe(u8, "");
    // Concatenate with '\n', then trim surrounding whitespace.
    var total: usize = 0;
    for (lines, 0..) |line, i| {
        total += line.len;
        if (i + 1 < lines.len) total += 1; // newline
    }
    var buf = try allocator.alloc(u8, total);
    var off: usize = 0;
    for (lines, 0..) |line, i| {
        @memcpy(buf[off..][0..line.len], line);
        off += line.len;
        if (i + 1 < lines.len) {
            buf[off] = '\n';
            off += 1;
        }
    }
    const trimmed = std.mem.trim(u8, buf, " \t\n\r");
    const out = try allocator.dupe(u8, trimmed);
    allocator.free(buf);
    return out;
}

fn extractResolution(
    allocator: std.mem.Allocator,
    body_lines: []const []const u8,
) std.mem.Allocator.Error![]const u8 {
    // Find first non-blank line.
    var idx: ?usize = null;
    for (body_lines, 0..) |line, i| {
        const t = std.mem.trim(u8, line, " \t");
        if (t.len != 0) {
            idx = i;
            break;
        }
    }
    const i = idx orelse return try allocator.dupe(u8, "");
    const first = trimRight(body_lines[i]);
    if (!std.mem.startsWith(u8, first, "Resolution:")) return try allocator.dupe(u8, "");

    var pieces: std.ArrayList([]const u8) = .empty;
    defer pieces.deinit(allocator);

    const after = std.mem.trim(u8, first[11..], " \t");
    if (after.len != 0) try pieces.append(allocator, after);
    for (body_lines[i + 1 ..]) |line| {
        try pieces.append(allocator, trimRight(line));
    }
    // Join with '\n' and trim.
    var total: usize = 0;
    for (pieces.items, 0..) |p, j| {
        total += p.len;
        if (j + 1 < pieces.items.len) total += 1;
    }
    var buf = try allocator.alloc(u8, total);
    var off: usize = 0;
    for (pieces.items, 0..) |p, j| {
        @memcpy(buf[off..][0..p.len], p);
        off += p.len;
        if (j + 1 < pieces.items.len) {
            buf[off] = '\n';
            off += 1;
        }
    }
    const trimmed = std.mem.trim(u8, buf, " \t\n\r");
    const out = try allocator.dupe(u8, trimmed);
    allocator.free(buf);
    return out;
}

fn isBullet(line: []const u8) bool {
    const t = std.mem.trimStart(u8, line, " \t");
    return std.mem.startsWith(u8, t, "- ") or std.mem.startsWith(u8, t, "* ");
}

fn parseBullet(allocator: std.mem.Allocator, line: []const u8) std.mem.Allocator.Error!WorkItem {
    var raw = std.mem.trimStart(u8, line, " \t");
    if (std.mem.startsWith(u8, raw, "- ")) raw = raw[2..] else if (std.mem.startsWith(u8, raw, "* ")) raw = raw[2..];

    var title = std.mem.trim(u8, raw, " \t");
    var touches: std.ArrayList([]const u8) = .empty;
    errdefer {
        for (touches.items) |s| allocator.free(s);
        touches.deinit(allocator);
    }
    var slug_buf: []const u8 = "";

    // `[touches: ...]` — search from the right; tolerate one annotation.
    if (std.mem.lastIndexOf(u8, title, "[touches:")) |idx| {
        if (std.mem.indexOf(u8, title[idx..], "]")) |close_off| {
            const inner = std.mem.trim(u8, title[idx + "[touches:".len .. idx + close_off], " \t");
            // Split on commas.
            var it = std.mem.splitScalar(u8, inner, ',');
            while (it.next()) |part| {
                const t = std.mem.trim(u8, part, " \t");
                if (t.len != 0) try touches.append(allocator, try allocator.dupe(u8, t));
            }
            // Strip annotation from title (preserve surrounding spacing).
            var rebuilt: std.ArrayList(u8) = .empty;
            defer rebuilt.deinit(allocator);
            try rebuilt.appendSlice(allocator, title[0..idx]);
            try rebuilt.appendSlice(allocator, title[idx + close_off + 1 ..]);
            const trimmed = std.mem.trim(u8, rebuilt.items, " \t");
            // We can't keep title pointing into `rebuilt` past its scope;
            // dupe out into a temporary buffer owned by the function arena.
            const dup = try allocator.dupe(u8, trimmed);
            // Stash for later; we'll free the previous title-string view
            // at function exit by storing dup as the canonical title.
            title = dup;
            // We must take care to free `dup` on later branches' error
            // paths — push it onto a defer with a stable label.
            // Realistically the next step's errdefer covers this since
            // `title` is the source for the final dupe at function end.
            // We free it explicitly via the consolidated cleanup below.
        }
    }
    var title_needs_free = !std.mem.eql(u8, title, std.mem.trim(u8, raw, " \t"));

    // `[slug: ...]`.
    if (std.mem.lastIndexOf(u8, title, "[slug:")) |idx| {
        if (std.mem.indexOf(u8, title[idx..], "]")) |close_off| {
            const inner = title[idx + "[slug:".len .. idx + close_off];
            slug_buf = try sanitizeSlug(allocator, inner);
            // Strip annotation from title.
            var rebuilt: std.ArrayList(u8) = .empty;
            defer rebuilt.deinit(allocator);
            try rebuilt.appendSlice(allocator, title[0..idx]);
            try rebuilt.appendSlice(allocator, title[idx + close_off + 1 ..]);
            const trimmed = std.mem.trim(u8, rebuilt.items, " \t");
            const dup = try allocator.dupe(u8, trimmed);
            if (title_needs_free) allocator.free(title);
            title = dup;
            title_needs_free = true;
        }
    }

    const title_final = try allocator.dupe(u8, title);
    if (title_needs_free) allocator.free(title);
    const touches_slice = try touches.toOwnedSlice(allocator);
    return .{ .title = title_final, .touches = touches_slice, .slug = slug_buf };
}

/// sanitizeSlug normalizes a slug annotation value: trim, lowercase ASCII,
/// replace runs of non-`[a-z0-9]` with `-`, strip leading/trailing `-`.
/// Empty input → empty result (caller treats as no-slug).
pub fn sanitizeSlug(allocator: std.mem.Allocator, raw: []const u8) std.mem.Allocator.Error![]const u8 {
    const t = std.mem.trim(u8, raw, " \t");
    if (t.len == 0) return try allocator.dupe(u8, "");

    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(allocator);
    var prev_dash = true; // suppress leading dashes
    for (t) |c| {
        if (c >= 'A' and c <= 'Z') {
            try buf.append(allocator, c + ('a' - 'A'));
            prev_dash = false;
        } else if ((c >= 'a' and c <= 'z') or (c >= '0' and c <= '9')) {
            try buf.append(allocator, c);
            prev_dash = false;
        } else {
            if (!prev_dash) {
                try buf.append(allocator, '-');
                prev_dash = true;
            }
        }
    }
    // Trim trailing '-'.
    while (buf.items.len > 0 and buf.items[buf.items.len - 1] == '-') {
        _ = buf.pop();
    }
    return try allocator.dupe(u8, buf.items);
}

/// stripFieldLine returns the trimmed value after `prefix:` when a line
/// (post-trim) starts with the literal `**Field:**` token. Caller owns
/// the returned string. Returns null when the prefix is absent.
fn stripFieldLine(
    allocator: std.mem.Allocator,
    line: []const u8,
    prefix: []const u8,
) std.mem.Allocator.Error!?[]const u8 {
    const t = std.mem.trim(u8, line, " \t");
    if (!std.mem.startsWith(u8, t, prefix)) return null;
    const after = std.mem.trim(u8, t[prefix.len..], " \t");
    return try allocator.dupe(u8, after);
}

/// parseTaskRefs splits a comma-separated `**Verifies:**` value into
/// TaskRef entries. Each entry is one of:
///   * bare positive integer (kind defaults to "task"),
///   * `<kind>:<int>`,
///   * `<kind>:<slug>` where the trailing token is a slug-looking token.
/// Malformed entries are silently dropped.
fn parseTaskRefs(
    allocator: std.mem.Allocator,
    value: []const u8,
    out: *std.ArrayList(TaskRef),
) std.mem.Allocator.Error!void {
    if (value.len == 0) return;
    var it = std.mem.splitScalar(u8, value, ',');
    while (it.next()) |raw| {
        const t = std.mem.trim(u8, raw, " \t");
        if (t.len == 0) continue;
        var kind_str: []const u8 = "task";
        var val_str: []const u8 = t;
        if (std.mem.indexOfScalar(u8, t, ':')) |colon_idx| {
            kind_str = std.mem.trim(u8, t[0..colon_idx], " \t");
            val_str = std.mem.trim(u8, t[colon_idx + 1 ..], " \t");
        }
        if (kind_str.len == 0 or val_str.len == 0) continue;

        // Try numeric.
        if (std.fmt.parseInt(i64, val_str, 10)) |id| {
            if (id <= 0) continue;
            try out.append(allocator, .{
                .kind = try allocator.dupe(u8, kind_str),
                .id = id,
                .slug = try allocator.dupe(u8, ""),
            });
            continue;
        } else |_| {}

        // Slug form — accept ASCII letters, digits, '-', '_'.
        if (!looksLikeSlug(val_str)) continue;
        try out.append(allocator, .{
            .kind = try allocator.dupe(u8, kind_str),
            .id = 0,
            .slug = try allocator.dupe(u8, val_str),
        });
    }
}

fn looksLikeSlug(s: []const u8) bool {
    if (s.len == 0) return false;
    for (s) |c| {
        if (!((c >= 'a' and c <= 'z') or
            (c >= 'A' and c <= 'Z') or
            (c >= '0' and c <= '9') or
            c == '-' or c == '_'))
        {
            return false;
        }
    }
    return true;
}

// =========================================================================
// Tests
// =========================================================================

const testing = std.testing;

test "sanitizeSlug: trims, lowercases, collapses separators" {
    const cases = [_]struct { in: []const u8, want: []const u8 }{
        .{ .in = "  Hello World  ", .want = "hello-world" },
        .{ .in = "foo--bar", .want = "foo-bar" },
        .{ .in = "FOO_BAR", .want = "foo-bar" },
        .{ .in = "-leading-", .want = "leading" },
        .{ .in = "", .want = "" },
        .{ .in = "   ", .want = "" },
    };
    for (cases) |c| {
        const out = try sanitizeSlug(testing.allocator, c.in);
        defer testing.allocator.free(out);
        try testing.expectEqualStrings(c.want, out);
    }
}

test "parseTechSpecDecisions: extracts H3-rooted decisions" {
    const body =
        \\## Decisions
        \\
        \\### Use SQLite
        \\
        \\Chosen for embedded simplicity.
        \\
        \\### Avoid cgo
        \\
        \\Pure-Go driver.
        \\
        \\## Open Questions
        \\
        \\### Some question
    ;
    const decs = try parseTechSpecDecisions(testing.allocator, body);
    defer deinitDecisions(decs, testing.allocator);

    try testing.expectEqual(@as(usize, 2), decs.len);
    try testing.expectEqualStrings("Use SQLite", decs[0].title);
    try testing.expectEqualStrings("Chosen for embedded simplicity.", decs[0].body);
    try testing.expectEqualStrings("Avoid cgo", decs[1].title);
    try testing.expectEqualStrings("Pure-Go driver.", decs[1].body);
}

test "parseTechSpecDecisions: returns empty when no section" {
    const out = try parseTechSpecDecisions(testing.allocator, "no decisions here\n");
    defer deinitDecisions(out, testing.allocator);
    try testing.expectEqual(@as(usize, 0), out.len);
}

test "parseTechSpecOpenQuestions: extracts H3 and resolution" {
    const body =
        \\## Open Questions
        \\
        \\### Q1: how to handle X?
        \\
        \\Some body text.
        \\
        \\### Q2: resolved already
        \\
        \\Resolution: use option A
        \\with extra context.
    ;
    const qs = try parseTechSpecOpenQuestions(testing.allocator, body);
    defer deinitQuestions(qs, testing.allocator);

    try testing.expectEqual(@as(usize, 2), qs.len);
    try testing.expectEqualStrings("Q1: how to handle X?", qs[0].title);
    try testing.expectEqualStrings("Some body text.", qs[0].body);
    try testing.expectEqualStrings("", qs[0].resolution);
    try testing.expectEqualStrings("Q2: resolved already", qs[1].title);
    try testing.expectEqualStrings("use option A\nwith extra context.", qs[1].resolution);
}

test "parseRoadmap: H2 milestones with intent and bullets" {
    const body =
        \\# Roadmap
        \\
        \\## M1
        \\
        \\First milestone intent paragraph.
        \\
        \\- Add foo
        \\- Add bar [touches: a, b]
        \\- Add baz [slug: add-baz]
        \\
        \\## M2
        \\
        \\Second milestone.
        \\
        \\- Do qux
    ;
    const ms = try parseRoadmap(testing.allocator, body);
    defer deinitMilestones(ms, testing.allocator);

    try testing.expectEqual(@as(usize, 2), ms.len);
    try testing.expectEqualStrings("M1", ms[0].name);
    try testing.expectEqualStrings("First milestone intent paragraph.", ms[0].intent);
    try testing.expectEqual(@as(usize, 3), ms[0].work_items.len);
    try testing.expectEqualStrings("Add foo", ms[0].work_items[0].title);
    try testing.expectEqualStrings("Add bar", ms[0].work_items[1].title);
    try testing.expectEqual(@as(usize, 2), ms[0].work_items[1].touches.len);
    try testing.expectEqualStrings("a", ms[0].work_items[1].touches[0]);
    try testing.expectEqualStrings("b", ms[0].work_items[1].touches[1]);
    try testing.expectEqualStrings("Add baz", ms[0].work_items[2].title);
    try testing.expectEqualStrings("add-baz", ms[0].work_items[2].slug);
    try testing.expectEqualStrings("M2", ms[1].name);
    try testing.expectEqual(@as(usize, 1), ms[1].work_items.len);
}

test "parseRoadmap: multi-line bullets fold continuation lines and surface trailing [slug:] / [touches:]" {
    // Real operator markdown wraps bullets across lines; without
    // folding, `[slug: ...]` on a continuation line is silently
    // dropped. Plan 352 task 2442 surfaced this against the
    // scenario-coverage roadmap.
    const body =
        \\## M1
        \\
        \\- First bullet runs across two
        \\  lines and the slug sits on the second. [slug: m1-first]
        \\- Second bullet has the touches
        \\  on a continuation. [touches: alpha, beta]
        \\- Third bullet single-line only [slug: m1-third]
    ;
    const ms = try parseRoadmap(testing.allocator, body);
    defer deinitMilestones(ms, testing.allocator);

    try testing.expectEqual(@as(usize, 1), ms.len);
    try testing.expectEqual(@as(usize, 3), ms[0].work_items.len);

    // Bullet 1: title is the merged text minus the [slug:].
    try testing.expectEqualStrings(
        "First bullet runs across two lines and the slug sits on the second.",
        ms[0].work_items[0].title,
    );
    try testing.expectEqualStrings("m1-first", ms[0].work_items[0].slug);

    // Bullet 2: touches on the continuation.
    try testing.expectEqualStrings(
        "Second bullet has the touches on a continuation.",
        ms[0].work_items[1].title,
    );
    try testing.expectEqual(@as(usize, 2), ms[0].work_items[1].touches.len);
    try testing.expectEqualStrings("alpha", ms[0].work_items[1].touches[0]);
    try testing.expectEqualStrings("beta", ms[0].work_items[1].touches[1]);

    // Bullet 3: single-line baseline unaffected.
    try testing.expectEqualStrings("Third bullet single-line only", ms[0].work_items[2].title);
    try testing.expectEqualStrings("m1-third", ms[0].work_items[2].slug);
}

test "parseTestSpec: scenarios with verifies, kind, acceptance" {
    const body =
        \\## Scenarios
        \\
        \\### Scenario: happy path
        \\
        \\**Verifies:** task:add-foo, task:42
        \\**Kind:** integration
        \\**Acceptance:** binary exits 0
        \\
        \\Prose body line one.
        \\Prose body line two.
        \\
        \\### plain title (no prefix)
        \\
        \\**Verifies:** task:add-bar
    ;
    const scs = try parseTestSpec(testing.allocator, body);
    defer deinitScenarios(scs, testing.allocator);

    try testing.expectEqual(@as(usize, 2), scs.len);
    try testing.expectEqualStrings("happy path", scs[0].title);
    try testing.expectEqualStrings("integration", scs[0].kind);
    try testing.expectEqualStrings("binary exits 0", scs[0].acceptance);
    try testing.expectEqual(@as(usize, 2), scs[0].verifies.len);
    try testing.expectEqualStrings("task", scs[0].verifies[0].kind);
    try testing.expectEqualStrings("add-foo", scs[0].verifies[0].slug);
    try testing.expectEqual(@as(i64, 0), scs[0].verifies[0].id);
    try testing.expectEqualStrings("task", scs[0].verifies[1].kind);
    try testing.expectEqualStrings("", scs[0].verifies[1].slug);
    try testing.expectEqual(@as(i64, 42), scs[0].verifies[1].id);
    try testing.expectEqualStrings("Prose body line one.\nProse body line two.", scs[0].body);

    try testing.expectEqualStrings("plain title (no prefix)", scs[1].title);
    try testing.expectEqual(@as(usize, 1), scs[1].verifies.len);
    try testing.expectEqualStrings("add-bar", scs[1].verifies[0].slug);
    try testing.expectEqualStrings("", scs[1].body);
}

test "parseTestSpec: missing verifies leaves verifies empty" {
    const body =
        \\## Scenarios
        \\
        \\### No verifies here
        \\
        \\**Kind:** unit
        \\
        \\Body only.
    ;
    const scs = try parseTestSpec(testing.allocator, body);
    defer deinitScenarios(scs, testing.allocator);

    try testing.expectEqual(@as(usize, 1), scs.len);
    try testing.expectEqual(@as(usize, 0), scs[0].verifies.len);
    try testing.expectEqualStrings("unit", scs[0].kind);
    try testing.expectEqualStrings("Body only.", scs[0].body);
}

test "parseTestSpec: malformed verifies entries silently dropped" {
    const body =
        \\## Scenarios
        \\
        \\### Adversarial
        \\
        \\**Verifies:** task:valid, , task:, :bad, task:good-slug, task:-1, task:0, task:5
    ;
    const scs = try parseTestSpec(testing.allocator, body);
    defer deinitScenarios(scs, testing.allocator);

    try testing.expectEqual(@as(usize, 1), scs.len);
    // Survivors: task:valid (slug), task:good-slug (slug), task:5 (id).
    // -1 fails as a slug (not slug-looking) AND fails as id (<=0 dropped).
    // Actually -1 is numeric-parseable but rejected via id <= 0 check.
    try testing.expectEqual(@as(usize, 3), scs[0].verifies.len);
    try testing.expectEqualStrings("valid", scs[0].verifies[0].slug);
    try testing.expectEqualStrings("good-slug", scs[0].verifies[1].slug);
    try testing.expectEqual(@as(i64, 5), scs[0].verifies[2].id);
}

// =========================================================================
// Red tests (GitHub #87 + #30): must FAIL against the unfixed parser.
// These pin the contract after the fix lands.
// =========================================================================

// (a) Bulleted `## Decisions` fallback: when there are no H3 headings but
// there are bullet items, each bullet becomes a Decision.
test "parseTechSpecDecisions: bulleted fallback when no H3 headings" {
    const body =
        \\## Decisions
        \\
        \\- **Use SQLite.** Chosen for embedded simplicity and zero deps.
        \\- **Avoid cgo.** Pure-Go driver keeps cross-compilation easy.
        \\
        \\## Open Questions
        \\
        \\### Some question
    ;
    const decs = try parseTechSpecDecisions(testing.allocator, body);
    defer deinitDecisions(decs, testing.allocator);

    // Expect 2 decisions parsed from bullets, NOT 0.
    try testing.expectEqual(@as(usize, 2), decs.len);
    try testing.expectEqualStrings("Use SQLite.", decs[0].title);
    try testing.expectEqualStrings("Avoid cgo.", decs[1].title);
}

// (b) When both H3 headings AND bullets are present in `## Decisions`,
// the H3 path is preferred (H3 wins).
test "parseTechSpecDecisions: H3 preferred over bullets when both present" {
    const body =
        \\## Decisions
        \\
        \\- **Bullet decision.** Should be ignored when H3 present.
        \\
        \\### Proper H3 Decision
        \\
        \\Body text here.
    ;
    const decs = try parseTechSpecDecisions(testing.allocator, body);
    defer deinitDecisions(decs, testing.allocator);

    // H3 path: exactly 1 decision from the H3, not from the bullet.
    try testing.expectEqual(@as(usize, 1), decs.len);
    try testing.expectEqualStrings("Proper H3 Decision", decs[0].title);
}

// (c) `#### Scenario:` H4 items nested under a `### <bucket>` H3 group
// header must be extracted as scenarios. The bucket H3 itself must NOT
// appear as a scenario.
test "parseTestSpec: H4 scenarios under bucket H3 group header" {
    const body =
        \\## Scenarios
        \\
        \\### Happy paths
        \\
        \\#### Scenario: add item succeeds
        \\
        \\**Verifies:** task:add-item
        \\**Kind:** integration
        \\**Acceptance:** exit 0
        \\
        \\#### Scenario: list items returns all
        \\
        \\**Verifies:** task:list-items
        \\**Kind:** integration
        \\**Acceptance:** JSON array with expected count
        \\
        \\### Errors
        \\
        \\#### Scenario: invalid input rejected
        \\
        \\**Verifies:** task:validate-input
        \\**Kind:** unit
        \\**Acceptance:** exit 1
    ;
    const scs = try parseTestSpec(testing.allocator, body);
    defer deinitScenarios(scs, testing.allocator);

    // Expect 3 real scenarios (from H4), NOT 2 bucket headers + 0 H4 scenarios.
    try testing.expectEqual(@as(usize, 3), scs.len);

    // Verify none of the scenarios have bucket-header names.
    for (scs) |s| {
        try testing.expect(!std.mem.eql(u8, s.title, "Happy paths"));
        try testing.expect(!std.mem.eql(u8, s.title, "Errors"));
    }

    try testing.expectEqualStrings("add item succeeds", scs[0].title);
    try testing.expectEqual(@as(usize, 1), scs[0].verifies.len);
    try testing.expectEqualStrings("add-item", scs[0].verifies[0].slug);

    try testing.expectEqualStrings("list items returns all", scs[1].title);
    try testing.expectEqualStrings("invalid input rejected", scs[2].title);
}

// (d) Canonical flat `### Scenario: <title>` H3 still extracts correctly
// (backward-compat: existing valid scenarios are unaffected).
test "parseTestSpec: canonical flat H3 Scenario prefix still extracts" {
    const body =
        \\## Scenarios
        \\
        \\### Scenario: happy path
        \\
        \\**Verifies:** task:do-thing
        \\**Kind:** integration
        \\**Acceptance:** exit 0
        \\
        \\### Scenario: error path
        \\
        \\**Verifies:** task:do-thing
        \\**Kind:** unit
        \\**Acceptance:** exit 1
    ;
    const scs = try parseTestSpec(testing.allocator, body);
    defer deinitScenarios(scs, testing.allocator);

    try testing.expectEqual(@as(usize, 2), scs.len);
    try testing.expectEqualStrings("happy path", scs[0].title);
    try testing.expectEqualStrings("error path", scs[1].title);
}

// (e) An H3 without `Scenario:` prefix that carries `**Verifies:**` is
// treated as a scenario (prefix-optional backward-compat path).
test "parseTestSpec: H3 without Scenario prefix but with Verifies is a scenario" {
    const body =
        \\## Scenarios
        \\
        \\### plain title no prefix
        \\
        \\**Verifies:** task:foo-bar
        \\**Kind:** unit
        \\**Acceptance:** passes
    ;
    const scs = try parseTestSpec(testing.allocator, body);
    defer deinitScenarios(scs, testing.allocator);

    try testing.expectEqual(@as(usize, 1), scs.len);
    try testing.expectEqualStrings("plain title no prefix", scs[0].title);
    try testing.expectEqual(@as(usize, 1), scs[0].verifies.len);
}

// (f) A bucket-group H3 that has NO Scenario: prefix AND no **Verifies:**
// line, but contains H4 Scenario: children, must NOT appear as a scenario.
test "parseTestSpec: bucket H3 without Scenario prefix and no Verifies is skipped" {
    const body =
        \\## Scenarios
        \\
        \\### Error cases
        \\
        \\Some descriptive text about this group.
        \\
        \\#### Scenario: network failure
        \\
        \\**Verifies:** task:handle-network-error
        \\**Kind:** integration
        \\**Acceptance:** graceful degradation
    ;
    const scs = try parseTestSpec(testing.allocator, body);
    defer deinitScenarios(scs, testing.allocator);

    // Only the H4 scenario should be extracted; the H3 "Error cases" must be skipped.
    try testing.expectEqual(@as(usize, 1), scs.len);
    try testing.expect(!std.mem.eql(u8, scs[0].title, "Error cases"));
    try testing.expectEqualStrings("network failure", scs[0].title);
}
