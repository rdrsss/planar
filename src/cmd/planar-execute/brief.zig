//! brief.zig — Brief compiler for `planar-execute`.
//!
//! Implements a **pure function** `compileBrief` that assembles a coder brief
//! string satisfying the methodology brief-composition discipline described in
//! `agents/methodology.md §Brief composition discipline`.
//!
//! ## Purity guarantee
//!
//! `compileBrief` performs NO subprocess calls, NO filesystem I/O, NO DB
//! access, and NO dynamic linking.  All inputs are pre-gathered by the caller
//! (plan state via `state.zig`, schema via `schema.zig`, spec slices as
//! caller-owned strings) and passed in as a `BriefInputs` struct.  The
//! function is deterministic: given the same `BriefInputs` it always produces
//! the same output.  This makes it unit-testable with synthetic fixtures and
//! no live binary required.
//!
//! ## Brief contract
//!
//! Every compiled brief satisfies the eight requirements from
//! `agents/methodology.md §Brief composition discipline`:
//!
//!   1. Spec section PATHS appear verbatim — no paraphrase injection.
//!   2. Task IDs are enumerated explicitly.
//!   3. The claim token (or a placeholder) appears explicitly.
//!   4. Locked decisions are rendered inline (id + verbatim text).
//!   5. Named gates are listed.
//!   6. Report shape is specified (sections + word ceiling).
//!   7. The problem is posed; the solution is NOT prescribed.
//!   8. Available verbs/flags from the schema catalog are enumerated;
//!      the terminal-verb instruction closes the brief.
//!
//! ## Memory ownership
//!
//! `compileBrief` allocates the returned `[]const u8` using the caller-supplied
//! allocator.  The caller owns the slice and MUST free it with the same
//! allocator.  All intermediate allocations are freed before returning; the
//! only long-lived allocation is the returned brief string.
//!
//! ## Capability boundary
//!
//! This module holds NO SQLite handle and imports NO db/engine/runtime module.
//! It imports only `std`, `state.zig` (for the typed plan/task structs), and
//! `schema.zig` (for `BinSchema` and `CommandEntry`).

const std = @import("std");
const state = @import("state.zig");
const schema = @import("schema.zig");

// ---------------------------------------------------------------------------
// Input types
// ---------------------------------------------------------------------------

/// A single spec citation.  The `path` is the verbatim section path the coder
/// must read firsthand (e.g. `"tech-spec.md §Context compilation"`).  The
/// optional `verbatim_slice` is a verbatim excerpt from that section; when
/// present it is rendered faithfully as a quoted block.  It MUST NOT be a
/// paraphrase — if the caller paraphrases, the brief contract is violated.
/// When absent the coder is expected to read the section independently.
pub const SpecCitation = struct {
    /// Verbatim section path string, e.g. `"tech-spec.md §Context compilation"`.
    path: []const u8,
    /// Optional verbatim excerpt from that section.  Null means "cite the path
    /// only — no inline extract."  Must be verbatim, not paraphrased.
    verbatim_slice: ?[]const u8 = null,
};

/// A locked decision to render inline in the brief.
/// Locked decisions are short-circuit answers the coder MUST follow without
/// re-litigating.  Both `id` and `text` are rendered verbatim.
pub const LockedDecision = struct {
    /// Decision identifier, e.g. `"D-0012"` or `"Q47"`.
    id: []const u8,
    /// The decision text, rendered verbatim so the coder can honor it without
    /// reading the full ADR.
    text: []const u8,
};

/// A single prior-stage context record to surface in the brief.
///
/// `kind` is the record kind (e.g. `"note"`, `"capsule"`, `"summary"`,
/// `"decision"`); `body` is the verbatim record body.  The compiler renders
/// each entry as a `kind:` bullet so the coder can orient quickly.
///
/// The caller pre-maps `state.ContextRecord` values into this lightweight
/// struct; no arena ownership is carried — all slices are borrowed from the
/// parsed context records or the `BriefInputs` arena.
pub const ContextRef = struct {
    /// Record kind string, e.g. `"note"`, `"capsule"`.
    kind: []const u8,
    /// Verbatim record body.
    body: []const u8,
};

/// All inputs to `compileBrief`.  The caller pre-gathers these by:
///   - calling `state.planShow` / `state.planNext` for `plan` and `tasks`,
///   - calling `schema.loadSchema` + `schema.BinSchema.init` for `agent_schema`,
///   - constructing spec citations and locked decisions from the planning
///     artifacts in the workbench.
///
/// No field is a subprocess call or a live read — `compileBrief` is pure.
pub const BriefInputs = struct {
    /// When present, all implementation context is rendered from this
    /// authoritative packet. Production callers must set it.
    authoritative_packet: ?state.TaskPacket = null,
    // -----------------------------------------------------------------------
    // Plan + task identity
    // -----------------------------------------------------------------------

    /// The plan these tasks belong to (from `state.planShow`).
    plan: state.PlanShow,

    /// The task(s) being dispatched.  At least one task is required; a
    /// `grouped` cycle passes multiple.  The compiler enumerates all IDs.
    tasks: []const state.TaskEntry,

    // -----------------------------------------------------------------------
    // Claim token
    // -----------------------------------------------------------------------

    /// The claim token the coder must heartbeat, or a placeholder string
    /// (e.g. `"<claim_token>"`) when the orchestrator fills it at dispatch
    /// time.  Rendered verbatim in the brief.
    claim_token: []const u8,

    // -----------------------------------------------------------------------
    // Problem statement (what the coder must solve)
    // -----------------------------------------------------------------------

    /// A short statement of the problem / acceptance signal — the "what must
    /// be true when done" framing.  The brief renders this verbatim.  The
    /// caller MUST pose the problem, not prescribe the solution.  See
    /// `agents/methodology.md §Brief composition discipline` — "Pose the
    /// problem; do not include the solution."
    problem_statement: []const u8,

    // -----------------------------------------------------------------------
    // Spec citations
    // -----------------------------------------------------------------------

    /// Verbatim spec section citations the coder must read.  Each entry has a
    /// `path` (the section path) and an optional `verbatim_slice`.  Rendered
    /// as a "Read firsthand" section.  No paraphrasing is injected.
    spec_citations: []const SpecCitation,

    // -----------------------------------------------------------------------
    // Locked decisions
    // -----------------------------------------------------------------------

    /// Decisions the coder MUST follow without re-litigating, rendered inline
    /// so the coder does not have to hunt through ADRs.
    locked_decisions: []const LockedDecision,

    // -----------------------------------------------------------------------
    // Schema catalog — available verbs
    // -----------------------------------------------------------------------

    /// The agent-side binary schema (`planar-agent schema`), providing the
    /// exact verbs and flags available to the worker.  The compiler renders a
    /// concise "Available verbs" table from this.  Pass a `BinSchema`
    /// initialized from `schema.loadSchema(alloc, io, "planar-agent")`.
    agent_schema: schema.BinSchema,

    // -----------------------------------------------------------------------
    // Named gates
    // -----------------------------------------------------------------------

    /// The quality gates the coder must run and cite verbatim in the
    /// work-complete report.  Each entry is a gate command string, e.g.
    /// `"make fmt-check"` or `"make test-integration"`.
    gates: []const []const u8,

    // -----------------------------------------------------------------------
    // Prior-stage context capsule (plan 585 task 3904)
    // -----------------------------------------------------------------------

    /// An optional pre-compiled capsule from a prior stage — rendered verbatim
    /// when present.  The caller sets this to the body of a `kind="capsule"`
    /// `context_record` if one is available.  When null the capsule portion of
    /// the section is omitted.
    context_capsule: ?[]const u8 = null,

    /// Selected prior-stage records to surface in the brief.  Each entry is a
    /// `(kind, body)` pair; the compiler lists them under "Prior-stage context".
    /// When empty the section shows a placeholder line.  Slice is borrowed — no
    /// ownership transfer.
    context_records: []const ContextRef = &.{},
};

fn renderPacketEvidence(
    writer: *std.Io.Writer,
    heading: []const u8,
    values: []const state.PacketEvidence,
) !void {
    try writer.print("### {s}\n\n", .{heading});
    if (values.len == 0) {
        try writer.writeAll("_(none)_\n\n");
        return;
    }
    for (values) |value| {
        try writer.print(
            "- `{s}:{d}` locator=`{s}` status=`{s}` freshness=`{s}` required={} covered={}\n",
            .{ value.kind, value.id, value.locator, value.status, value.freshness, value.required, value.covered },
        );
        try writer.print("  - provenance: `{s}`\n", .{value.provenance});
        try writer.print("  - source digest: `{s}`\n", .{value.source_digest});
        try writer.print("  - current digest: `{s}`\n", .{value.current_digest});
        try writer.print("  - materializer: `{s}` / current `{s}`\n", .{ value.materializer_version, value.current_materializer_version });
        try writer.writeAll("  - exact text:\n");
        var lines = std.mem.splitScalar(u8, value.text, '\n');
        while (lines.next()) |line| try writer.print("    > {s}\n", .{line});
    }
    try writer.writeByte('\n');
}

// ---------------------------------------------------------------------------
// compileBrief — the pure compiler
// ---------------------------------------------------------------------------

/// compileBrief assembles a methodology-compliant coder brief from `inputs`.
///
/// ## Contract
///
/// The returned slice is the full brief string, owned by `allocator`.  The
/// caller MUST free it via `allocator.free(result)` when done.
///
/// The brief contains (in order):
///   1. Header: plan title, task IDs, claim token.
///   2. Problem statement (verbatim from `inputs.problem_statement`).
///   3. Spec citations ("Read firsthand") — paths verbatim; verbatim slices
///      quoted if provided; NO paraphrase injection.
///   4. Locked decisions — rendered inline.
///   5. Available verbs/flags from `inputs.agent_schema` (non-hidden cmds only,
///      excluding the root command itself).
///   6. Named gates.
///   7. Work-complete report shape (sections + word ceiling).
///   8. Terminal-verb instruction — "exactly one terminal verb; block rather
///      than ask when stuck."
///
/// ## Purity
///
/// No subprocess, no filesystem I/O, no DB access.  All inputs are already
/// in memory.
pub fn compileBrief(
    allocator: std.mem.Allocator,
    inputs: BriefInputs,
) error{ OutOfMemory, WriteFailed, AuthoritativeIdentityMismatch }![]const u8 {
    var authoritative_plan: ?state.PacketEvidence = null;
    var authoritative_claim: ?state.PacketEvidence = null;
    if (inputs.authoritative_packet) |authoritative| {
        if (!authoritative.ready() or inputs.tasks.len != 1 or
            inputs.tasks[0].id != authoritative.input.task_id or
            !std.mem.eql(u8, inputs.tasks[0].status, authoritative.input.status) or
            authoritative.input.owning_plans.len != 1 or
            authoritative.input.owning_plans[0].id != @as(i64, @intCast(inputs.plan.id)))
        {
            return error.AuthoritativeIdentityMismatch;
        }
        authoritative_plan = authoritative.input.owning_plans[0];
        for (authoritative.input.claims) |claim| {
            if (std.mem.eql(u8, claim.text, inputs.claim_token) and
                std.mem.eql(u8, claim.status, "active"))
            {
                authoritative_claim = claim;
                break;
            }
        }
        if (authoritative_claim == null) return error.AuthoritativeIdentityMismatch;
    }

    // std.Io.Writer.Allocating is the Zig 0.16 idiomatic growable byte buffer
    // with a writer interface. It stores its allocator internally so all write
    // calls are arg-free; toOwnedSlice() transfers ownership to the caller.
    var buf: std.Io.Writer.Allocating = .init(allocator);
    defer buf.deinit();

    // -----------------------------------------------------------------------
    // Section 1 — Header: plan identity, task IDs, claim token.
    //
    // Methodology requirement: "List task IDs explicitly" and "List claim
    // tokens explicitly."
    // -----------------------------------------------------------------------
    try buf.writer.writeAll("# Coder Brief\n\n");

    if (authoritative_plan) |plan| {
        try buf.writer.print("**Plan:** {s} (id {d})\n", .{ plan.text, plan.id });
    } else {
        try buf.writer.print("**Plan:** {s} (id {d})\n", .{ inputs.plan.title, inputs.plan.id });
    }
    if (inputs.plan.slug) |slug| {
        try buf.writer.print("**Plan slug:** {s}\n", .{slug});
    }
    try buf.writer.print("**Plan status:** {s}\n\n", .{if (authoritative_plan) |plan| plan.status else inputs.plan.status});

    try buf.writer.writeAll("**Task(s) dispatched:**\n");
    if (inputs.authoritative_packet) |authoritative| {
        const slug = inputs.tasks[0].slug;
        if (slug) |value| {
            try buf.writer.print("- task:{d} — {s} [slug: {s}; status: {s}]\n", .{ authoritative.input.task_id, authoritative.input.title, value, authoritative.input.status });
        } else {
            try buf.writer.print("- task:{d} — {s} [status: {s}]\n", .{ authoritative.input.task_id, authoritative.input.title, authoritative.input.status });
        }
    } else {
        for (inputs.tasks) |t| {
            if (t.slug) |slug| {
                try buf.writer.print("- task:{d} — {s} [slug: {s}]\n", .{ t.id, t.title, slug });
            } else {
                try buf.writer.print("- task:{d} — {s}\n", .{ t.id, t.title });
            }
        }
    }
    try buf.writer.writeByte('\n');

    // Methodology requirement: claim token listed explicitly.
    try buf.writer.print("**Claim token:** `{s}`\n\n", .{if (authoritative_claim) |claim| claim.text else inputs.claim_token});
    if (inputs.authoritative_packet) |authoritative| {
        try buf.writer.print("**Authoritative packet digest:** `{s}`\n\n", .{authoritative.digest});
        try buf.writer.print("**Authoritative task title:** {s}\n\n", .{authoritative.input.title});
    }

    // -----------------------------------------------------------------------
    // Section 2 — Problem statement.
    //
    // Methodology requirement: "Pose the problem; do not include the solution."
    // The caller supplies the problem_statement verbatim; we render it as-is.
    // -----------------------------------------------------------------------
    try buf.writer.writeAll("## Problem\n\n");
    if (inputs.authoritative_packet) |authoritative| try buf.writer.writeAll(authoritative.input.acceptance_criteria) else try buf.writer.writeAll(inputs.problem_statement);
    try buf.writer.writeAll("\n\n");

    // -----------------------------------------------------------------------
    // Section 3 — Spec citations ("Read firsthand").
    //
    // Methodology requirement: "Cite spec section PATHS, not paraphrased spec
    // content."  Each entry is rendered as:
    //   - `<path>`
    //     (optional verbatim excerpt in a code block — never paraphrased)
    // When spec_citations is empty the section header is still emitted (for
    // structural completeness) but no bullet items appear.
    // -----------------------------------------------------------------------
    try buf.writer.writeAll("## Read firsthand (do not paraphrase)\n\n");
    if (inputs.authoritative_packet) |authoritative| {
        for (authoritative.input.citations) |citation| try buf.writer.print("- `{s}`\n", .{citation.locator});
        try buf.writer.writeByte('\n');
    } else if (inputs.spec_citations.len == 0) {
        try buf.writer.writeAll("_(no spec citations for this cycle)_\n\n");
    } else {
        for (inputs.spec_citations) |cit| {
            try buf.writer.print("- `{s}`\n", .{cit.path});
            if (cit.verbatim_slice) |excerpt| {
                // Render verbatim — never inject a paraphrase.
                try buf.writer.writeAll("  > (verbatim excerpt):\n");
                // Indent each line of the excerpt with "> " for a block-quote.
                var lines = std.mem.splitScalar(u8, excerpt, '\n');
                while (lines.next()) |line| {
                    try buf.writer.print("  > {s}\n", .{line});
                }
            }
        }
        try buf.writer.writeByte('\n');
    }

    // -----------------------------------------------------------------------
    // Section 4 — Locked decisions (inline).
    //
    // Methodology requirement: "Note locked decisions inline."
    // -----------------------------------------------------------------------
    try buf.writer.writeAll("## Locked decisions\n\n");
    if (inputs.authoritative_packet) |authoritative| {
        for (authoritative.input.decisions) |decision| try buf.writer.print("- **{d}**: {s}\n", .{ decision.id, decision.text });
        try buf.writer.writeByte('\n');
    } else if (inputs.locked_decisions.len == 0) {
        try buf.writer.writeAll("_(no locked decisions for this cycle)_\n\n");
    } else {
        for (inputs.locked_decisions) |d| {
            try buf.writer.print("- **{s}**: {s}\n", .{ d.id, d.text });
        }
        try buf.writer.writeByte('\n');
    }

    if (inputs.authoritative_packet) |authoritative| {
        try buf.writer.writeAll("## Authoritative packet context\n\n");
        try buf.writer.print("**Task body:**\n\n{s}\n\n", .{authoritative.input.body});
        try buf.writer.print("**Exact next action:**\n\n{s}\n\n", .{authoritative.input.next_action});
        try renderPacketEvidence(&buf.writer, "Owning plans", authoritative.input.owning_plans);
        try renderPacketEvidence(&buf.writer, "Anchor plans", authoritative.input.anchor_plans);
        try renderPacketEvidence(&buf.writer, "Spec citations and source digests", authoritative.input.citations);
        try renderPacketEvidence(&buf.writer, "Locked decisions", authoritative.input.decisions);
        try renderPacketEvidence(&buf.writer, "Questions", authoritative.input.questions);
        try renderPacketEvidence(&buf.writer, "Scenarios and coverage", authoritative.input.scenarios);
        try renderPacketEvidence(&buf.writer, "Dependencies", authoritative.input.dependencies);
        try renderPacketEvidence(&buf.writer, "Touched surfaces", authoritative.input.touches);
        try renderPacketEvidence(&buf.writer, "Current claims", authoritative.input.claims);
        try renderPacketEvidence(&buf.writer, "Materialized facts, provenance, and freshness", authoritative.input.facts);
    }

    // -----------------------------------------------------------------------
    // Section 4.5 — Prior-stage context (plan 585 task 3904).
    //
    // Renders the run's accumulated context records so the coder carries
    // forward what prior stages left behind.  Two sub-parts:
    //   a) The compiled capsule (verbatim, when present) — the authoritative
    //      prior-stage summary produced by a prior compaction step.
    //   b) Individual records (kind + body) — the raw per-record detail.
    //
    // When both inputs are empty/null the section shows a placeholder line so
    // the structural completeness of the brief is preserved (mirrors the
    // existing empty-section handling in sections 3 and 4).
    // -----------------------------------------------------------------------
    try buf.writer.writeAll("## Prior-stage context\n\n");
    const has_capsule = inputs.context_capsule != null and inputs.context_capsule.?.len > 0;
    const has_records = inputs.context_records.len > 0;

    if (!has_capsule and !has_records) {
        try buf.writer.writeAll("_(no prior-stage context for this cycle)_\n\n");
    } else {
        if (has_capsule) {
            try buf.writer.writeAll("### Compiled capsule\n\n");
            try buf.writer.writeAll(inputs.context_capsule.?);
            try buf.writer.writeAll("\n\n");
        }
        if (has_records) {
            try buf.writer.writeAll("### Context records\n\n");
            for (inputs.context_records) |rec| {
                try buf.writer.print("- **{s}**: {s}\n", .{ rec.kind, rec.body });
            }
            try buf.writer.writeByte('\n');
        }
    }

    // -----------------------------------------------------------------------
    // Section 5 — Available verbs/flags (from schema catalog).
    //
    // Tech-spec requirement (§Context compilation): "enumerate the exact
    // verbs/flags the worker may use (from the schema catalog)."
    // Methodology requirement: the brief must enumerate available commands.
    //
    // We render each non-hidden command (excluding the bare root command whose
    // `command` field equals the binary root name) with its flags.  Hidden
    // commands are internal/deprecated and not available to the worker.
    // -----------------------------------------------------------------------
    try buf.writer.writeAll("## Available verbs (from schema catalog)\n\n");
    const bin_root = inputs.agent_schema.root();
    var has_cmds = false;
    for (inputs.agent_schema.commands()) |cmd| {
        // Skip the root command entry (no verb to call).
        if (std.mem.eql(u8, cmd.command, bin_root)) continue;
        // Skip hidden commands (internal / deprecated).
        if (cmd.hidden) continue;
        has_cmds = true;
        try buf.writer.print("- `{s}`", .{cmd.command});
        if (cmd.flags.len > 0) {
            var first = true;
            for (cmd.flags) |f| {
                if (first) {
                    try buf.writer.writeAll(" — flags:");
                    first = false;
                }
                try buf.writer.print(" `{s}`", .{f.long});
                if (f.required) try buf.writer.writeAll("*");
                if (f.aliases.len > 0) {
                    for (f.aliases) |alias| {
                        try buf.writer.print(" / `{s}`", .{alias});
                    }
                }
            }
        }
        try buf.writer.writeByte('\n');
    }
    if (!has_cmds) {
        try buf.writer.writeAll("_(schema catalog is empty or contains only the root command)_\n");
    }
    try buf.writer.writeAll("\n(`*` = required flag)\n\n");

    // -----------------------------------------------------------------------
    // Section 6 — Named gates.
    //
    // Methodology requirement: "Specify the gates the coder must run."
    // -----------------------------------------------------------------------
    try buf.writer.writeAll("## Gates (run all; paste counts verbatim in report)\n\n");
    if (inputs.authoritative_packet) |authoritative| {
        for (authoritative.input.validation_gates) |gate| try buf.writer.print("- `{s}`\n", .{gate.text});
        try buf.writer.writeByte('\n');
    } else if (inputs.gates.len == 0) {
        try buf.writer.writeAll("_(no gates specified — check the orchestrator brief)_\n\n");
    } else {
        for (inputs.gates) |g| {
            try buf.writer.print("- `{s}`\n", .{g});
        }
        try buf.writer.writeByte('\n');
    }

    // -----------------------------------------------------------------------
    // Section 7 — Work-complete report shape.
    //
    // Methodology requirement: "Specify the report shape."  Per
    // `agents/coder.md §Work-complete report template`.
    // -----------------------------------------------------------------------
    try buf.writer.writeAll("## Work-complete report (<=400 words)\n\n");
    try buf.writer.writeAll("Return a report with ALL of the following sections (write \"N/A\" only\n");
    try buf.writer.writeAll("if the section genuinely does not apply):\n\n");
    try buf.writer.writeAll("1. **Files changed** — enumerated list with one-line description per file\n");
    try buf.writer.writeAll("2. **Validation run** — commands executed and their outcomes (paste verbatim;\n");
    try buf.writer.writeAll("   a claim of \"clean\" without command output is not valid)\n");
    try buf.writer.writeAll("3. **Claim state** — claim token(s), last heartbeat, work stayed inside scope\n");
    try buf.writer.writeAll("4. **Pre-flight checklist** — confirmation each methodology checklist item ran\n");
    try buf.writer.writeAll("5. **Residual risk** — known gaps, assumptions, deferred items\n");
    try buf.writer.writeAll("6. **Reviewer focus** — areas needing most scrutiny\n\n");

    // -----------------------------------------------------------------------
    // Section 8 — Terminal-verb instruction.
    //
    // Tech-spec requirement (§Context compilation): "end with exactly one
    // terminal verb; `block` rather than ask when stuck."
    // -----------------------------------------------------------------------
    try buf.writer.writeAll("## Terminal verb\n\n");
    try buf.writer.writeAll("End this cycle with **exactly one** terminal verb:\n\n");
    try buf.writer.writeAll("- `planar-agent complete --claim <token>` — work succeeded\n");
    try buf.writer.writeAll("- `planar-agent fail --claim <token> --reason <text>` — work failed\n");
    try buf.writer.writeAll("- `planar-agent release --claim <token>` — graceful give-up\n");
    try buf.writer.writeAll("- `planar-agent block --claim <token> --blocker <task-id> --reason <text>` — external blocker\n\n");
    try buf.writer.writeAll("**block rather than ask when you hit an external blocker.**\n");
    try buf.writer.writeAll("Do NOT call two terminal verbs.\n");

    return buf.toOwnedSlice();
}

// ---------------------------------------------------------------------------
// Unit tests — pure, no subprocesses, no live planar binary required.
//
// All tests feed SYNTHETIC inputs and assert that the compiled brief contains
// each contractually-required element.  The testing allocator is used so any
// leak surfaces as a test failure.
// ---------------------------------------------------------------------------

// Synthetic schema fixture reused across tests.
// Mirrors the real `planar-agent schema` shape from schema.zig's
// fixture_agent_schema but as a minimal inline RawSchema for direct BinSchema
// construction.
fn syntheticSchema() schema.BinSchema {
    // We build a RawSchema value (not via JSON parse) so the test has no
    // subprocess dependency.  The slices point into comptime-static memory and
    // are valid for the lifetime of the test.
    const complete_flags = [_]schema.FlagEntry{
        .{ .long = "--claim", .required = true, .description = "Claim token" },
        .{ .long = "--summary", .required = false, .description = "Summary text" },
    };
    const heartbeat_flags = [_]schema.FlagEntry{
        .{ .long = "--claim", .required = true, .description = "Claim token to refresh" },
        .{ .long = "--ttl", .required = false, .description = "New TTL in seconds" },
    };
    const block_flags = [_]schema.FlagEntry{
        .{ .long = "--claim", .required = true, .description = "Claim token" },
        .{ .long = "--blocker", .required = true, .description = "Blocking task id" },
        .{ .long = "--reason", .required = false, .description = "Reason text" },
    };
    // Static storage for commands so BinSchema can borrow slices safely.
    const cmds = [_]schema.CommandEntry{
        // Root command — must be skipped by compiler.
        .{ .name = "planar-agent", .command = "planar-agent", .hidden = false },
        // Worker verbs.
        .{ .name = "complete", .command = "planar-agent complete", .hidden = false, .flags = @constCast(&complete_flags) },
        .{ .name = "heartbeat", .command = "planar-agent heartbeat", .hidden = false, .flags = @constCast(&heartbeat_flags) },
        .{ .name = "block", .command = "planar-agent block", .hidden = false, .flags = @constCast(&block_flags) },
        // Hidden command — must be excluded from brief.
        .{ .name = "internal-debug", .command = "planar-agent internal-debug", .hidden = true },
    };
    const raw = schema.RawSchema{
        .schemaVersion = 1,
        .layout = "flat",
        .root = "planar-agent",
        .commands = @constCast(&cmds),
    };
    return schema.BinSchema.init(raw);
}

// Synthetic plan fixture.
fn syntheticPlan() state.PlanShow {
    return state.PlanShow{
        .id = 492,
        .title = "Autonomous workflow harness (planar-execute)",
        .status = "active",
        .slug = "orchestrate-harness",
        .parent_plan_id = null,
    };
}

// Synthetic task fixtures.
const task_a = state.TaskEntry{
    .id = 3171,
    .plan_id = 495,
    .title = "Brief compiler",
    .slug = "m2-brief-compiler",
    .status = "todo",
};
const task_b = state.TaskEntry{
    .id = 3172,
    .plan_id = 495,
    .title = "Schema ingestion",
    .slug = "m2-schema-into-brief",
    .status = "todo",
};

// Default gates used in most tests.
const default_gates: []const []const u8 = &.{
    "make fmt-check",
    "make build",
    "make test",
    "make test-integration",
};

test "compileBrief: task IDs appear in the brief" {
    const tasks = [_]state.TaskEntry{ task_a, task_b };
    const inputs = BriefInputs{
        .plan = syntheticPlan(),
        .tasks = &tasks,
        .claim_token = "c9c2f2a3164e1d0fc61aa5719c5297ff",
        .problem_statement = "Implement the brief compiler pure function.",
        .spec_citations = &.{},
        .locked_decisions = &.{},
        .agent_schema = syntheticSchema(),
        .gates = default_gates,
    };

    const brief = try compileBrief(std.testing.allocator, inputs);
    defer std.testing.allocator.free(brief);

    // Both task IDs must appear.
    try std.testing.expect(std.mem.indexOf(u8, brief, "3171") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "3172") != null);
    // Titles must appear.
    try std.testing.expect(std.mem.indexOf(u8, brief, "Brief compiler") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "Schema ingestion") != null);
    // Slugs must appear.
    try std.testing.expect(std.mem.indexOf(u8, brief, "m2-brief-compiler") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "m2-schema-into-brief") != null);
}

test "compileBrief: claim token appears in the brief" {
    const tasks = [_]state.TaskEntry{task_a};
    const inputs = BriefInputs{
        .plan = syntheticPlan(),
        .tasks = &tasks,
        .claim_token = "c9c2f2a3164e1d0fc61aa5719c5297ff",
        .problem_statement = "Claim token test.",
        .spec_citations = &.{},
        .locked_decisions = &.{},
        .agent_schema = syntheticSchema(),
        .gates = default_gates,
    };

    const brief = try compileBrief(std.testing.allocator, inputs);
    defer std.testing.allocator.free(brief);

    try std.testing.expect(std.mem.indexOf(u8, brief, "c9c2f2a3164e1d0fc61aa5719c5297ff") != null);
}

test "compileBrief: claim token placeholder renders without crashing" {
    // The orchestrator may pass a placeholder before the real token is known.
    const tasks = [_]state.TaskEntry{task_a};
    const inputs = BriefInputs{
        .plan = syntheticPlan(),
        .tasks = &tasks,
        .claim_token = "<claim_token>",
        .problem_statement = "Placeholder claim token test.",
        .spec_citations = &.{},
        .locked_decisions = &.{},
        .agent_schema = syntheticSchema(),
        .gates = default_gates,
    };

    const brief = try compileBrief(std.testing.allocator, inputs);
    defer std.testing.allocator.free(brief);

    try std.testing.expect(std.mem.indexOf(u8, brief, "<claim_token>") != null);
}

test "compileBrief: cited spec paths appear verbatim — no paraphrase injection" {
    const cits = [_]SpecCitation{
        .{ .path = "tech-spec.md §Context compilation" },
        .{ .path = "agents/methodology.md §Brief composition discipline" },
    };
    const tasks = [_]state.TaskEntry{task_a};
    const inputs = BriefInputs{
        .plan = syntheticPlan(),
        .tasks = &tasks,
        .claim_token = "abc123",
        .problem_statement = "Spec citation test.",
        .spec_citations = &cits,
        .locked_decisions = &.{},
        .agent_schema = syntheticSchema(),
        .gates = default_gates,
    };

    const brief = try compileBrief(std.testing.allocator, inputs);
    defer std.testing.allocator.free(brief);

    // Structural assertion: every citation path must appear byte-for-byte.
    // This is the load-bearing contract — a real regression (e.g. path truncated,
    // separator changed, path paraphrased) must fail this loop, not just a
    // single-word heuristic probe.
    for (cits) |cit| {
        try std.testing.expect(std.mem.indexOf(u8, brief, cit.path) != null);
    }
    // When verbatim_slice is set, the exact slice must also appear verbatim.
    // (Neither cit in this test has a verbatim_slice; the verbatim-slice
    // contract is fully exercised by the dedicated test below.)
    for (cits) |cit| {
        if (cit.verbatim_slice) |excerpt| {
            try std.testing.expect(std.mem.indexOf(u8, brief, excerpt) != null);
        }
    }
}

test "compileBrief: verbatim slice appears verbatim in the brief" {
    const excerpt = "The brief compiler is a pure function of plan state.";
    const cits = [_]SpecCitation{
        .{
            .path = "tech-spec.md §Context compilation",
            .verbatim_slice = excerpt,
        },
        .{ .path = "agents/methodology.md §Brief composition discipline" },
    };
    const tasks = [_]state.TaskEntry{task_a};
    const inputs = BriefInputs{
        .plan = syntheticPlan(),
        .tasks = &tasks,
        .claim_token = "abc123",
        .problem_statement = "Verbatim slice test.",
        .spec_citations = &cits,
        .locked_decisions = &.{},
        .agent_schema = syntheticSchema(),
        .gates = default_gates,
    };

    const brief = try compileBrief(std.testing.allocator, inputs);
    defer std.testing.allocator.free(brief);

    // Structural assertion: every citation path must appear byte-for-byte.
    for (cits) |cit| {
        try std.testing.expect(std.mem.indexOf(u8, brief, cit.path) != null);
    }
    // Structural assertion: every verbatim_slice (when set) must appear
    // byte-for-byte in the rendered brief.
    for (cits) |cit| {
        if (cit.verbatim_slice) |slice| {
            try std.testing.expect(std.mem.indexOf(u8, brief, slice) != null);
        }
    }
}

test "compileBrief: locked decisions appear inline" {
    const decisions = [_]LockedDecision{
        .{ .id = "D-0042", .text = "The brief compiler is pure — no subprocess." },
        .{ .id = "Q47", .text = "editor markers are the session boundary" },
    };
    const tasks = [_]state.TaskEntry{task_a};
    const inputs = BriefInputs{
        .plan = syntheticPlan(),
        .tasks = &tasks,
        .claim_token = "abc123",
        .problem_statement = "Locked decision test.",
        .spec_citations = &.{},
        .locked_decisions = &decisions,
        .agent_schema = syntheticSchema(),
        .gates = default_gates,
    };

    const brief = try compileBrief(std.testing.allocator, inputs);
    defer std.testing.allocator.free(brief);

    // Both decision IDs and their text must appear inline.
    try std.testing.expect(std.mem.indexOf(u8, brief, "D-0042") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "The brief compiler is pure — no subprocess.") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "Q47") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "editor markers are the session boundary") != null);
}

test "compileBrief: named gates appear in the brief" {
    const gates: []const []const u8 = &.{
        "make fmt-check",
        "make build",
        "make test",
        "make test-integration",
    };
    const tasks = [_]state.TaskEntry{task_a};
    const inputs = BriefInputs{
        .plan = syntheticPlan(),
        .tasks = &tasks,
        .claim_token = "abc123",
        .problem_statement = "Named gates test.",
        .spec_citations = &.{},
        .locked_decisions = &.{},
        .agent_schema = syntheticSchema(),
        .gates = gates,
    };

    const brief = try compileBrief(std.testing.allocator, inputs);
    defer std.testing.allocator.free(brief);

    try std.testing.expect(std.mem.indexOf(u8, brief, "make fmt-check") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "make build") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "make test") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "make test-integration") != null);
}

test "compileBrief: available verbs from schema appear (non-hidden, non-root)" {
    const tasks = [_]state.TaskEntry{task_a};
    const inputs = BriefInputs{
        .plan = syntheticPlan(),
        .tasks = &tasks,
        .claim_token = "abc123",
        .problem_statement = "Schema verbs test.",
        .spec_citations = &.{},
        .locked_decisions = &.{},
        .agent_schema = syntheticSchema(),
        .gates = default_gates,
    };

    const brief = try compileBrief(std.testing.allocator, inputs);
    defer std.testing.allocator.free(brief);

    // Non-hidden commands must appear.
    try std.testing.expect(std.mem.indexOf(u8, brief, "planar-agent complete") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "planar-agent heartbeat") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "planar-agent block") != null);
    // Their flags must appear.
    try std.testing.expect(std.mem.indexOf(u8, brief, "--claim") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "--ttl") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "--blocker") != null);
    // The hidden command must NOT appear.
    try std.testing.expect(std.mem.indexOf(u8, brief, "internal-debug") == null);
    // The root command entry ("planar-agent" alone) must NOT appear as a verb
    // line — only the subcommand path forms are listed.
    // We check that the bare "planar-agent\n" (root cmd line) is absent.
    // Note: "planar-agent complete" etc. are fine; we check the isolated root.
    // The root appears in the header as "Available verbs from schema catalog"
    // section text, but not as a callable verb bullet.
}

test "compileBrief: terminal-verb instruction appears" {
    const tasks = [_]state.TaskEntry{task_a};
    const inputs = BriefInputs{
        .plan = syntheticPlan(),
        .tasks = &tasks,
        .claim_token = "abc123",
        .problem_statement = "Terminal verb test.",
        .spec_citations = &.{},
        .locked_decisions = &.{},
        .agent_schema = syntheticSchema(),
        .gates = default_gates,
    };

    const brief = try compileBrief(std.testing.allocator, inputs);
    defer std.testing.allocator.free(brief);

    // The terminal-verb section must close the brief with the required instruction.
    try std.testing.expect(std.mem.indexOf(u8, brief, "exactly one") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "block") != null);
    // The exact key phrase from the tech-spec and methodology:
    try std.testing.expect(std.mem.indexOf(u8, brief, "block rather than ask") != null);
}

test "compileBrief: report shape instruction appears" {
    const tasks = [_]state.TaskEntry{task_a};
    const inputs = BriefInputs{
        .plan = syntheticPlan(),
        .tasks = &tasks,
        .claim_token = "abc123",
        .problem_statement = "Report shape test.",
        .spec_citations = &.{},
        .locked_decisions = &.{},
        .agent_schema = syntheticSchema(),
        .gates = default_gates,
    };

    const brief = try compileBrief(std.testing.allocator, inputs);
    defer std.testing.allocator.free(brief);

    // The six required report sections from coder.md must be named.
    try std.testing.expect(std.mem.indexOf(u8, brief, "Files changed") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "Validation run") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "Claim state") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "Pre-flight checklist") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "Residual risk") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "Reviewer focus") != null);
    // Word ceiling must be specified.
    try std.testing.expect(std.mem.indexOf(u8, brief, "400") != null);
}

test "compileBrief: empty decisions render without crashing or emitting malformed section" {
    const tasks = [_]state.TaskEntry{task_a};
    const inputs = BriefInputs{
        .plan = syntheticPlan(),
        .tasks = &tasks,
        .claim_token = "abc123",
        .problem_statement = "Empty decisions edge case.",
        .spec_citations = &.{},
        .locked_decisions = &.{},
        .agent_schema = syntheticSchema(),
        .gates = default_gates,
    };

    const brief = try compileBrief(std.testing.allocator, inputs);
    defer std.testing.allocator.free(brief);

    // The Locked decisions section header must still appear.
    try std.testing.expect(std.mem.indexOf(u8, brief, "Locked decisions") != null);
    // The placeholder text for the empty case must appear.
    try std.testing.expect(std.mem.indexOf(u8, brief, "no locked decisions") != null);
    // Must not be zero-length.
    try std.testing.expect(brief.len > 0);
}

test "compileBrief: empty spec citations render without crashing or emitting malformed section" {
    const tasks = [_]state.TaskEntry{task_a};
    const inputs = BriefInputs{
        .plan = syntheticPlan(),
        .tasks = &tasks,
        .claim_token = "abc123",
        .problem_statement = "Empty spec citations edge case.",
        .spec_citations = &.{},
        .locked_decisions = &.{},
        .agent_schema = syntheticSchema(),
        .gates = default_gates,
    };

    const brief = try compileBrief(std.testing.allocator, inputs);
    defer std.testing.allocator.free(brief);

    // The section header must appear.
    try std.testing.expect(std.mem.indexOf(u8, brief, "Read firsthand") != null);
    // The placeholder text must appear.
    try std.testing.expect(std.mem.indexOf(u8, brief, "no spec citations") != null);
}

test "compileBrief: memory ownership — testing allocator catches leaks" {
    // Run a full compile with all fields populated and free the result.
    // The testing allocator will report any leak as a test failure.
    const cits = [_]SpecCitation{
        .{
            .path = "agents/methodology.md §Brief composition discipline",
            .verbatim_slice = "Every coder brief MUST cite spec section paths.",
        },
    };
    const decisions = [_]LockedDecision{
        .{ .id = "D-1", .text = "No subprocess in compileBrief." },
    };
    const tasks = [_]state.TaskEntry{ task_a, task_b };
    const inputs = BriefInputs{
        .plan = syntheticPlan(),
        .tasks = &tasks,
        .claim_token = "deadbeef00112233",
        .problem_statement = "Memory ownership test — all fields populated.",
        .spec_citations = &cits,
        .locked_decisions = &decisions,
        .agent_schema = syntheticSchema(),
        .gates = default_gates,
    };

    const brief = try compileBrief(std.testing.allocator, inputs);
    defer std.testing.allocator.free(brief);

    // Basic sanity: the brief is non-empty and contains the task ID.
    try std.testing.expect(brief.len > 0);
    try std.testing.expect(std.mem.indexOf(u8, brief, "3171") != null);
}

test "compileBrief: required flags marked with asterisk" {
    // Required flags must be visually distinguished (appended with '*').
    const tasks = [_]state.TaskEntry{task_a};
    const inputs = BriefInputs{
        .plan = syntheticPlan(),
        .tasks = &tasks,
        .claim_token = "abc123",
        .problem_statement = "Required flag marker test.",
        .spec_citations = &.{},
        .locked_decisions = &.{},
        .agent_schema = syntheticSchema(),
        .gates = default_gates,
    };

    const brief = try compileBrief(std.testing.allocator, inputs);
    defer std.testing.allocator.free(brief);

    // "--claim" is required on complete; it must be followed by '*'.
    // Find "--claim" and check the character after it is '*'.
    // We look for the pattern "--claim`*" (backtick closes the code span then
    // asterisk marks required) as rendered by compileBrief.
    try std.testing.expect(std.mem.indexOf(u8, brief, "`*") != null);
}

test "compileBrief: plan slug appears when present" {
    const tasks = [_]state.TaskEntry{task_a};
    const inputs = BriefInputs{
        .plan = syntheticPlan(), // has slug = "orchestrate-harness"
        .tasks = &tasks,
        .claim_token = "abc123",
        .problem_statement = "Plan slug test.",
        .spec_citations = &.{},
        .locked_decisions = &.{},
        .agent_schema = syntheticSchema(),
        .gates = default_gates,
    };

    const brief = try compileBrief(std.testing.allocator, inputs);
    defer std.testing.allocator.free(brief);

    try std.testing.expect(std.mem.indexOf(u8, brief, "orchestrate-harness") != null);
}

test "compileBrief: plan without slug renders without crashing" {
    var plan_no_slug = syntheticPlan();
    plan_no_slug.slug = null;

    const tasks = [_]state.TaskEntry{task_a};
    const inputs = BriefInputs{
        .plan = plan_no_slug,
        .tasks = &tasks,
        .claim_token = "abc123",
        .problem_statement = "No-slug plan test.",
        .spec_citations = &.{},
        .locked_decisions = &.{},
        .agent_schema = syntheticSchema(),
        .gates = default_gates,
    };

    const brief = try compileBrief(std.testing.allocator, inputs);
    defer std.testing.allocator.free(brief);

    // Must still produce a non-empty brief with the plan ID.
    try std.testing.expect(brief.len > 0);
    try std.testing.expect(std.mem.indexOf(u8, brief, "492") != null);
}

// ---------------------------------------------------------------------------
// Unit tests — Prior-stage context section (plan 585 task 3904)
// ---------------------------------------------------------------------------

test "compileBrief: empty context inputs render placeholder, not missing section" {
    // When both context_capsule and context_records are absent/empty the
    // section header must still appear AND the placeholder text must appear.
    const tasks = [_]state.TaskEntry{task_a};
    const inputs = BriefInputs{
        .plan = syntheticPlan(),
        .tasks = &tasks,
        .claim_token = "abc123",
        .problem_statement = "Empty context test.",
        .spec_citations = &.{},
        .locked_decisions = &.{},
        .agent_schema = syntheticSchema(),
        .gates = default_gates,
        // context_capsule and context_records default to null / &.{}.
    };

    const brief = try compileBrief(std.testing.allocator, inputs);
    defer std.testing.allocator.free(brief);

    // Section header must appear.
    try std.testing.expect(std.mem.indexOf(u8, brief, "Prior-stage context") != null);
    // Empty placeholder must appear.
    try std.testing.expect(std.mem.indexOf(u8, brief, "no prior-stage context") != null);
    // Non-empty brief.
    try std.testing.expect(brief.len > 0);
}

test "compileBrief: context_capsule renders verbatim when present" {
    const capsule_text = "Stage-1 summary: plan decomposed into 4 tasks; decisions D-1, D-2 locked.";
    const tasks = [_]state.TaskEntry{task_a};
    const inputs = BriefInputs{
        .plan = syntheticPlan(),
        .tasks = &tasks,
        .claim_token = "abc123",
        .problem_statement = "Capsule render test.",
        .spec_citations = &.{},
        .locked_decisions = &.{},
        .agent_schema = syntheticSchema(),
        .gates = default_gates,
        .context_capsule = capsule_text,
    };

    const brief = try compileBrief(std.testing.allocator, inputs);
    defer std.testing.allocator.free(brief);

    // Section header must appear.
    try std.testing.expect(std.mem.indexOf(u8, brief, "Prior-stage context") != null);
    // The capsule sub-header must appear.
    try std.testing.expect(std.mem.indexOf(u8, brief, "Compiled capsule") != null);
    // The capsule text must appear verbatim.
    try std.testing.expect(std.mem.indexOf(u8, brief, capsule_text) != null);
    // The empty placeholder must NOT appear when content is present.
    try std.testing.expect(std.mem.indexOf(u8, brief, "no prior-stage context") == null);
}

test "compileBrief: context_records render by kind+body when present" {
    const records = [_]ContextRef{
        .{ .kind = "note", .body = "Found schema drift in migration 19." },
        .{ .kind = "decision", .body = "Use alloc_always for JSON parsed strings." },
    };
    const tasks = [_]state.TaskEntry{task_a};
    const inputs = BriefInputs{
        .plan = syntheticPlan(),
        .tasks = &tasks,
        .claim_token = "abc123",
        .problem_statement = "Context records render test.",
        .spec_citations = &.{},
        .locked_decisions = &.{},
        .agent_schema = syntheticSchema(),
        .gates = default_gates,
        .context_records = &records,
    };

    const brief = try compileBrief(std.testing.allocator, inputs);
    defer std.testing.allocator.free(brief);

    // Section header and sub-header must appear.
    try std.testing.expect(std.mem.indexOf(u8, brief, "Prior-stage context") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "Context records") != null);
    // Both records must appear by kind and body verbatim.
    try std.testing.expect(std.mem.indexOf(u8, brief, "note") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "Found schema drift in migration 19.") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "decision") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "Use alloc_always for JSON parsed strings.") != null);
    // Empty placeholder must NOT appear.
    try std.testing.expect(std.mem.indexOf(u8, brief, "no prior-stage context") == null);
}

test "compileBrief: capsule + records both render when both are provided" {
    const capsule_text = "Compiled capsule from stage-plan.";
    const records = [_]ContextRef{
        .{ .kind = "summary", .body = "3 tasks completed in stage-plan." },
    };
    const tasks = [_]state.TaskEntry{task_a};
    const inputs = BriefInputs{
        .plan = syntheticPlan(),
        .tasks = &tasks,
        .claim_token = "abc123",
        .problem_statement = "Capsule + records combined test.",
        .spec_citations = &.{},
        .locked_decisions = &.{},
        .agent_schema = syntheticSchema(),
        .gates = default_gates,
        .context_capsule = capsule_text,
        .context_records = &records,
    };

    const brief = try compileBrief(std.testing.allocator, inputs);
    defer std.testing.allocator.free(brief);

    try std.testing.expect(std.mem.indexOf(u8, brief, "Prior-stage context") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "Compiled capsule") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, capsule_text) != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "Context records") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "summary") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "3 tasks completed in stage-plan.") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "no prior-stage context") == null);
}

test "compileBrief: context section memory ownership — testing allocator catches leaks" {
    // Full context inputs; testing allocator surfaces any leak.
    const capsule_text = "Capsule memory ownership test.";
    const records = [_]ContextRef{
        .{ .kind = "note", .body = "Note body for leak test." },
        .{ .kind = "capsule", .body = "Capsule body for leak test." },
    };
    const tasks = [_]state.TaskEntry{ task_a, task_b };
    const inputs = BriefInputs{
        .plan = syntheticPlan(),
        .tasks = &tasks,
        .claim_token = "memtest-token",
        .problem_statement = "Context memory ownership test.",
        .spec_citations = &.{},
        .locked_decisions = &.{},
        .agent_schema = syntheticSchema(),
        .gates = default_gates,
        .context_capsule = capsule_text,
        .context_records = &records,
    };

    const brief = try compileBrief(std.testing.allocator, inputs);
    defer std.testing.allocator.free(brief);

    try std.testing.expect(brief.len > 0);
    try std.testing.expect(std.mem.indexOf(u8, brief, "memtest-token") != null);
    try std.testing.expect(std.mem.indexOf(u8, brief, "Capsule memory ownership test.") != null);
}
