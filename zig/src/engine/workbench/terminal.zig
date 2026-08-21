//! engine/workbench/terminal — comptime status table + filter-mode helper.
//!
//! The workbench projects six entity kinds onto the filesystem (plans, tasks,
//! decisions, questions, test_scenarios, artifacts). Each kind has a status
//! lifecycle anchored by migration CHECK constraints (see migrations/00002,
//! 00003, 00008, 00013). This module captures those lifecycles as a comptime
//! source of truth, partitions them along a success-vs-failure terminal axis,
//! and exposes one helper — `isFiltered(kind, status, mode)` — that the
//! workbench push / archive / restore / gc paths consume.
//!
//! Filter modes:
//! - `.failures` (default): only failure terminals are filtered. Success
//!   terminals (`tasks.done`, `questions.answered`, `test_scenarios.verified`)
//!   stay visible on disk as checkpoint artifacts. This is the operator-facing
//!   default per plan 439 / [[q334-terminal-set-definition]].
//! - `.all`: every terminal status is filtered. Used by operators who want
//!   their workbench tree to mirror active work only.
//!
//! Pull is intentionally NOT a caller of this module — pull always ingests
//! terminal-backed FS files (retroactive-documentation case, per
//! [[q336-pull-filter]]).
//!
//! New statuses added by future migrations must be classified here; the unit
//! tests at the bottom of this file exhaustively switch on each kind's status
//! enum, so adding an unclassified status is a compile-time error.

const std = @import("std");

/// The six entity kinds the workbench projects onto the filesystem.
pub const Kind = enum {
    plan,
    task,
    decision,
    question,
    test_scenario,
    artifact,

    pub fn fromString(s: []const u8) ?Kind {
        if (std.mem.eql(u8, s, "plan")) return .plan;
        if (std.mem.eql(u8, s, "task")) return .task;
        if (std.mem.eql(u8, s, "decision")) return .decision;
        if (std.mem.eql(u8, s, "question")) return .question;
        if (std.mem.eql(u8, s, "test_scenario")) return .test_scenario;
        // The workbench sync layer aliases `test_scenario` as `scenario` in
        // its entity stream; accept both forms.
        if (std.mem.eql(u8, s, "scenario")) return .test_scenario;
        if (std.mem.eql(u8, s, "artifact")) return .artifact;
        return null;
    }
};

/// Filter mode chosen by the operator (default `.failures` — see module doc).
pub const Mode = enum {
    failures,
    all,

    pub fn fromString(s: []const u8) ?Mode {
        if (std.mem.eql(u8, s, "failures")) return .failures;
        if (std.mem.eql(u8, s, "all")) return .all;
        return null;
    }

    pub fn toString(m: Mode) []const u8 {
        return switch (m) {
            .failures => "failures",
            .all => "all",
        };
    }
};

/// Classification of a (kind, status) pair against the terminal axis.
pub const Classification = enum {
    /// Non-terminal — the entity is active work; never filtered.
    active,
    /// Success terminal — the entity completed successfully. Filtered only
    /// under `Mode.all`.
    success_terminal,
    /// Failure terminal — the entity was abandoned, cancelled, superseded,
    /// withdrawn, retired, or marked wontfix. Filtered under both modes.
    failure_terminal,
};

/// Status enums per kind. These mirror the migration CHECK constraints
/// verbatim — a divergence is a compile-time error in the classification
/// switch below.
pub const PlanStatus = enum {
    draft,
    active,
    paused,
    done,
    abandoned,

    pub fn fromString(s: []const u8) ?PlanStatus {
        if (std.mem.eql(u8, s, "draft")) return .draft;
        if (std.mem.eql(u8, s, "active")) return .active;
        if (std.mem.eql(u8, s, "paused")) return .paused;
        if (std.mem.eql(u8, s, "done")) return .done;
        if (std.mem.eql(u8, s, "abandoned")) return .abandoned;
        return null;
    }
};

pub const TaskStatus = enum {
    todo,
    doing,
    blocked,
    done,
    cancelled,

    pub fn fromString(s: []const u8) ?TaskStatus {
        if (std.mem.eql(u8, s, "todo")) return .todo;
        if (std.mem.eql(u8, s, "doing")) return .doing;
        if (std.mem.eql(u8, s, "blocked")) return .blocked;
        if (std.mem.eql(u8, s, "done")) return .done;
        if (std.mem.eql(u8, s, "cancelled")) return .cancelled;
        return null;
    }
};

pub const DecisionStatus = enum {
    proposed,
    accepted,
    superseded,
    withdrawn,

    pub fn fromString(s: []const u8) ?DecisionStatus {
        if (std.mem.eql(u8, s, "proposed")) return .proposed;
        if (std.mem.eql(u8, s, "accepted")) return .accepted;
        if (std.mem.eql(u8, s, "superseded")) return .superseded;
        if (std.mem.eql(u8, s, "withdrawn")) return .withdrawn;
        return null;
    }
};

pub const QuestionStatus = enum {
    open,
    answered,
    wontfix,

    pub fn fromString(s: []const u8) ?QuestionStatus {
        if (std.mem.eql(u8, s, "open")) return .open;
        if (std.mem.eql(u8, s, "answered")) return .answered;
        if (std.mem.eql(u8, s, "wontfix")) return .wontfix;
        return null;
    }
};

pub const TestScenarioStatus = enum {
    draft,
    ready,
    verified,
    failing,
    retired,

    pub fn fromString(s: []const u8) ?TestScenarioStatus {
        if (std.mem.eql(u8, s, "draft")) return .draft;
        if (std.mem.eql(u8, s, "ready")) return .ready;
        if (std.mem.eql(u8, s, "verified")) return .verified;
        if (std.mem.eql(u8, s, "failing")) return .failing;
        if (std.mem.eql(u8, s, "retired")) return .retired;
        return null;
    }
};

pub const ArtifactStatus = enum {
    draft,
    active,
    superseded,
    retired,

    pub fn fromString(s: []const u8) ?ArtifactStatus {
        if (std.mem.eql(u8, s, "draft")) return .draft;
        if (std.mem.eql(u8, s, "active")) return .active;
        if (std.mem.eql(u8, s, "superseded")) return .superseded;
        if (std.mem.eql(u8, s, "retired")) return .retired;
        return null;
    }
};

/// Classification table — comptime exhaustive switches per kind.
///
/// Adding a new status to any kind's enum (because a future migration
/// extended the CHECK constraint) requires updating the matching switch
/// here; the compiler will refuse to build until every variant is handled.
pub fn classifyPlan(s: PlanStatus) Classification {
    return switch (s) {
        .draft, .active, .paused => .active,
        .done => .success_terminal,
        .abandoned => .failure_terminal,
    };
}

pub fn classifyTask(s: TaskStatus) Classification {
    return switch (s) {
        .todo, .doing, .blocked => .active,
        .done => .success_terminal,
        .cancelled => .failure_terminal,
    };
}

pub fn classifyDecision(s: DecisionStatus) Classification {
    return switch (s) {
        .proposed, .accepted => .active,
        .superseded, .withdrawn => .failure_terminal,
    };
}

pub fn classifyQuestion(s: QuestionStatus) Classification {
    return switch (s) {
        .open => .active,
        .answered => .success_terminal,
        .wontfix => .failure_terminal,
    };
}

pub fn classifyTestScenario(s: TestScenarioStatus) Classification {
    return switch (s) {
        .draft, .ready, .failing => .active,
        .verified => .success_terminal,
        .retired => .failure_terminal,
    };
}

pub fn classifyArtifact(s: ArtifactStatus) Classification {
    return switch (s) {
        .draft, .active => .active,
        .superseded, .retired => .failure_terminal,
    };
}

/// Filter predicate — the one helper sync.zig / archive / restore / gc consume.
///
/// Returns true when the entity should be EXCLUDED from the workbench write
/// set under the given mode. Active entities are never filtered. Failure
/// terminals are filtered under both modes. Success terminals are filtered
/// only under `.all`.
pub fn isFiltered(class: Classification, mode: Mode) bool {
    return switch (class) {
        .active => false,
        .failure_terminal => true,
        .success_terminal => mode == .all,
    };
}

/// String-based adapter for sync.zig, which carries entity_kind / status as
/// `[]const u8` pulled from SQLite. Returns null when either string is not a
/// recognized value (caller decides whether to refuse-or-pass-through).
pub fn isFilteredStr(kind_str: []const u8, status_str: []const u8, mode: Mode) ?bool {
    const kind = Kind.fromString(kind_str) orelse return null;
    const class: Classification = switch (kind) {
        .plan => classifyPlan(PlanStatus.fromString(status_str) orelse return null),
        .task => classifyTask(TaskStatus.fromString(status_str) orelse return null),
        .decision => classifyDecision(DecisionStatus.fromString(status_str) orelse return null),
        .question => classifyQuestion(QuestionStatus.fromString(status_str) orelse return null),
        .test_scenario => classifyTestScenario(TestScenarioStatus.fromString(status_str) orelse return null),
        .artifact => classifyArtifact(ArtifactStatus.fromString(status_str) orelse return null),
    };
    return isFiltered(class, mode);
}

// ============================================================================
// Tests
// ============================================================================

const testing = std.testing;

test "Mode.fromString round-trip" {
    try testing.expectEqual(@as(?Mode, .failures), Mode.fromString("failures"));
    try testing.expectEqual(@as(?Mode, .all), Mode.fromString("all"));
    try testing.expectEqual(@as(?Mode, null), Mode.fromString("other"));
    try testing.expectEqualStrings("failures", Mode.toString(.failures));
    try testing.expectEqualStrings("all", Mode.toString(.all));
}

test "Kind.fromString covers every kind" {
    try testing.expectEqual(@as(?Kind, .plan), Kind.fromString("plan"));
    try testing.expectEqual(@as(?Kind, .task), Kind.fromString("task"));
    try testing.expectEqual(@as(?Kind, .decision), Kind.fromString("decision"));
    try testing.expectEqual(@as(?Kind, .question), Kind.fromString("question"));
    try testing.expectEqual(@as(?Kind, .test_scenario), Kind.fromString("test_scenario"));
    try testing.expectEqual(@as(?Kind, .artifact), Kind.fromString("artifact"));
    try testing.expectEqual(@as(?Kind, null), Kind.fromString("session"));
}

test "classifyPlan covers every status" {
    try testing.expectEqual(Classification.active, classifyPlan(.draft));
    try testing.expectEqual(Classification.active, classifyPlan(.active));
    try testing.expectEqual(Classification.active, classifyPlan(.paused));
    try testing.expectEqual(Classification.success_terminal, classifyPlan(.done));
    try testing.expectEqual(Classification.failure_terminal, classifyPlan(.abandoned));
}

test "classifyTask covers every status" {
    try testing.expectEqual(Classification.active, classifyTask(.todo));
    try testing.expectEqual(Classification.active, classifyTask(.doing));
    try testing.expectEqual(Classification.active, classifyTask(.blocked));
    try testing.expectEqual(Classification.success_terminal, classifyTask(.done));
    try testing.expectEqual(Classification.failure_terminal, classifyTask(.cancelled));
}

test "classifyDecision covers every status" {
    try testing.expectEqual(Classification.active, classifyDecision(.proposed));
    try testing.expectEqual(Classification.active, classifyDecision(.accepted));
    try testing.expectEqual(Classification.failure_terminal, classifyDecision(.superseded));
    try testing.expectEqual(Classification.failure_terminal, classifyDecision(.withdrawn));
}

test "classifyQuestion covers every status" {
    try testing.expectEqual(Classification.active, classifyQuestion(.open));
    try testing.expectEqual(Classification.success_terminal, classifyQuestion(.answered));
    try testing.expectEqual(Classification.failure_terminal, classifyQuestion(.wontfix));
}

test "classifyTestScenario covers every status" {
    try testing.expectEqual(Classification.active, classifyTestScenario(.draft));
    try testing.expectEqual(Classification.active, classifyTestScenario(.ready));
    try testing.expectEqual(Classification.success_terminal, classifyTestScenario(.verified));
    try testing.expectEqual(Classification.active, classifyTestScenario(.failing));
    try testing.expectEqual(Classification.failure_terminal, classifyTestScenario(.retired));
}

test "classifyArtifact covers every status" {
    try testing.expectEqual(Classification.active, classifyArtifact(.draft));
    try testing.expectEqual(Classification.active, classifyArtifact(.active));
    try testing.expectEqual(Classification.failure_terminal, classifyArtifact(.superseded));
    try testing.expectEqual(Classification.failure_terminal, classifyArtifact(.retired));
}

test "isFiltered respects mode" {
    // Active entities are never filtered.
    try testing.expectEqual(false, isFiltered(.active, .failures));
    try testing.expectEqual(false, isFiltered(.active, .all));

    // Failure terminals are always filtered.
    try testing.expectEqual(true, isFiltered(.failure_terminal, .failures));
    try testing.expectEqual(true, isFiltered(.failure_terminal, .all));

    // Success terminals are filtered only under .all.
    try testing.expectEqual(false, isFiltered(.success_terminal, .failures));
    try testing.expectEqual(true, isFiltered(.success_terminal, .all));
}

test "isFilteredStr — happy path against all six kinds" {
    // Active.
    try testing.expectEqual(@as(?bool, false), isFilteredStr("plan", "active", .failures));
    try testing.expectEqual(@as(?bool, false), isFilteredStr("task", "doing", .failures));
    try testing.expectEqual(@as(?bool, false), isFilteredStr("decision", "proposed", .failures));
    try testing.expectEqual(@as(?bool, false), isFilteredStr("question", "open", .failures));
    try testing.expectEqual(@as(?bool, false), isFilteredStr("test_scenario", "ready", .failures));
    try testing.expectEqual(@as(?bool, false), isFilteredStr("artifact", "draft", .failures));

    // Failure terminals — filtered under failures.
    try testing.expectEqual(@as(?bool, true), isFilteredStr("plan", "abandoned", .failures));
    try testing.expectEqual(@as(?bool, true), isFilteredStr("task", "cancelled", .failures));
    try testing.expectEqual(@as(?bool, true), isFilteredStr("decision", "superseded", .failures));
    try testing.expectEqual(@as(?bool, true), isFilteredStr("decision", "withdrawn", .failures));
    try testing.expectEqual(@as(?bool, true), isFilteredStr("question", "wontfix", .failures));
    try testing.expectEqual(@as(?bool, true), isFilteredStr("test_scenario", "retired", .failures));
    try testing.expectEqual(@as(?bool, true), isFilteredStr("artifact", "superseded", .failures));
    try testing.expectEqual(@as(?bool, true), isFilteredStr("artifact", "retired", .failures));

    // Success terminals — visible under failures, filtered under all.
    try testing.expectEqual(@as(?bool, false), isFilteredStr("plan", "done", .failures));
    try testing.expectEqual(@as(?bool, true), isFilteredStr("plan", "done", .all));
    try testing.expectEqual(@as(?bool, false), isFilteredStr("task", "done", .failures));
    try testing.expectEqual(@as(?bool, true), isFilteredStr("task", "done", .all));
    try testing.expectEqual(@as(?bool, false), isFilteredStr("question", "answered", .failures));
    try testing.expectEqual(@as(?bool, true), isFilteredStr("question", "answered", .all));
    try testing.expectEqual(@as(?bool, false), isFilteredStr("test_scenario", "verified", .failures));
    try testing.expectEqual(@as(?bool, true), isFilteredStr("test_scenario", "verified", .all));
}

test "isFilteredStr — unrecognized kind or status returns null" {
    try testing.expectEqual(@as(?bool, null), isFilteredStr("session", "active", .failures));
    try testing.expectEqual(@as(?bool, null), isFilteredStr("annotation", "active", .failures));
    try testing.expectEqual(@as(?bool, null), isFilteredStr("task", "fabricated", .failures));
    try testing.expectEqual(@as(?bool, null), isFilteredStr("plan", "completed", .failures));
}
