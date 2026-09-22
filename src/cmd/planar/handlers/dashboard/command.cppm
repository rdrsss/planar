/// @file dashboard.cppm
/// @brief `planar.cmd.planar.handlers.dashboard` — the `planar dashboard`
/// leaf (plan 996, task 6329).
///
/// Port of zig/src/cmd/planar/handlers/dashboard.zig.
///
/// ## THE STATED BLOCKER WAS STALE, NOT WRONG-WHEN-WRITTEN
///
/// This leaf sat in `unported_paths()` behind "the absent layer-3 cmd
/// surface" (task 6102, echoed by src/cmd/planar/CMakeLists.txt's own
/// header, which names `dashboard` in its blocked-on-layer-3 list). That
/// was true when written. Layer 3 arrived at task 6105 and the note was
/// never revisited, so the leaf stayed parked for six milestones behind a
/// blocker that had already been removed. Every engine symbol it needs —
/// `plan::list_plans`, `agentactivity`'s claim/next-work readers, and
/// `agentrender`'s two JSON fragment writers — was already in the tree.
///
/// Recorded rather than deleted: the failure mode is a blocker note that
/// ages out silently, and it is the same shape as the `audit commits` and
/// `audit handoff-readiness` corrections in handlers/audit.cppm.
///
/// ## `--agents` IS TWO DIFFERENT VERBS, NOT A FOLD-IN FLAG
///
/// Without it the payload is `{"active_plans":[…]}` and nothing else — no
/// empty `claims` object, no empty `next_available_by_plan`. With it the
/// payload gains both keys. A port that always emits the three keys and
/// leaves two empty changes the bytes of the default shape, which is the
/// one every scripted caller reads.
///
/// ## "ACTIVE PLANS" MEANS THREE STATUSES, AND `done` IS NOT ONE
///
/// The roll-up is draft + active + paused. A `done` plan is absent from
/// both the count and the list. `plan::plan_list_filter{}` with an EMPTY
/// `statuses` vector already defaults to exactly those three, so the
/// filter is left default rather than spelled out — see that member's own
/// doc comment, which records that empty does NOT mean "every status".
///
/// ## `--scope` TAKES ONE SLUG AND IS NOT COMMA-SPLIT
///
/// `plan list --scope` splits on `,` and OR-s the members; this leaf hands
/// the raw flag value through as a single slug. `--scope repo` is a
/// REFUSAL (exit 1, `plan list: SlugNotFound`) even in a repo whose
/// cwd-derived scope prints as `repo:repo` — the accepted spelling is the
/// association slug, `project:repo`. Both arms captured from the oracle;
/// the refusal is not a defect, the two slug namespaces are simply
/// different.
///
/// ## PLAN ORDER IS THE MAP ORDER, SO THERE IS NO HASH-MAP HAZARD
///
/// `next_available_by_plan` is a JSON object keyed by plan id, which is
/// the shape task 6274's iteration-order finding was about. It is safe
/// here: the oracle builds it by walking the `active_plans` vector in
/// order, not by iterating a map, so the keys come out in plan-id order
/// on every run. Reproduced by construction rather than by sorting.
///
/// A plan with no available work emits `"<id>":[]` — present and empty,
/// not omitted. The oracle's `catch continue` skips a plan only when the
/// next-work QUERY fails, which an empty result is not.
module;

export module planar.cmd.planar.handlers.dashboard;

import std;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;
import cli11;

namespace planar::cmd::handlers {

/// @brief Handle `planar dashboard [--scope <slug>] [--agents] [--json]`.
/// @param ctx The process context.
/// @param args The parsed command line.
/// @return Success after writing the roll-up, or the scope refusal.
export auto dashboard(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the `dashboard` leaf. Folded out of the generated
/// `surface.cpp` at M11.3f (task 6636, decision 1068).
/// @param root The root app to attach it to.
export auto declare_dashboard(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
