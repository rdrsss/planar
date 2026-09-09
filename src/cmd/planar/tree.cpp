/// @file tree.cpp
/// @brief Implementation of `planar.cmd.planar.tree`.

module planar.cmd.planar.tree;

import std;
import cli11;
import planar.cliapp.surface;
import planar.cmd.planar.declare;
import planar.cmd.planar.surface;
import planar.cmd.planar.handlers.annotate;
import planar.cmd.planar.handlers.assoc;
import planar.cmd.planar.handlers.artifact;
import planar.cmd.planar.handlers.audit;
import planar.cmd.planar.handlers.capture;
import planar.cmd.planar.handlers.config;
import planar.cmd.planar.handlers.decision;
import planar.cmd.planar.handlers.feedback;
import planar.cmd.planar.handlers.handoff;
import planar.cmd.planar.handlers.links;
import planar.cmd.planar.handlers.local;
import planar.cmd.planar.handlers.models;
import planar.cmd.planar.handlers.plan;
import planar.cmd.planar.handlers.question;
import planar.cmd.planar.handlers.runs;
import planar.cmd.planar.handlers.scenario;
import planar.cmd.planar.handlers.scope;
import planar.cmd.planar.handlers.task;
import planar.cmd.planar.handlers.templates;
import planar.cmd.planar.handlers.workbench;
import planar.cmd.planar.handlers.workspace;

namespace planar::cmd {

namespace {

// `add_json`, `add_string`, `add_int` and `add_bool` used to be declared
// here. They moved to `planar.cmd.planar.declare` at M11.3a (task 6631):
// the folded per-domain declarations under `handlers/` need the same
// primitives, and a second private copy of them is the "too spread out"
// shape M11 exists to remove. Call sites below are unchanged --
// unqualified lookup finds them in the enclosing `planar::cmd`.

/// @brief The `workflow` group — transcribed from
/// zig/src/cmd/planar/handlers/workflow/cmd.zig. `run` is absent: it is
/// deferred with its process-spawn dependency (see
/// src/lib/engine/workflows/CMakeLists.txt), so this group's own help page
/// lists two commands where the oracle lists three.
/// @param root The root app to attach the group to.
auto add_workflow(CLI::App& root) -> void {
  CLI::App* workflow = root.add_subcommand("workflow", "Enumerate, inspect, and invoke shipped and sandbox Lua workflows.\n\n"
                                                       "  Shipped workflows live at $PLANAR_HOME/workflows/ (default\n"
                                                       "  ~/.planar/workflows/).  Sandbox workflows live at\n"
                                                       "  ~/.planar/local/workflows/ and are marked `local`.\n\n"
                                                       "  These commands are READ-ONLY w.r.t. SQLite.  `run` delegates\n"
                                                       "  execution to `planar-execute` and forwards its output + exit code.");
  workflow->require_subcommand(0);

  CLI::App* list = workflow->add_subcommand("list", "List shipped and sandbox workflows.");
  cliapp::add_bool_flag(*list, "--local");
  add_json(*list);

  CLI::App* show = workflow->add_subcommand("show", "Show @meta and source path for a named workflow.");
  add_json(*show);
  show->add_option("name")->required();
}

/// @brief The `unlink` leaf — transcribed from
/// zig/src/cmd/planar/handlers/unlink.zig. A top-level LEAF, not a group.
///
/// `<link-id>` is declared as a plain STRING positional with no validator,
/// deliberately: the oracle's node declares `.kind = .string` and the
/// handler converts with Zig's `parseInt`, so `unlink abc` reaches the
/// HANDLER's refusal (exit 1), not a parse error (exit 2). Attaching
/// `zig_int_validator` here would move that failure a layer earlier and
/// change both the message and the exit code. The parity suite pins the
/// whole `1_0` / `007` / `+12` / `_10` / `10_` / `0x10` / overflow table
/// against the live oracle through this leaf.
/// @param root The root app to attach the leaf to.
auto add_unlink(CLI::App& root) -> void {
  CLI::App* unlink = root.add_subcommand("unlink", "Remove an external_links row by its link id.\n\n"
                                                   "  Associated sync_events rows are detached by setting link_id to null\n"
                                                   "  rather than cascade-deleted; they are no longer reachable through\n"
                                                   "  the deleted link's audit trail.");
  unlink->add_option("--scope")->description("Scope for the cross-scope guard (currently informational)");
  add_json(*unlink);
  unlink->add_option("link-id")->description("External-link id (integer)")->required();
}

// The `ext` group (register/list/test/create/propagate-one) and the whole
// `sync` group (pull/push/status/resolve) moved off this binary at plan
// 996, task 6419 — see `src/cmd/planar-ext/tree.cpp`'s `add_ext`/`add_sync`
// for the destination. `ext propagate`, the one remaining unported leaf of
// the family, moved with them conceptually but is NOT yet declared on
// EITHER binary: it stays blocked on the same surfaces
// `k_unported`'s header always named (the feature-tree walk, the
// create/propagate half of `engine_extsync`), and lands on `planar-ext`
// once a later step of task 6412's extraction closes that gap. `ext
// register jira`/`github`'s auth-resolution FACTORY
// (`ext_adapter_factory.cppm`) stayed here — `audit publish-decision` and
// `workbench publish` are its other two callers and neither moved, and
// D18 forbids a `cmd_planar -> cmd_planar_ext` edge — so
// `src/cmd/planar-ext/handlers/ext_adapter_factory.cppm` is a deliberate
// duplicate, not a shared import. Same shape for the cross-scope guard
// (`planar.cmd.planar.scope`'s `guard_with_membership`/
// `resolve_write_scope`, also needed by `feedback triage set`, which
// stayed): `src/cmd/planar-ext/scope.cppm` duplicates it.

/// @brief The `skills` node — transcribed from
/// zig/src/cmd/planar/handlers/skills/cmd.zig.
///
/// Zero subcommands and zero flags, which is the whole point: plan 918 M5
/// retired `render`/`status`/`repair` and left the node registered so the
/// verb reports "no subcommands" rather than an unknown-verb error. With
/// no children it is a LEAF and needs a handler — see
/// `planar.cmd.planar.handlers.skills`.
/// @param root The root app to attach the leaf to.
auto add_skills(CLI::App& root) -> void {
  root.add_subcommand("skills", "The unified skill source tree under skills/src/ is rendered by the\n"
                                "  external scriptorium binary (plan 918). Planar no longer renders vendor\n"
                                "  projections nor tracks their install-drift in-band; use `scriptorium\n"
                                "  check`/`scriptorium status` instead. This command has no subcommands.");
}

/// @brief The `resume` group — transcribed from
/// zig/src/cmd/planar/handlers/resume/cmd.zig. Also a DUAL node.
///
/// The parent's handler is a `not_implemented` placeholder: the 8-section
/// packet is deferred with its four unported layer-2 dependencies. It is
/// still DECLARED here, positional included, so the catalog comparison sees
/// the same surface the oracle declares and so the bare verb fails at exit
/// 64 rather than silently rendering a help page. See
/// `planar.cmd.planar.handlers.resume`.
/// @param root The root app to attach the group to.
/// @brief The `spec ingest` leaf (plan 996, task 6365).
///
/// Hand-wired, like `declare_capture`'s `commits` (now in
/// handlers/capture.cpp), because CLI11's declarative
/// surface table has no variadic-positional primitive: `spec ingest <p1>
/// <p2> <p3>` batches every argument (`zig`'s `rest_field = "extra_plans"`),
/// so `plan` is REQUIRED and a second, hidden `extra-plans` positional takes
/// `expected(0, -1)` to catch the rest. See spec_ingest.cppm's header.
auto add_spec(CLI::App& root) -> void {
  CLI::App* spec = root.add_subcommand("spec", "Commands for the planning pipeline spec surface.\n\n"
                                               "  'spec ingest' decomposes workbench planning documents into a\n"
                                               "  structured task graph in the database.\n"
                                               "  'spec draft' generates initial spec artifacts from a goal statement.");
  spec->require_subcommand(0);

  CLI::App* ingest = spec->add_subcommand("ingest", "Decompose workbench spec documents into the task graph.");
  add_bool(*ingest, "--apply");
  add_bool(*ingest, "--apply-removals");
  // The oracle declares the default in its catalog as well as applying it
  // at runtime; preserve both surfaces.
  ingest->add_option("--format")->default_val("text");
  add_string(*ingest, "--scope");
  add_bool(*ingest, "--strict");
  add_json(*ingest);
  ingest->add_option("plan")->required();
  // Hidden variadic "rest" positional -- see this function's header.
  ingest->add_option("extra-plans")->expected(0, -1)->group("");
}

auto add_resume(CLI::App& root) -> void {
  // The oracle's LONG description, not its one-line summary. Task 6065
  // made the catalog carry both — the summary out of band, the description
  // off the tree — and `resume` was the one hand-written node still
  // declaring the short form where the oracle declares the long one.
  CLI::App* resume = root.add_subcommand("resume", "Produce a structured 8-section resume packet for the specified\n"
                                                   "  task.\n\n"
                                                   "  The packet contains:\n"
                                                   "    1. Identity       — task id, plan id, title, scope\n"
                                                   "    2. State          — status, next_action, last action\n"
                                                   "    3. Plan position  — parent plan, completed/current/remaining steps\n"
                                                   "    4. Operational    — external_links for the task; refreshed if stale\n"
                                                   "    5. Recent activity — session entries from recent sessions\n"
                                                   "    6. Decisions and questions\n"
                                                   "    7. Linked artifacts\n"
                                                   "    8. Audit footer   — previous session vendor and timestamp, plus\n"
                                                   "                        the active claim's worktree path (when held)\n"
                                                   "                        so the resumer can prepend `cd <path>`");
  resume->require_subcommand(0);
  add_json(*resume);

  resume->add_option("task-id");

  CLI::App* validate = resume->add_subcommand("validate", "Check if a task is resumable.");
  add_json(*validate);
  validate->add_option("task-id")->required();
}

} // namespace

auto root_app() -> std::unique_ptr<CLI::App> {
  auto app = std::make_unique<CLI::App>("Planning + agent operations CLI.", "planar");
  // A bare `planar` must render root help rather than fail.
  app->require_subcommand(0);

  // FOLDED PER-DOMAIN DECLARATIONS (M11.3, decision 1068). Each of these
  // lives next to its handlers under `handlers/`; `surface.cpp` no longer
  // carries a `node_spec` for any node beneath them.
  //
  // ORDER. While `surface.cpp` still holds entries, `apply_surface`'s
  // `reorder_children` pass below rewrites the root's child order from the
  // spec list, so these calls' position is not yet what the catalog shows --
  // the twenty-two retained ORDERING ANCHORS in `surface.cpp` are (see that
  // file). When the LAST wave deletes `surface.cpp` and with it the
  // `apply_surface` call, this call order becomes authoritative. The block
  // below is ALREADY in catalog order among itself (scope 2nd, assoc 3rd,
  // plan 4th, task 5th, question 6th, scenario 7th, decision 8th,
  // artifact 9th, annotate 10th, workbench 13th, workspace 14th, links
  // 17th, handoff 19th, capture 20th, audit 21st, models 23rd, config
  // 27th, templates 28th, local 31st, bench 39th, run 41st, feedback
  // 45th), so the last wave interleaves the remaining verbs into it
  // rather than resequencing it.
  //
  // WHAT EACH WAVE FOUND WHEN IT FOLDED, because the shape differs and the
  // difference is the interesting part:
  //
  //   M11.3a  plan, task -- nothing was shadowed.
  //   M11.3b  models, annotate, workbench. RETIRED `add_annotate`,
  //           `add_workbench` and the `add_filter_mode` helper the
  //           workbench group shared with nothing else. Twelve nodes were
  //           SHADOWED -- hand-declared here AND separately described by a
  //           `node_spec` that `apply_surface`'s find-or-create arm
  //           skipped -- and the two halves agreed field for field.
  //   M11.3c  question, scenario, decision, artifact. The opposite shape:
  //           NONE of their 44 nodes was hand-declared here. Their
  //           `edit`/`view`/`diff`/`review` leaves are handled in
  //           `handlers/drafting.cpp` but are declared with their own
  //           domain, since a group's sibling order is only correct as one
  //           contiguous list.
  //   M11.3d  assoc, workspace, handoff, capture, templates, bench.
  //           RETIRED `add_workspace`, `add_capture` and `add_handoff`.
  //           The most shadowed wave: 19 of its 43 nodes were declared
  //           here as well as in `surface.cpp` (all of `capture`, all of
  //           `handoff`, and four of `workspace`'s seven). Every field the
  //           two halves both described agreed. The ONE thing they
  //           disagreed on was ordering, in two places, and both are
  //           recorded where they now live: `workspace` had three children
  //           only the generated half named, and `handoff`'s children
  //           receive their three parent flags BEFORE their own here where
  //           `apply_surface` would append them after. The hand order is
  //           what ships, and the fold keeps it.
  //   M11.3e  scope, audit, config, local, links, run, feedback. Wave 3's
  //           shape again: NONE of its 39 nodes was hand-declared here,
  //           verified by re-reading every `add_subcommand` call site
  //           under `src/cmd/planar/` rather than assuming. `run` moved
  //           into `handlers/runs.cpp` beside the `bench` group M11.3d
  //           folded there, as its OWN function -- the two share an engine
  //           but are separate top-level verbs at separate catalog
  //           positions.
  handlers::declare_scope(*app);
  handlers::declare_assoc(*app);
  handlers::declare_plan(*app);
  handlers::declare_task(*app);
  handlers::declare_question(*app);
  handlers::declare_scenario(*app);
  handlers::declare_decision(*app);
  handlers::declare_artifact(*app);
  handlers::declare_annotate(*app);
  handlers::declare_workbench(*app);
  handlers::declare_workspace(*app);
  handlers::declare_links(*app);
  handlers::declare_handoff(*app);
  handlers::declare_capture(*app);
  handlers::declare_audit(*app);
  handlers::declare_models(*app);
  handlers::declare_config(*app);
  handlers::declare_templates(*app);
  handlers::declare_local(*app);
  handlers::declare_bench(*app);
  handlers::declare_run(*app);
  handlers::declare_feedback(*app);

  app->add_subcommand("version", "Print the planar version, commit, and C++ toolchain.");
  add_workflow(*app);
  add_unlink(*app);
  add_skills(*app);
  add_resume(*app);
  add_spec(*app);
  // Everything above is hand-transcribed and lands WITH its handler. This
  // fills in the rest of the oracle's surface — ~190 leaves that land no
  // behaviour — from generated data, skipping every node declared above.
  // See `planar.cmd.planar.surface`'s header for why the two halves are
  // written differently, and `planar.cliapp.surface`'s for what a declared
  // node without a handler does (exit 64, never a silent 0).
  (void)cliapp::apply_surface(*app, surface_nodes());
  // Help renders the same page it rendered before every bool flag gained
  // its `--no-X` negation — see `planar.cliapp.surface::hide_negations_in_help`.
  // Must come AFTER the whole tree exists.
  cliapp::hide_negations_in_help(*app);
  return app;
}

} // namespace planar::cmd
