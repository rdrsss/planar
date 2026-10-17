/// @file conformance.cppm
/// @brief `planar.engine.runtime.conformance` — a read-only grade of one plan's trail against the
/// coordination rules (plan 1142, tech spec component 10).
///
/// The checker reads a plan, its descendant plans, their tasks and the claims taken on any of them,
/// and answers one question for each: did the work leave the trail the mechanism promises? It is the
/// same for every orchestrator, because it reads only what the verbs recorded and never asks which
/// prompt, vendor or role convention produced it.
///
/// ## Rules
///
/// 1. **Completed claim.** Every `done` task had a claim that ended `completed`. A task whose trailing run of
///    done-valued status changes (the `status_change` rows since its last status change that is not a `done`:
///    summary `done`, `done (--force)` or none) holds an operator `--force` to `done` (summary
///    `done (--force)`) is listed as an override, not a violation, even when a no-op `task done` followed it; a
///    reopen or any other status change ends the run, so force, reopen and a plain `done` is an ordinary done
///    task again. The override says whether a claim was live when it was forced.
/// 2. **Terminal verb.** Every claim ended through a terminal verb: a claim marked `stale`, or still `active`
///    past its lease expiry, is a violation. The second half reads the clock, so the same database can pass
///    now and fail later; a live `active` claim is work in progress.
/// 3. **Model.** Under `require_model`, every `coder` claim records a non-empty model.
/// 4. **Parent action.** In a plan tree where an orchestration is visible (some claim has the role
///    `orchestrator`, or some claim has a parent action), every claim with a role other than `orchestrator`
///    has a root action under a parent action. A claim's root action is its first non-review action; a child
///    that the claim's own worker nests under that root does not count, and neither does a parent that belongs
///    to the same claim. A claim with no role is never graded, and a tree with no orchestration (a solo session)
///    grades no claim on this rule.
/// 5. **Queued gate.** Only when `use_queue` AND a `task_gate` are both set (`use_queue` alone grades nothing),
///    every completed task claim has a passing queued run of exactly the gate argv tagged with its token, run in
///    its directory. The runs arrive through the injected `gate_run_lookup`, and the build queue database's header
///    through the injected `queue_header_reader`, because that database is read by a layer-1 module this bucket
///    does not link; the requirement itself is `completion::unmet_completion_requirements`, the function
///    `complete` applies. The database keeps tagged runs only for `tagged_history_days`, and exists only since it
///    was created, so a claim is graded only if it ended at or after the **evidence horizon**: the later of the
///    database's `created_at` and the clock minus `tagged_history_days`; with no database, the `applied_at` of
///    `planar.db`'s migration 44 (the split). A claim that ended before the horizon is listed in
///    `queue_evidence_unavailable` and not graded on this rule, as `policy_changed` is. When no horizon is known
///    (no database and no record of migration 44) every gated claim is listed there. With no database, a claim
///    after the horizon has no evidence and fails with the reason `no build queue database at <path>`. A database
///    that is busy, unreadable or incompatible refuses the whole check (`queue_unreadable`) and grades nothing.
/// 6. **Independent review.** Under `review_required`, every completed task claim passes the same review test
///    `complete` applies: the latest review (a row with a non-NULL verdict) approves and was written by
///    another session. A NULL-verdict row is never a review.
///
/// ## Policy
///
/// The effective policy arrives through the injected `policy_resolver`, per plan, because the config module is
/// another bucket's. A claim records only the digest of the policy it ran under, so the policy rules (3, 5 and
/// 6) grade a claim only when its digest equals the resolved policy's, or when it recorded none (graded under
/// the plan's current policy). A claim whose digest differs ran under a policy the checker cannot reconstruct:
/// it is listed in `policy_changed` and still graded on the policy-independent rules (1, 2 and 4). The digest
/// covers every policy key, including the display-only `confirm_before` and `require_scenario_coverage`, so
/// editing either one also moves earlier claims into `policy_changed`.
///
/// ## The floor
///
/// History from before the first release that carries this check never fails; it is counted
/// separately. The cut-over is a claim id, `floor_claim_id`: claims with a lower id are "before the
/// floor". A task has no claim id, so the floor carries a time as well, `cut_at`, and a done task that became
/// done earlier is before the floor. When a task became done is the start of its trailing done run, so a no-op
/// `task done` later does not move it; a task whose latest status change is not a done (it became done through
/// `planar-agent complete`, which writes no audit row) uses its `updated_at`. The default is read from the
/// database, so it works on every install without a baked-in id: the cut-over is when migration 00042 was applied
/// (`schema_migrations.applied_at`); the floor is the lowest claim id claimed at or after that moment
/// (one past the highest id when there is none), and `cut_at` is that moment. When no row records
/// migration 42, the floor is the lowest claim id that recorded a policy digest (a column 00042
/// added) and `cut_at` is that claim's `claimed_at`. An explicit `floor_claim_id` replaces the
/// default: `cut_at` is then the `claimed_at` of the lowest claim at or above it, and everything is
/// graded when no claim lies below it.
///
/// Read-only: SELECTs only. Error boundary: `std::expected`; nothing throws.
module;

export module planar.engine.runtime.conformance;

import std;
import planar.db;
import planar.engine.runtime.completion;

namespace planar::engine::runtime::conformance {

/// @brief The rules, numbered as the tech spec numbers them.
export enum class rule : std::uint8_t {
  completed_claim    = 1, ///< A done task had a claim that ended `completed`.
  terminal_verb      = 2, ///< A claim ended in a terminal verb.
  coder_model        = 3, ///< Under `require_model`, a coder claim records a model.
  parent_action      = 4, ///< A worker's claim carries a parent action.
  queued_gate        = 5, ///< Under `use_queue`, a completed claim has a queued gate run.
  independent_review = 6, ///< Under `review_required`, a completed claim has an independent review.
};

/// @brief The rule's number, 1 through 6.
/// @param value The rule.
/// @return The number.
export auto rule_number(rule value) -> int;

/// @brief The rule's short name, for example `completed-claim`.
/// @param value The rule.
/// @return The kebab-case name.
export auto rule_name(rule value) -> std::string_view;

/// @brief The policy keys the checker reads for one plan, with the digest the claims recorded.
export struct policy_view {
  bool                                    review_required = false; ///< Rule 6 applies.
  std::optional<std::vector<std::string>> task_gate;               ///< Rule 5 applies when set together with `use_queue`.
  bool                                    require_model = false;   ///< Rule 3 applies.
  bool                                    use_queue     = false;   ///< Rule 5 applies when set together with `task_gate`.
  std::string digest; ///< `policy_digest` of the resolved values; compared with `agent_work_claims.policy_digest`.
};

/// @brief The build queue database's header, as the evidence horizon needs it.
export struct queue_header {
  std::string                path;                     ///< Where the database is expected, for the missing-database reason.
  bool                       present = false;          ///< Whether the file exists.
  std::optional<std::string> created_at;               ///< `queue_identity.created_at` (ISO-8601 text); unset when absent.
  int                        tagged_history_days = 90; ///< Retention for tagged runs, in days.
};

/// @brief Reads the build queue database's header, once per check, and only when a gated claim is graded.
///
/// Injected because the reader is not this bucket's. The string is the reason the database could not be read
/// (busy, unreadable or incompatible, or no location could be resolved).
export using queue_header_reader = std::function<std::expected<queue_header, std::string>()>;

/// @brief Resolves the effective policy for a plan's scope.
///
/// Injected because the config module belongs to another bucket. The string is the reason the policy
/// could not be resolved (an unreadable or invalid config), shown to the operator.
export using policy_resolver = std::function<std::expected<policy_view, std::string>(db::connection& conn, std::int64_t plan_id)>;

/// @brief One broken rule.
export struct violation {
  conformance::rule          broken{};       ///< Which rule.
  std::string                entity_kind;    ///< `task` or `claim`.
  std::int64_t               entity_id = 0;  ///< The task's or the claim's row id.
  std::optional<std::string> claim_token;    ///< The claim's token, for a claim violation.
  std::optional<std::string> claimed_entity; ///< What the claim holds, for example `task:12`.
  std::string                reason;         ///< One line, no trailing punctuation.
};

/// @brief One done task an operator forced, which is listed and not graded.
export struct override_entry {
  std::int64_t task_id = 0; ///< The task.
  std::string  reason;      ///< One line naming the audit row.
};

/// @brief A claim graded only on the policy-independent rules.
export struct policy_changed_claim {
  std::int64_t claim_id = 0; ///< The claim.
  std::string  claim_token;  ///< Its token.
};

/// @brief A gated claim that ended before the evidence horizon, so rule 5 did not grade it.
export struct queue_evidence_unavailable_claim {
  std::int64_t claim_id = 0; ///< The claim.
  std::string  claim_token;  ///< Its token.
};

/// @brief How the floor was chosen.
export enum class floor_origin : std::uint8_t {
  migration, ///< The moment migration 00042 was applied.
  digest,    ///< The lowest claim that recorded a policy digest.
  explicit_, ///< The caller named a claim id.
};

/// @brief The wire spelling of a floor origin: `migration`, `digest` or `explicit`.
/// @param value The origin.
/// @return The name.
export auto floor_origin_name(floor_origin value) -> std::string_view;

/// @brief The cut-over the grade used.
export struct floor_info {
  std::int64_t               claim_id = 0;                       ///< Claims with this id or higher are graded.
  floor_origin               origin   = floor_origin::migration; ///< How it was chosen.
  std::optional<std::string> cut_at; ///< Done tasks that became done at or after this are graded; unset grades none.
};

/// @brief What lies before the floor, which never fails.
export struct before_floor {
  std::int64_t claims     = 0; ///< Claims of the plan with an id below the floor.
  std::int64_t done_tasks = 0; ///< Done tasks of the plan that became done before the cut-over.
};

/// @brief Inputs to `check`.
export struct options {
  std::int64_t                plan_id = 0;    ///< The plan to grade, with its descendants.
  std::optional<std::int64_t> floor_claim_id; ///< Replaces the data-derived floor when set.
  /// @brief A fixed clock for the lease-expiry comparison, as `%Y-%m-%dT%H:%M:%fZ`. Unset means now.
  std::optional<std::string> now;
};

/// @brief The grade of one plan.
export struct report {
  std::int64_t                                  plan_id = 0;                ///< The graded plan.
  std::int64_t                                  plans   = 0;                ///< Plans graded: the plan and its descendants.
  floor_info                                    floor;                      ///< The cut-over used.
  std::int64_t                                  claims_graded     = 0;      ///< Claims at or above the floor.
  std::int64_t                                  done_tasks_graded = 0;      ///< Done tasks at or after the cut-over.
  std::vector<violation>                        violations;                 ///< Ordered by rule, then entity kind, then id.
  std::vector<override_entry>                   overrides;                  ///< Forced done tasks, ordered by task id.
  std::vector<policy_changed_claim>             policy_changed;             ///< Claims not graded on the policy rules.
  std::vector<queue_evidence_unavailable_claim> queue_evidence_unavailable; ///< Gated claims not graded on rule 5: too old.
  before_floor                                  before;                     ///< Counted, never graded.

  /// @brief Whether the trail broke no rule.
  /// @return True when `violations` is empty.
  [[nodiscard]] auto conforms() const -> bool {
    return violations.empty();
  }
};

/// @brief Why `check` refused.
export enum class error_kind : std::uint8_t {
  plan_not_found,     ///< The plan does not exist.
  query_failed,       ///< SQL failure.
  policy_unavailable, ///< The policy resolver failed; `detail` says why.
  queue_unreadable,   ///< The build queue database could not be read; `detail` says why.
};

/// @brief A refusal with its detail.
export struct check_error {
  error_kind  kind = error_kind::query_failed; ///< Which refusal.
  std::string detail;                          ///< For `policy_unavailable` and `queue_unreadable`, the reason; otherwise empty.
};

/// @brief Resolve the floor without grading: the default for this database, or the explicit one.
/// @param conn An open connection (read-only is sufficient).
/// @param explicit_floor A claim id to use instead of the data-derived default.
/// @return The floor, or `query_failed`.
export auto resolve_floor(db::connection& conn, std::optional<std::int64_t> explicit_floor)
    -> std::expected<floor_info, check_error>;

/// @brief Grade one plan, its descendants, their tasks and the claims taken on them.
/// @param conn An open connection (read-only is sufficient).
/// @param opts The plan, the optional floor and the optional clock.
/// @param resolve Resolves the effective policy of a plan's scope; called once per plan that has a graded claim.
/// @param lookup Reads the runs tagged with a claim's token, for rule 5.
/// @param header Reads the build queue database's header, for rule 5's evidence horizon.
/// @return The grade, or why it could not be made.
export auto check(db::connection& conn, const options& opts, const policy_resolver& resolve,
                  const completion::gate_run_lookup& lookup, const queue_header_reader& header)
    -> std::expected<report, check_error>;

/// @brief Render a grade the way `planar health conformance` prints it without `--json`.
/// @param value The grade.
/// @return The complete stdout payload including its trailing newline.
export auto render_text(const report& value) -> std::string;

/// @brief Render a grade as the `--json` payload.
/// @param value The grade.
/// @return One JSON object and a trailing newline.
export auto render_json(const report& value) -> std::string;

} // namespace planar::engine::runtime::conformance
