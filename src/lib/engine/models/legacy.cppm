/// @file legacy.cppm
/// @brief `planar.engine.models.legacy` — the note-convention scorecard behind
/// `planar models evals` when NO cohort flags are supplied (plan 996, task
/// 6111).
///
/// Behavior-preserving port (D2) of zig/src/engine/evals.zig plus the
/// legacy half of `handleEvals` in zig/src/cmd/planar/handlers/models.zig.
///
/// ## What this is, and why it is labelled legacy
///
/// `models evals` has two entirely separate implementations behind one verb,
/// selected by ONE flag:
///
///     if (optFlag(args.vendor)) |vendor| { rankCohort(...); return; }
///
/// With `--vendor`, the evidence-backed cohort ranking runs (ported in task
/// 6096; see ranking.cppm and its Wilson bound). Without it, control falls
/// through to THIS aggregation, which predates the routing evidence plane. It
/// mines a note convention out of `session_entries` rather than reading
/// declared experiments, and the verb's own long help says so: "not
/// evidence-backed... carries no cohort or independent-quality guarantee".
///
/// Every other cohort flag is silently IGNORED on this path. Oracle-confirmed:
/// `models evals --role coder --tier medium --json` runs the legacy branch and
/// discards both flags rather than refusing. `--vendor` alone refuses at the
/// cohort branch with `error: --project is required when ranking a cohort`.
///
/// ## The note convention
///
/// The orchestrator writes one `session_entries` note per dispatch cycle
/// (agents/orchestrator.md step 8a) containing a `dispatch_shape:` marker and a
/// `model_choice:` line whose value is a JSON object keyed by TASK ID:
///
///     dispatch_shape: fan-out
///     model_choice: {"41":{"tier":"medium","candidate":"c-1","work_type":"feature"}}
///
/// The parse is deliberately forgiving — a malformed note is a silent skip,
/// never an error — and forgiving in a specific, load-bearing way:
///
///   - Only the FIRST `model_choice:` line in a note body is read. A second is
///     ignored, because the reader returns as soon as it has processed one.
///   - A key that does not parse as an integer is skipped entirely.
///   - `iterations` is incremented for a task BEFORE the triple is validated.
///     So a note naming a task with an incomplete triple still counts as an
///     iteration for that task — which is exactly right, since the dispatch
///     did happen; only its classification is unusable.
///   - A task whose notes NEVER carried a complete triple is dropped from
///     scoring and counted in `legacy_dispatch_notes_skipped`. That counter
///     counts TASKS, not notes, despite its name.
///   - A note with `dispatch_shape:` and no `model_choice:` line at all
///     contributes NOTHING and is NOT counted. Oracle-confirmed: such a
///     fixture left the counter at 0, and adding a `model_choice` entry with a
///     missing `work_type` raised it to 1.
///
/// ## Three signals, read from three different tables
///
///   reviewer disposition  the most recent `agent_work_claims` row for the
///                         task: `completed` -> approved, `aborted` ->
///                         aborted, anything else -> other. NO row is also
///                         `other` — an undispatched task is not a success.
///   iteration count       the number of dispatch notes naming the task.
///   test-coder expansion  the most recent `agent_actions` row with
///                         `action_kind='test_coder'`: `ok` -> ok, anything
///                         else -> other. NO row contributes to NEITHER
///                         counter, so the two need not sum to dispatch_count.
///
/// The fourth signal in the design, quality-gate pass/fail, is not persisted
/// anywhere in the schema. `signals_sourced.quality_gate_pass_fail` is
/// therefore hardcoded FALSE — see signals_sourced.
///
/// ## Vendor is read back, never inferred
///
/// `vendor` comes from the most recent `agent_work_claims` row whose `model`
/// equals the candidate string. Planar removed its model catalog in plan 950
/// precisely so it would stop making support claims about which vendor serves
/// which model; this reads back what an agent actually recorded. Null when no
/// claim carries that string, and rendered as `?` in text mode.
///
/// ## Two branches that are dead in the current implementation
///
/// `insufficient_data` is ALWAYS false and `rank` is therefore never null.
/// Sibling enumeration — "every candidate in the same effective list that has
/// no history" — was deleted with the model catalog, so rows exist only for
/// candidates with observed history. The struct fields and both render arms
/// are kept because they are part of the wire contract, but no fixture can
/// exercise them end-to-end and this port does not pretend otherwise: the
/// tests drive the insufficient-data render arm directly.

module;

export module planar.engine.models.legacy;

import std;
import planar.db;

namespace planar::engine::models::legacy {

/// @brief Which of the design's four signals this aggregation actually sources.
///
/// A CAPABILITY statement, not a count: every field is a compile-time constant
/// describing what the implementation reads at all, and none of them varies
/// with the data. `quality_gate_pass_fail` is false on every database,
/// including a fully populated one, because gate output is pasted into a
/// coder's report and never persisted as a discrete pass/fail row. Reporting
/// it as false rather than fabricating a proxy is the point.
export struct signals_sourced {
  bool reviewer_disposition   = true;  ///< From `agent_work_claims.status`.
  bool iteration_count        = true;  ///< From the count of dispatch notes.
  bool quality_gate_pass_fail = false; ///< Not persisted anywhere. Always false.
  bool test_coder_expansion   = true;  ///< From `agent_actions.outcome`.
};

/// @brief One per-(work-type, candidate) scorecard row.
export struct score_row {
  std::string                work_type;                      ///< From the note's triple.
  std::string                candidate;                      ///< The opaque candidate string.
  std::optional<std::string> vendor;                         ///< Read back from a claim; null when unknown.
  std::string                tier;                           ///< From the FIRST task folded into this group.
  std::size_t                dispatch_count         = 0;     ///< Tasks in this group.
  std::size_t                approved_count         = 0;     ///< Claims that ended `completed`.
  std::size_t                aborted_count          = 0;     ///< Claims that ended `aborted`.
  std::size_t                other_count            = 0;     ///< Everything else, INCLUDING no claim at all.
  double                     approval_rate          = 0;     ///< approved_count / dispatch_count.
  double                     avg_iterations         = 0;     ///< total notes / dispatch_count.
  std::size_t                test_coder_ok_count    = 0;     ///< Test-coder actions with `outcome='ok'`.
  std::size_t                test_coder_other_count = 0;     ///< Test-coder actions with any other outcome.
  bool                       insufficient_data      = false; ///< Always false; see this module's header.
  std::optional<std::size_t> rank;                           ///< 1-based within the work type.
};

/// @brief A preview-only routing recommendation. Writes nothing, ever.
export struct recommendation {
  std::string                work_type; ///< The work type this covers.
  std::optional<std::string> vendor;    ///< Copied from the winning row.
  std::string                tier;      ///< Copied from the winning row.
  std::string                candidate; ///< The rank-1 candidate.
  std::string                rationale; ///< Human-readable justification.
};

/// @brief The whole aggregation's output.
export struct result {
  std::vector<score_row>      scorecard;                         ///< Sorted; see aggregate().
  std::vector<recommendation> recommendations;                   ///< One per work type with a rank-1 row.
  legacy::signals_sourced     signals_sourced;                   ///< Constant capability statement.
  std::size_t                 legacy_dispatch_notes_skipped = 0; ///< TASKS with no complete triple.
};

/// @brief The `{tier, candidate, work_type}` triple plus the iteration count,
/// accumulated per task across every note that names it.
export struct task_info {
  std::string work_type;          ///< Last complete note's work type.
  std::string candidate;          ///< Last complete note's candidate.
  std::string tier;               ///< Last complete note's tier.
  std::size_t iterations = 0;     ///< Notes naming this task, complete or not.
  bool        has_info   = false; ///< Whether any note carried a COMPLETE triple.
};

/// @brief Fold one note body's `model_choice:` line into `tasks`.
///
/// Exposed so the forgiving-parse rules can be pinned without a database.
/// Reads only the FIRST `model_choice:` line and returns; a second is ignored.
/// Every failure — no line, empty value, non-JSON, non-object, non-integer key,
/// missing or non-string triple field — is a SILENT skip. `iterations` is still
/// incremented for any integer-keyed entry, before the triple is validated.
/// @param body The note's full text.
/// @param tasks Accumulator, keyed by task id; updated in place.
export auto process_dispatch_body(std::string_view body, std::map<std::int64_t, task_info>& tasks) -> void;

/// @brief Build the scorecard and its recommendations. Reads only; writes
/// nothing to the database or the filesystem.
///
/// ORDERING, which is a contract because it determines `rank`: work_type
/// ascending bytewise, then scored rows before insufficient ones, then
/// approval_rate DESCENDING, then avg_iterations ASCENDING, then candidate
/// ascending bytewise. The last key makes the order total, so two runs over the
/// same database are byte-identical despite the hash-map accumulation in the
/// middle.
///
/// `rank` restarts at 1 at every work_type boundary in that sorted sequence.
/// @param conn An open connection.
/// @return The aggregation, or `std::nullopt` when a query failed.
export auto aggregate(db::connection& conn) -> std::optional<result>;

/// @brief Render `models evals --json` for the legacy branch.
///
/// The envelope is HAND-ASSEMBLED from four pieces with literal separators
/// rather than serialized from one struct, so the key order is fixed by the
/// format string: scorecard, recommendations, signals_sourced,
/// legacy_dispatch_notes_skipped.
/// @param value The aggregation.
/// @return One JSON object, newline-terminated.
export auto evals_json(const result& value) -> std::string;

/// @brief Render `models evals` (no `--json`) for the legacy branch.
///
/// Three sections separated by blank lines, each with its own empty-state
/// sentence, then a trailing `note:` line only when tasks were skipped. Every
/// dash in the section headers is U+2014 EM DASH.
/// @param value The aggregation.
/// @return The complete stdout payload.
export auto evals_text(const result& value) -> std::string;

/// @brief The refusal `models evals --vendor <v>` emits with no `--project`.
///
/// The cohort branch's own guard, included here because it is the ONLY thing
/// `--vendor` does when the rest of the cohort is unspecified, and because it
/// is what distinguishes "fell through to legacy" from "tried to rank".
/// @return The line, newline-terminated.
export auto cohort_requires_project_error() -> std::string;

} // namespace planar::engine::models::legacy
