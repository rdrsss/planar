/// @file spec_ingest.cppm
/// @brief `planar.cmd.planar.handlers.spec_ingest` — the `spec ingest` leaf
/// (plan 996, task 6365).
///
/// Port target: `zig/src/cmd/planar/handlers/spec/ingest.zig` (599 lines),
/// mirroring Go's `runSpecIngestOne`.
///
/// ## THE READ SIDE WAS ALREADY PORTED; THE COMPOSE SIDE WAS NOT
///
/// `engine_ingest` (task 6035, M4) ported `parse`, `diff`, `coverage`,
/// `render` and `materialize` — the whole read path: workbench markdown in,
/// proposed diff plus strict-gate coverage plus provenance facts out. That
/// bucket's own CMakeLists.txt documents `apply.zig` (1616 Zig lines) as
/// DELIBERATELY not ported, and gives an architectural reason: `apply`
/// reaches into `engine_planning`'s plan/task/decision/question/scenario
/// CRUD, `engine_entitylink`, and `engine_runtime.session` — six layer-2
/// `engine_*` peers, which D15/D18 forbid `engine_ingest` (also layer 2)
/// from depending on. That reasoning is sound and unchanged by this task.
///
/// What THAT note gets wrong today is a factual premise, not the
/// architecture: it says three of the six callee modules "DO NOT EXIST in
/// the C++ tree yet" (decision, question, scenario). All three exist now
/// (tasks landed between M4 and this one) — this milestone's now-familiar
/// shape of a blocker note going stale as the tree fills in underneath it.
///
/// The correct fix was already named in that same note: land the
/// composition at LAYER 3, "where composing engine buckets is exactly what
/// the layer is for" (the exact D20 shape `annotate add` and `unlink`
/// pioneered — see `src/cmd/planar/CMakeLists.txt`'s header). So `apply`'s
/// logic lives HERE, in the handler, calling `engine_planning`,
/// `engine_entitylink` and `engine_runtime` directly. No `engine_ingest`
/// module was touched or extended for this.
///
/// ## APPLY ATOMICITY MATCHES THE ORACLE
///
/// The oracle wraps every write `apply` performs — child plans, tasks,
/// decisions, scenarios, questions, dependency edges, touch links, the
/// materializer's fact-set replacement, and the anchor's draft→active flip
/// — inside ONE SQLite SAVEPOINT, so a failure partway through rolls back
/// everything already written in that call.
///
/// This handler opens one outer `planar.db::transaction` for the full apply.
/// `planar.db` detects that the composed engine CRUD operations run inside an
/// existing transaction and gives each its own uniquely named SAVEPOINT;
/// their commits release only that nested scope. An apply failure then lets
/// the outer RAII transaction roll back the complete derived graph. This is
/// the C++ equivalent of the oracle's `spec_ingest_apply` savepoint, including
/// the oracle's all-or-nothing failure contract.
///
/// ## THE VARIADIC "BATCH" POSITIONAL
///
/// `spec ingest <p1> <p2> <p3>` processes each argument in turn (Go/Zig
/// parity: the oracle's `rest_field = "extra_plans"`). CLI11's declarative
/// surface table has no variadic-positional primitive, so — mirroring
/// `capture commits`'s hidden `shas` positional — `tree.cpp` hand-declares a
/// second, hidden `extra-plans` positional with `expected(0, -1)` alongside
/// the required `plan` one.
module;

export module planar.cmd.planar.handlers.spec_ingest;

import std;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief `planar spec ingest <plan> [<plan>...] [--apply] [--apply-removals]
/// [--strict] [--json] [--format text|json] [--scope <slug>]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto spec_ingest(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
