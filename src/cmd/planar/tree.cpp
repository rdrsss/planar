/// @file tree.cpp
/// @brief Implementation of `planar.cmd.planar.tree`.

module planar.cmd.planar.tree;

import std;
import cli11;
import planar.cliapp.surface;
import planar.cmd.planar.declare;
import planar.cmd.planar.handlers.annotate;
import planar.cmd.planar.handlers.artifact;
import planar.cmd.planar.handlers.assoc;
import planar.cmd.planar.handlers.audit;
import planar.cmd.planar.handlers.capture;
import planar.cmd.planar.handlers.catalog;
import planar.cmd.planar.handlers.closure;
import planar.cmd.planar.handlers.config;
import planar.cmd.planar.handlers.dashboard;
import planar.cmd.planar.handlers.decision;
import planar.cmd.planar.handlers.feedback;
import planar.cmd.planar.handlers.groups;
import planar.cmd.planar.handlers.handoff;
import planar.cmd.planar.handlers.health;
import planar.cmd.planar.handlers.importer;
import planar.cmd.planar.handlers.init;
import planar.cmd.planar.handlers.link;
import planar.cmd.planar.handlers.links;
import planar.cmd.planar.handlers.local;
import planar.cmd.planar.handlers.models;
import planar.cmd.planar.handlers.plan;
import planar.cmd.planar.handlers.promotion;
import planar.cmd.planar.handlers.question;
import planar.cmd.planar.handlers.report;
import planar.cmd.planar.handlers.resume;
import planar.cmd.planar.handlers.runs;
import planar.cmd.planar.handlers.scenario;
import planar.cmd.planar.handlers.scope;
import planar.cmd.planar.handlers.search;
import planar.cmd.planar.handlers.skills;
import planar.cmd.planar.handlers.spec_ingest;
import planar.cmd.planar.handlers.synthesize;
import planar.cmd.planar.handlers.task;
import planar.cmd.planar.handlers.templates;
import planar.cmd.planar.handlers.test_spec;
import planar.cmd.planar.handlers.tree;
import planar.cmd.planar.handlers.unlink;
import planar.cmd.planar.handlers.version;
import planar.cmd.planar.handlers.workbench;
import planar.cmd.planar.handlers.workflow;
import planar.cmd.planar.handlers.workspace;

namespace planar::cmd {

namespace {

/// @brief Declare the `explore` leaf — the ONE verb in this binary with
/// no handler module to sit beside.
///
/// `explore` is the cockpit alias, and the cockpit is not ported
/// (decision 1003). It is the sole surviving entry in `unported_paths()`,
/// and its refusal is registered by `planar.cmd.planar.dispatch`'s
/// `explore_fallback` — a function that takes the BUILT node, so it
/// cannot host the declaration without inverting the dependency. Every
/// other M11.3 fold moved a declaration next to its handler; this one has
/// nowhere to move to, so it stays here rather than being given a handler
/// module that would hold a declaration and nothing else.
/// @param root The root app to attach the leaf to.
auto declare_explore(CLI::App& root) -> void {
  CLI::App* explore = root.add_subcommand(
      "explore", "Launch the interactive Planar cockpit.\n\n  Equivalent to invoking `planar` with no verb on a terminal. Use\n  "
                 "`planar explore` when you want to force-launch the cockpit by name,\n  or from a context where bare-invocation "
                 "detection may not fire.\n\n  --plan, --task, and --scope seed the initial focus.\n\n  Falls back to this help "
                 "text when stdout is not a TTY, when TERM=dumb,\n  when PLANAR_NO_TUI is set, or when --plain is passed.");
  add_string(*explore, "--plan", "Seed initial focus on this plan ID");
  add_string(*explore, "--task", "Seed initial focus on this task ID");
  add_string(*explore, "--scope", "Seed scope filter");
  add_bool(*explore, "--plain", "Fall back to help/usage instead of launching the cockpit");
}

} // namespace

auto root_app() -> std::unique_ptr<CLI::App> {
  auto app = std::make_unique<CLI::App>("Planning + agent operations CLI.", "planar");
  // A bare `planar` must render root help rather than fail.
  app->require_subcommand(0);

  // THE ROOT'S CHILD ORDER, AND IT IS NOW AUTHORITATIVE (M11.3f, task
  // 6636; the risk was filed ahead as task 6642).
  //
  // Until this wave a `cliapp::apply_surface(*app, surface_nodes())` call
  // sat below this block and rewrote the root's child order on a second
  // pass, from `surface.cpp`'s spec list. That pass is gone with the file,
  // so THE CALL ORDER BELOW is what `planar --help` and the `schema`
  // catalog report -- for all forty-five verbs at once, the ones
  // hand-written here long before M11 included, not only the ones this
  // wave folded.
  //
  // The trailing numbers are each verb's catalog position, derived from
  // the cycle-base binary's own `planar schema` before any edit and
  // re-verified against it after. They are not decorative: a call moved
  // out of sequence is a change to the pinned surface, and
  // `scripts/surface-snapshot.sh verify` fails on it (break-probed at this
  // task by swapping two calls -- the gate reported the moved verbs in
  // both the schema digest and the root help hash).
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
  //           here as well as in `surface.cpp`. Every field the two halves
  //           both described agreed. The ONE thing they disagreed on was
  //           ordering, in two places, and both are recorded where they
  //           now live: `workspace` had three children only the generated
  //           half named, and `handoff`'s children receive their three
  //           parent flags BEFORE their own where `apply_surface` would
  //           append them after. The hand order is what ships.
  //   M11.3e  scope, audit, config, local, links, run, feedback. Wave 3's
  //           shape again: NONE of its 39 nodes was hand-declared here.
  //           `run` moved into `handlers/runs.cpp` beside the `bench`
  //           group M11.3d folded there, as its OWN function -- the two
  //           share an engine but are separate top-level verbs at separate
  //           catalog positions.
  //   M11.3f  the thirty-three remaining singletons, and the end of the
  //           generated table. RETIRED `add_workflow`, `add_unlink`,
  //           `add_skills`, `add_spec` and `add_resume`, and with them the
  //           last hand-written declaration that did not sit beside its
  //           handler -- `explore` excepted, which has no handler module to
  //           sit beside (see `declare_explore` above). Six domains were
  //           shadowed: `unlink`, `skills`, `spec`, `resume`, `version`,
  //           and `workflow`'s `list`/`show` but not its `run` -- the
  //           surface's only PARTIAL fold, closed by folding the group
  //           whole. ONE real disagreement, recorded in `declare_resume`:
  //           the generated half gave `resume validate` no flags where the
  //           hand half declares `--json`, and the hand half is what
  //           shipped.
  handlers::declare_init(*app);       // 1
  handlers::declare_scope(*app);      // 2
  handlers::declare_assoc(*app);      // 3
  handlers::declare_plan(*app);       // 4
  handlers::declare_task(*app);       // 5
  handlers::declare_question(*app);   // 6
  handlers::declare_scenario(*app);   // 7
  handlers::declare_decision(*app);   // 8
  handlers::declare_artifact(*app);   // 9
  handlers::declare_annotate(*app);   // 10
  handlers::declare_promote(*app);    // 11
  handlers::declare_demote(*app);     // 12
  handlers::declare_workbench(*app);  // 13
  handlers::declare_workspace(*app);  // 14
  handlers::declare_link(*app);       // 15
  handlers::declare_unlink(*app);     // 16
  handlers::declare_links(*app);      // 17
  handlers::declare_resume(*app);     // 18
  handlers::declare_handoff(*app);    // 19
  handlers::declare_capture(*app);    // 20
  handlers::declare_audit(*app);      // 21
  handlers::declare_health(*app);     // 22
  handlers::declare_models(*app);     // 23
  handlers::declare_dashboard(*app);  // 24
  handlers::declare_spec(*app);       // 25
  handlers::declare_test_spec(*app);  // 26
  handlers::declare_config(*app);     // 27
  handlers::declare_templates(*app);  // 28
  handlers::declare_tree(*app);       // 29
  handlers::declare_search(*app);     // 30
  handlers::declare_local(*app);      // 31
  handlers::declare_skills(*app);     // 32
  handlers::declare_import(*app);     // 33
  handlers::declare_synthesize(*app); // 34
  handlers::declare_version(*app);    // 35
  handlers::declare_completion(*app); // 36
  handlers::declare_schema(*app);     // 37
  handlers::declare_report(*app);     // 38
  handlers::declare_bench(*app);      // 39
  handlers::declare_closure(*app);    // 40
  handlers::declare_run(*app);        // 41
  handlers::declare_groups(*app);     // 42
  declare_explore(*app);              // 43
  handlers::declare_workflow(*app);   // 44
  handlers::declare_feedback(*app);   // 45

  // Help renders the same page it rendered before every bool flag gained
  // its `--no-X` negation — see `planar.cliapp.surface::hide_negations_in_help`.
  // Must come AFTER the whole tree exists.
  cliapp::hide_negations_in_help(*app);
  return app;
}

} // namespace planar::cmd
