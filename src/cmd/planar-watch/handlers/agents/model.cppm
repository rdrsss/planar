/// @file model.cppm
/// @brief `planar.cmd.planar_watch.handlers.agents.model` — the read-only
/// snapshot behind the interactive agent view, and the flattened rows the
/// screen draws.
///
/// One agent is one claim. An orchestrator holds a claim on a plan; the
/// coders it dispatches hold claims on tasks inside that plan. The snapshot
/// gives each plan claim a row whose tree is the plan's milestones and tasks,
/// and marks every task another claim holds with that claim's state. A task
/// claim whose plan has no displayed plan claim above it gets a row of its
/// own, rooted at the task's plan.
///
/// Entry points: `build_snapshot` reads the database inside one short read
/// transaction; `flatten` turns a snapshot plus the operator's expand and
/// collapse choices into display rows. `classify` maps one claim to working,
/// waiting or stopped. Database failures return the `db::db_error` from the
/// failing statement; nothing here writes.
module;
export module planar.cmd.planar_watch.handlers.agents.model;
import std;
import planar.db;

namespace planar::cmd::watch::agents {

/// @brief The light shown beside an agent and on the task it holds.
export enum class agent_state : std::uint8_t {
  working, ///< Active claim, live lease, no `awaiting:` status.
  waiting, ///< Active claim, live lease, latest status starts with `awaiting:`.
  stopped, ///< Lease expired, or the claim reached a terminal status.
};

/// @brief One `agent_work_claims` row, with its latest heartbeat status.
export struct claim_info {
  std::int64_t               id{};                         ///< Claim row id.
  std::string                vendor;                       ///< Vendor tag.
  std::optional<std::string> role;                         ///< Role name.
  std::optional<std::string> model;                        ///< Model identifier.
  std::optional<std::string> branch;                       ///< Branch at claim time.
  std::string                entity_kind;                  ///< `plan`, `plan_step` or `task`.
  std::int64_t               entity_id{};                  ///< Claimed entity id.
  std::string                status;                       ///< Claim status column.
  std::string                claimed_at;                   ///< Creation timestamp.
  std::string                last_heartbeat_at;            ///< Last heartbeat timestamp.
  std::string                lease_expires_at;             ///< Lease expiry timestamp.
  std::optional<std::string> released_at;                  ///< Terminal timestamp.
  std::optional<std::string> release_reason;               ///< Terminal reason.
  std::optional<std::string> status_text;                  ///< Latest heartbeat `--status` text.
  agent_state                state = agent_state::stopped; ///< Classified light.
};

/// @brief One task under a plan node.
export struct task_node {
  std::int64_t              id{};    ///< Task id.
  std::string               title;   ///< Task title.
  std::string               status;  ///< Task status column.
  std::vector<std::int64_t> holders; ///< Claim ids, from the snapshot, that hold this task.
};

/// @brief One plan with its direct tasks and child plans (milestones).
export struct plan_node {
  std::int64_t           id{};       ///< Plan id.
  std::string            title;      ///< Plan title.
  std::string            status;     ///< Plan status column.
  std::vector<task_node> tasks;      ///< Tasks whose `plan_id` is this plan.
  std::vector<plan_node> milestones; ///< Child plans, in id order.
};

/// @brief One top-level agent row and the work beneath it.
export struct agent_node {
  std::int64_t                claim_id{}; ///< The claim this row is.
  std::vector<std::string>    path;       ///< Titles of the ancestors above `root`, outermost first.
  std::optional<plan_node>    root;       ///< The plan the work lives in; unset for a `plan_step` claim.
  std::optional<std::int64_t> task_id;    ///< The claimed task, for a task claim.
};

/// @brief Everything one refresh read.
export struct snapshot {
  std::string                        generated_at; ///< The `now` the snapshot was classified against.
  std::vector<agent_node>            agents;       ///< Top-level rows, live agents first.
  std::map<std::int64_t, claim_info> claims;       ///< Every claim the rows reference, by id.
};

/// @brief What `build_snapshot` reads.
export struct options {
  std::string now;           ///< Current time, `YYYY-MM-DDTHH:MM:SS.mmmZ`.
  std::string stopped_since; ///< Stopped claims older than this are left out.
};

/// @brief Classify one claim against `now`.
/// @param claim The claim; only `status`, `lease_expires_at` and `status_text` are read.
/// @param now The current time, in the database's timestamp format.
/// @return The light to show.
export auto classify(const claim_info& claim, std::string_view now) -> agent_state;

/// @brief Read every live claim and every claim stopped since `opts.stopped_since`.
///
/// Runs inside one deferred read transaction, held only for the duration of
/// the call, so the database's WAL can still checkpoint between refreshes.
/// @param conn A connection; the viewer passes its read-only handle.
/// @param opts The time window.
/// @return The snapshot, or the first statement failure.
export auto build_snapshot(db::connection& conn, const options& opts) -> std::expected<snapshot, db::db_error>;

/// @brief The current UTC time in the database's timestamp format.
/// @return For example `2026-10-09T16:16:57.460Z`.
export auto now_timestamp() -> std::string;

/// @brief A timestamp `seconds` before `stamp`, in the same format.
/// @param stamp A timestamp in the database's format.
/// @param seconds How far back.
/// @return The earlier timestamp; `stamp` unchanged when it does not parse.
export auto timestamp_minus(std::string_view stamp, std::int64_t seconds) -> std::string;

/// @brief A short human age such as `14m` or `3h`, from `then` to `now`.
/// @param then The earlier timestamp.
/// @param now The later timestamp.
/// @return The age, or an empty string when either timestamp does not parse.
export auto short_age(std::string_view then, std::string_view now) -> std::string;

/// @brief What a display row shows.
export enum class row_kind : std::uint8_t {
  agent, ///< A top-level agent.
  plan,  ///< A plan or milestone under an agent.
  task,  ///< A task under a plan.
};

/// @brief One line of the interactive view.
export struct row {
  row_kind                   kind{};             ///< What the row shows.
  std::string                key;                ///< Stable identity for expand state and selection.
  int                        depth = 0;          ///< Indentation level.
  std::string                text;               ///< The main label.
  std::string                detail;             ///< Secondary text drawn after the label.
  std::optional<agent_state> dot;                ///< The agent light, on agent rows.
  std::optional<agent_state> caret;              ///< The `>` marker, on held tasks.
  std::string                status;             ///< The task status column, on task rows.
  bool                       expandable = false; ///< Whether the row has children.
  bool                       expanded   = false; ///< Whether its children are shown.
  bool                       dim        = false; ///< Finished work, drawn muted.
};

/// @brief The operator's expand and collapse choices.
///
/// Every expandable row has a default (live agents and plans holding a
/// claimed task open; the rest closed). A key in `toggled` flips its row
/// away from that default, so the choice survives refreshes that move rows.
export struct view_state {
  std::set<std::string, std::less<>> toggled;             ///< Keys flipped from their default.
  bool                               show_stopped = true; ///< Whether stopped agents are listed.
};

/// @brief Flatten a snapshot into the rows currently visible.
/// @param snap The snapshot.
/// @param view The expand and collapse choices.
/// @return Rows in display order.
export auto flatten(const snapshot& snap, const view_state& view) -> std::vector<row>;

} // namespace planar::cmd::watch::agents
