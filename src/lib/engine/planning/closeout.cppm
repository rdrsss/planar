/// @file closeout.cppm
/// @brief `planar.engine.planning.closeout` — the delivery-evidence gate
/// behind `planar plan closeout` (plan 996, task 6317).
///
/// Port target: `zig/src/engine/planning/closeout.zig` (913 lines) plus its
/// 213-line handler `zig/src/cmd/planar/handlers/plan/closeout.zig`.
///
/// This is NOT a read verb. `CLAUDE.md` names it the AUTHORITATIVE GATE the
/// janitor runs to close a plan, and in apply mode it writes `plans.status =
/// 'done'` plus an `audit_log` row. Every behaviour below was captured by
/// running the oracle in a pinned scratch arena (`PLANAR_DB` pointed at a
/// throwaway file), never against an operator database and never inferred
/// from a sibling verb.
///
/// ## THE HARD GATE (three rules; each failure is a `blocked_by` reason)
///
///  1. Every task on the plan AND on every descendant plan is `done` or
///     `cancelled`. `todo`/`doing`/`blocked` are "open" and block.
///  2. Every descendant plan (recursively, via `parent_plan_id`) is `done`
///     or `abandoned`. `draft`/`active`/`paused` are "open" and block.
///  3. No LIVE claim: no `agent_work_claims` row with `status='active'` and
///     `lease_expires_at` in the future, on any task of the plan or its
///     descendants.
///
/// CANCELLED TASKS ARE TERMINAL HISTORY. They are counted and reported and
/// they do NOT block — measured against a plan whose only task was
/// `cancelled`: `ready:true`, `cancelled:1`.
///
/// STALE CLAIMS DO NOT BLOCK. An `active` claim whose lease has expired is
/// counted under `claims.stale` and produces a WARNING, not a reason.
///
/// ## THE THREE COUNTING QUERIES DISAGREE ABOUT `status`, AND THAT IS THE
/// ORACLE'S BEHAVIOUR, NOT AN OVERSIGHT TO TIDY
///
/// `collect_claim_counts` filters `c.status = 'active'`. The two LOCALITY
/// queries that feed the advisory git layer (`collect_git_evidence` and
/// `collect_epic_merge_rollup`) DO NOT — they filter only on
/// `entity_kind='task'`, plan membership, and a non-null `repo_root`. A
/// `released` claim therefore contributes NO claim count and DOES
/// contribute a git-evidence row. Measured: a fixture with three `active`
/// and one `released` locality-bearing claim reported `claims.live:3` and
/// FOUR `git_evidence` entries.
///
/// The two locality queries also differ from each other: `git_evidence` is
/// `distinct (repo_root, branch, head_sha_at_claim)` and the epic roll-up is
/// `distinct (repo_root, branch)` with BOTH required non-null. On a
/// six-claim fixture that produced six git-evidence entries and
/// `total_branches:4`.
///
/// ## THE ADVISORY GIT LAYER IS DEAD CODE IN THE ORACLE — see the DIVERGENCE
/// section below. It is ported to WORK here, deliberately, and that is the
/// one place this module does not reproduce measured oracle output.
///
/// ## `finalization_tasks` IS PURE LABELLING
///
/// Counts tasks (plan + descendants) whose slug is `LIKE 'finalize-%'`,
/// `'merge-%'` or `'reconcile-%'`. It is reported and never consulted: a
/// fixture with three such slugs plus one ordinary task, all `todo`,
/// reported `open:4  finalization_tasks:3` and blocked on all four.
///
/// ## THE ALREADY-TERMINAL SHORT-CIRCUIT RETURNS A DIFFERENT SHAPE
///
/// When the plan is already `done` or `abandoned`, `evaluate` returns
/// BEFORE collecting anything: `ready:true`, `applied:false`, all counts
/// zero, and — the part that is easy to get wrong — `git_evidence: []`,
/// EMPTY. A live evaluation with no locality data returns a ONE-entry
/// synthetic `(none)` row instead. Both were measured on the same plan
/// before and after it was closed. `--check-merge` on an already-terminal
/// plan also yields `epic_merge: null` regardless.
///
/// ## `applied` VS `ready`, AND THE TEXT BANNER'S ORACLE DEFECT
///
/// The text renderer picks its banner from `applied` first, then `ready`:
///
///     applied            -> "plan {id}: marked done"
///     ready, !applied    -> "plan {id}: ready (already terminal — no change)"
///     !ready             -> "plan {id}: NOT ready to close"
///
/// The middle arm is reached by ANY ready-but-not-applied evaluation, which
/// includes EVERY `--dry-run` on a ready plan. Measured on a `draft` plan
/// with no tasks: `planar plan closeout 1 --dry-run` printed "ready (already
/// terminal — no change)" while the plan was neither terminal nor changed.
/// The wording is wrong and it is REPRODUCED VERBATIM (D2) — see the report
/// for the task row.
///
/// ## EXIT CODES, AND A STALE HELP STRING
///
///     nonexistent plan       exit 1  `error: no plan with id 999`
///     non-integer id         exit 2  `error: plan id must be an integer, got 'abc'`
///     blocked, APPLY mode    exit 3  `error: plan {id} is not ready to close ({n} reason(s))`
///     blocked, --dry-run     exit 0  (full report, no error line)
///
/// `--dry-run` EXITS 0 EVEN WHEN BLOCKED. The verb's own help text in
/// `surface.cpp` claims "Hard gate failures produce a non-zero exit in both
/// dry-run and apply modes" — that sentence is STALE in the oracle and is
/// carried verbatim in this tree's already-declared surface. The handler
/// comment beside the check states the real rule (a preview must let a
/// caller read `{ready, blocked_by}` before deciding), and the measurement
/// agrees with the comment, not the help. Prose lost to code, as it has
/// every other time this milestone.
///
/// In the blocked APPLY arm the FULL report still goes to stdout; the error
/// line is stderr only. Both streams were captured separately to establish
/// that.
///
/// ## DIVERGENCE (the only one): THE GIT LAYER IS PORTED WORKING
///
/// `closeout.zig`'s git probes route through
/// `std.Io.Threaded.global_single_threaded.io()`. Under Zig 0.16.0 every
/// `std.process.run` on that handle fails with `OutOfMemory` — reproduced in
/// an eleven-line standalone program, with three different allocators, so it
/// is a property of the handle and not of this call site. The consequence:
/// `detect_target_branch` NEVER answers, so in the oracle EVERY git-evidence
/// entry is `{target_branch:null, base_merged:null, branch_merged:null,
/// note:"git-evidence unavailable"}` and every epic roll-up is
/// `{target_branch:"(unknown)", merged_count:0, note:"git-evidence
/// unavailable — epic-merge check inconclusive"}`, no matter what the
/// repository actually contains. Measured against a clone with a genuine
/// `refs/remotes/origin/HEAD`, a merged branch and an unmerged branch, where
/// the identical git commands all succeed from a shell.
///
/// This is NOT `tree --sort`'s deliberate inertness. That flag is inert by
/// the oracle's own logic and is reproduced inert. Here the oracle's SOURCE
/// unambiguously specifies the behaviour and an environment-level stdlib bug
/// prevents it running; the same binary on a fixed Zig would emit real
/// evidence with no source change. So the logic is ported over the working
/// layer-1 `planar.git`, whose `run`/`run_trimmed` contract already matches
/// the Zig helpers' exactly (non-zero exit and signal death are both "no
/// answer"; `run_trimmed` additionally treats empty output as no answer).
///
/// What that costs, stated plainly so nobody has to rediscover it:
///
///   - THE HARD GATE IS BIT-IDENTICAL. `ready`, `applied`, `blocked_by`,
///     `warnings`, every count, the exit code, the `plans` UPDATE and the
///     `audit_log` row do not read the git layer at all. Everything
///     authoritative agrees with the oracle.
///   - The ADVISORY fields diverge, and only when locality data exists. With
///     no `repo_root` on any claim — which is every arena `statediff.t.cpp`
///     builds — both sides emit the identical synthetic `(none)` entry, so
///     the differential lane stays green. A future statediff step that seeds
///     a locality-bearing claim WILL diverge here, by design; read this
///     paragraph before "fixing" it.
///
/// ## ONE ORACLE ARTIFACT, REPRODUCED AND THEN REMOVED
///
/// `branch_exists` used to carry the oracle's fixed 128-byte ref buffer and
/// return FALSE when `refs/heads/{branch}` did not fit — so a branch whose
/// name exceeded 117 bytes reported ABSENT rather than being probed, giving
/// a closeout the wrong answer silently. It was reproduced while the oracle
/// existed (D2) and REMOVED at task 6321 under decision 1067, which ended
/// the bug-for-bug rule and named this row in its FIX set.
///
/// The ceiling was always artificial in this port: the ref is formatted into
/// a `std::string`, which has no such limit. Git's own ref-name limit now
/// applies and `rev-parse --verify` reports it.
module;

export module planar.engine.planning.closeout;

import std;
import planar.db;

namespace planar::engine::planning::closeout {

/// @brief Why a closeout evaluation failed.
///
/// Mirrors the reachable arms of zig's `closeout.Error`. Its `AlreadyTerminal`
/// member has no `return` site anywhere in the oracle — the already-terminal
/// case is a successful short-circuit, not a failure — and is not reproduced.
export enum class closeout_error : std::uint8_t {
  not_found,    ///< No plan with that id. Handler: exit 1, `no plan with id {n}`.
  query_failed, ///< A SELECT or the status UPDATE failed. Handler: exit 1, `plan closeout: QueryFailed`.
  write_failed, ///< The `audit_log` INSERT failed; the whole apply rolls back. Handler: `plan closeout: WriteFailed`.
};

/// @brief Task-status counts across the plan AND its descendant plans.
export struct task_counts {
  std::int64_t open      = 0; ///< `todo` + `doing` + `blocked`. Non-zero blocks.
  std::int64_t done      = 0; ///< Terminal.
  std::int64_t cancelled = 0; ///< Terminal history — counted, never blocking.
};

/// @brief Descendant-plan counts (recursive via `parent_plan_id`).
export struct descendant_counts {
  std::int64_t open     = 0; ///< `draft` + `active` + `paused`. Non-zero blocks.
  std::int64_t terminal = 0; ///< `done` + `abandoned`.
};

/// @brief Active-claim counts, split by whether the lease has expired.
export struct claim_counts {
  std::int64_t live  = 0; ///< `status='active'` and lease in the future. Non-zero blocks.
  std::int64_t stale = 0; ///< `status='active'` but lease expired. Warns only.
};

/// @brief One advisory git-evidence entry, from one distinct
/// `(repo_root, branch, head_sha_at_claim)` tuple.
export struct git_evidence {
  std::string                repo_root;     ///< `(unknown)` when the column was NULL, `(none)` for the no-locality synthetic.
  std::optional<std::string> branch;        ///< Unset when not recorded.
  std::optional<std::string> target_branch; ///< Unset when detection failed.
  std::optional<bool>        base_merged;   ///< Unset = inconclusive / no sha to check.
  std::optional<bool>        branch_merged; ///< Unset = inconclusive / branch absent / unavailable.
  std::string                note;          ///< Human-readable status; ALWAYS set.
};

/// @brief Epic-branch merge roll-up. Present only when `check_merge` was
/// requested AND at least one claim carries both a `repo_root` and a
/// `branch`; unset otherwise (including on the already-terminal path).
export struct epic_merge_rollup {
  std::string  target_branch  = "(unknown)"; ///< Detected default branch, or `(unknown)`.
  std::int64_t total_branches = 0;           ///< Distinct non-null `(repo_root, branch)` pairs.
  std::int64_t merged_count   = 0;           ///< How many of them are merged into `target_branch`.
  std::string  note;                         ///< Human-readable summary.
};

/// @brief The DB-evidence half of the result.
export struct hard_evidence {
  task_counts       tasks;                  ///< Task roll-up.
  descendant_counts descendants;            ///< Descendant-plan roll-up.
  claim_counts      claims;                 ///< Claim roll-up.
  std::int64_t      finalization_tasks = 0; ///< Advisory label count; never consulted by the gate.
};

/// @brief The full result of one closeout evaluation.
export struct closeout_result {
  std::int64_t  plan_id = 0;     ///< The plan asked about.
  bool          ready   = false; ///< True when `blocked_by` is empty.
  bool          applied = false; ///< True only when apply ran AND the gate passed AND the plan was not already terminal.
  hard_evidence hard;            ///< The gate's evidence.
  std::vector<std::string>         blocked_by; ///< One reason per failed rule, in rule order.
  std::vector<git_evidence>        git;        ///< Advisory; empty ONLY on the already-terminal path.
  std::optional<epic_merge_rollup> epic_merge; ///< Set only when `check_merge` and locality data both hold.
  std::vector<std::string>         warnings;   ///< Advisory; stale claims land here.
};

/// @brief Evaluate the closeout gate for `plan_id`.
///
/// When `apply` is true AND the hard gate passes AND the plan is not already
/// terminal, sets `plans.status = 'done'` and writes one `status_change`
/// audit row, both inside a single `BEGIN IMMEDIATE` transaction. A failure
/// of the audit INSERT rolls the status change back and reports
/// `write_failed` — the plan is never left closed without its audit row.
/// @param conn An open, migrated database connection.
/// @param plan_id The plan to evaluate.
/// @param apply False for `--dry-run`; true otherwise.
/// @param check_merge True when `--check-merge` was supplied.
/// @return The result, or the failure.
export auto evaluate(db::connection& conn, std::int64_t plan_id, bool apply, bool check_merge)
    -> std::expected<closeout_result, closeout_error>;

/// @brief Render `plan closeout --json`.
/// @param result The evaluation.
/// @return A COMPLETE stdout payload including its trailing newline.
export auto render_json(const closeout_result& result) -> std::string;

/// @brief Render `plan closeout` in text mode.
/// @param result The evaluation.
/// @param dry_run True when `--dry-run` was supplied; prefixes every banner
/// with `[dry-run] `.
/// @return A COMPLETE stdout payload including its trailing newline.
export auto render_text(const closeout_result& result, bool dry_run) -> std::string;

} // namespace planar::engine::planning::closeout
