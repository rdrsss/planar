/// @file descendants.cppm
/// @brief `planar.engine.planning.descendants` — the feature-tree walk behind
/// `plan descendants` (plan 996, task 6298).
///
/// Port target: the `TreeEntry` / `walkTree` / `freeTree` triple from
/// `zig/src/engine/extsync/propagate.zig` (lines 200-291 of 409).
///
/// ## IT LIVES IN `engine_planning`, NOT `engine_extsync`, AND THAT IS
/// ## FORCED BY AN INVARIANT RATHER THAN CHOSEN FOR TIDINESS
///
/// The oracle keeps this walk in `engine/extsync/propagate.zig`, so mirroring
/// the oracle's layout would put it in `planar.engine.extsync`. It cannot go
/// there. `src/engine/extsync/CMakeLists.txt` states the bucket's whole
/// point: it "does NOT depend on `db` — an adapter talks to a provider, never
/// to SQLite, and keeping the edge out is what makes every test in this
/// bucket runnable with no database at all." This walk is three SQL queries
/// and nothing else, so landing it there would mean adding the exact edge
/// that invariant exists to forbid, and every adapter test would acquire a
/// database dependency to host one function none of them call.
///
/// `engine_planning` already carries the `db` edge and already owns the
/// `plans` / `tasks` tables this reads. The C++ bucket boundaries are drawn
/// differently from the Zig ones here, and this is one of the places the
/// difference is load-bearing rather than cosmetic.
///
/// The rest of `propagate.zig` stays unported and `ext propagate` stays on
/// the unported inventory. `strategyForSystem`, `selectStrategy`,
/// `countDistinctReposInFeature`, `hasExistingMirrorLink` and
/// `loadExistingMirror` are all unreached from this leaf -- reading the LEAF
/// rather than the MODULE is what made `plan descendants` portable at all,
/// the same correction task 6272 made on `audit commits` and task 6294 made
/// on the three `sync` write leaves.
///
/// ## ONE SURPRISING BEHAVIOUR REMAINS; THE OTHER WAS A DEFECT AND IS FIXED
///
/// **`tasks.plan_id` IS CONSULTED, ALONGSIDE `derives-from` (task 6307).**
/// A task reaches the tree either by carrying `plan_id` of a plan in the
/// walk, or through an `entity_links` row `(from_kind='task',
/// to_kind='plan', relationship='derives-from')`. `distinct` collapses a
/// task that arrives by both routes for the same plan.
///
/// It used to read the EDGE ONLY, which was the oracle's behaviour and was
/// reproduced under D2. Measured then: a fixture with three tasks created as
/// `task add --plan N` and no links returned the four PLANS and ZERO tasks,
/// while `plan next` on the same database listed all three. The two verbs
/// disagreed about what a plan's tasks are, and `descendants` was the one
/// that was wrong -- `task add --plan` is the ordinary way to attach a task
/// and writes no edge.
///
/// The failure mode is why it outranked the other findings in its milestone:
/// the answer was CONFIDENT, WELL-FORMED and EMPTY OF TASKS, indistinguishable
/// from a plan that genuinely has none. An operator asking "what is under
/// this plan before I close it" was told nothing was.
///
/// The relationship filter still applies to the EDGE route: a `cites` link
/// from a task whose `plan_id` is outside the walk does NOT reach the tree.
/// `plan_descendants_sync_status_leaves.t.cpp`'s T-five exists to pin that,
/// because the fixture's older discriminator (T-four) is `--plan 1` and now
/// arrives by the plan_id route regardless of its edge.
///
/// **A TASK LINKED TO N PLANS IN THE TREE APPEARS N TIMES.** The `distinct`
/// is scoped to one query and the query runs once per plan. With `task:1`
/// deriving from both `plan:1` and `plan:2`, the oracle's
/// `plan descendants 1` emitted `{"kind":"task","role":"task","id":1,...}`
/// twice, adjacently. Reproduced deliberately rather than de-duplicated:
/// de-duplicating is a silent behaviour change on a verb whose output
/// another tool may already be counting, and the fix belongs to the oracle.
module;

export module planar.engine.planning.descendants;

import std;
import planar.db;

namespace planar::engine::planning::descendants {

/// @brief Which kind of entity a walked row is, and the role it plays.
///
/// `plan_anchor` and `plan_child` both render `kind: "plan"` and differ only
/// in the rendered `role`, so the pair cannot collapse to a bool without
/// losing the distinction the renderer needs.
export enum class entry_kind : std::uint8_t {
  plan_anchor, ///< The plan the walk started from.
  plan_child,  ///< Any plan reached by `parent_plan_id`, at any depth.
  task,        ///< A task linked `derives-from` to some plan in the tree.
};

/// @brief One entity discovered while walking a feature tree.
export struct tree_entry {
  entry_kind   kind = entry_kind::plan_anchor; ///< What it is.
  std::int64_t id   = 0;                       ///< The row id in its own table.
  std::string  title;                          ///< `coalesce(title, '')`.
};

/// @brief Why a walk failed.
export enum class descendants_error : std::uint8_t {
  not_found,    ///< The anchor plan does not exist.
  query_failed, ///< SQL failure.
};

/// @brief Enumerate the entities under `anchor_plan_id`.
///
/// Order is anchor, then every descendant plan breadth-first by
/// `parent_plan_id` (each level ordered by id), then tasks -- the task pass
/// runs once per plan in the collected set, in that same order, each pass
/// ordered by task id. The result is FLAT: a grandchild is emitted as
/// `plan_child` exactly like a direct child, with no depth marker, so the
/// hierarchy cannot be reconstructed from the output.
///
/// An unknown anchor returns `not_found` from the anchor's own title lookup.
/// `plan descendants` refuses earlier with its own `count(*)` probe, so from
/// the CLI that arm is unreachable; it is still returned rather than
/// asserted because this is a library entry point.
/// @param conn An open, migrated database connection.
/// @param anchor_plan_id The plan to walk from.
/// @return The entries in walk order, or the failure.
export auto walk_tree(db::connection& conn, std::int64_t anchor_plan_id)
    -> std::expected<std::vector<tree_entry>, descendants_error>;

} // namespace planar::engine::planning::descendants
