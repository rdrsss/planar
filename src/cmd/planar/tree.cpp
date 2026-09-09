/// @file tree.cpp
/// @brief Implementation of `planar.cmd.planar.tree`.

module planar.cmd.planar.tree;

import std;
import cli11;
import planar.cliapp.surface;
import planar.cmd.planar.declare;
import planar.cmd.planar.surface;
import planar.cmd.planar.handlers.annotate;
import planar.cmd.planar.handlers.models;
import planar.cmd.planar.handlers.plan;
import planar.cmd.planar.handlers.task;
import planar.cmd.planar.handlers.workbench;

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

/// @brief The `workspace` group — transcribed from
/// zig/src/cmd/planar/handlers/workspace/cmd.zig. TWO of its four
/// subcommands are ported (`doctor` and `routing show`), so this group's
/// own help page lists two commands where the oracle lists four, and the
/// nested `routing` group lists one child where the oracle lists two.
///
/// `routing` is registered as a GROUP even though only one of its two
/// children is ported, which is the same rule the rest of the tree follows:
/// omit the unported CHILD (`build`), not the group that would otherwise
/// have nowhere to hang `show`.
/// @param root The root app to attach the group to.
auto add_workspace(CLI::App& root) -> void {
  CLI::App* workspace =
      root.add_subcommand("workspace", "Workspace administration.\n\n"
                                       "  A workspace is identified by an associations row of kind=org. Each\n"
                                       "  workspace owns a state directory under\n"
                                       "  ${PLANAR_HOME:-~/.planar}/workspaces/<org_id>/ holding the canonical\n"
                                       "  AGENTS.md surface for the org.");
  workspace->require_subcommand(0);
  CLI::App* doctor = workspace->add_subcommand("doctor", "Scan and fix workspace registration and state consistency.");
  add_json(*doctor);

  CLI::App* routing = workspace->add_subcommand("routing", "Manage workspace routing table.");
  routing->require_subcommand(0);
  CLI::App* show = routing->add_subcommand("show", "Display current routing table.");
  // The OPTIONAL `[workspace]` selector every workspace leaf takes: an id, a
  // slug, either with an `org:` prefix, or absent for "the only one".
  show->add_option("workspace");
  add_json(*show);
}

/// @brief The `capture` group — transcribed from
/// zig/src/cmd/planar/handlers/capture/cmd.zig. All seven leaves are
/// declared as of task 6358, which added `commits`.
///
/// `end`'s `<session-id>` positional is declared as a STRING even though
/// it names an integer. That is deliberate and load-bearing: the oracle
/// parses it in the handler and answers `session id must be an integer,
/// got 'x'` with exit 2, and a `zig_int_validator()` here would answer
/// CLI11's `ValidationError` wording instead.
///
/// `commits`'s trailing SHA list is declared `->group("")`-HIDDEN, not as
/// an ordinary visible positional. That is not a style choice — see
/// `parsed_args::positional_lists`'s header for the full reasoning, but
/// the load-bearing fact is: the oracle's own `rest_field` mechanism
/// (etcli-zig) is NOT a real positional and never appears in the oracle's
/// `schema` catalog (confirmed against a live oracle run: `capture
/// commits` reports `"positionals":[]`). Declaring this as a VISIBLE
/// positional would add a catalog entry `catalog_parity.hpp` cannot find
/// in the oracle and fail the parity gate. `group("")` hides it from
/// `schema`/`--help`/completion the same way CLI11 hides `--help` itself
/// (see `walk.cppm`'s `visible`), while `harvest()` still collects it —
/// visibility is a rendering concern, not a parsing one.
/// @param root The root app to attach the group to.
auto add_capture(CLI::App& root) -> void {
  CLI::App* capture = root.add_subcommand("capture", "Capture commands manage explicit session management and context\n"
                                                     "  capture.\n\n"
                                                     "  Automatic capture happens on every write command; use these\n"
                                                     "  subcommands for explicit session management, narrative notes,\n"
                                                     "  command history, and snapshots.");
  capture->require_subcommand(0);

  CLI::App* session = capture->add_subcommand("session", "Open or reuse a session for the current (vendor, "
                                                         "vendor-session-id) tuple.");
  add_string(*session, "--vendor");
  add_string(*session, "--vendor-session-id");
  add_string(*session, "--model");
  add_int(*session, "--task");
  add_json(*session);

  CLI::App* commits = capture->add_subcommand("commits", "Record explicit git commits into a session.");
  add_int(*commits, "--session");
  add_string(*commits, "--repo");
  add_string(*commits, "--since");
  add_json(*commits);
  // Hidden variadic "rest" positional -- see this function's header.
  commits->add_option("shas")->expected(0, -1)->group("");

  CLI::App* end = capture->add_subcommand("end", "End the active or specified session.");
  add_int(*end, "--session");
  add_string(*end, "--summary");
  end->add_option("session-id");
  add_json(*end);

  CLI::App* note = capture->add_subcommand("note", "Append a narrative note to the active session.");
  add_int(*note, "--session");
  note->add_option("body")->required();
  add_json(*note);

  CLI::App* command = capture->add_subcommand("command", "Append a command to the active session.");
  add_int(*command, "--session");
  add_string(*command, "--outcome");
  command->add_option("command")->required();
  add_json(*command);

  CLI::App* file = capture->add_subcommand("file", "Attach a file to the active session.");
  add_int(*file, "--session");
  add_string(*file, "--role");
  file->add_option("path")->required();
  add_json(*file);

  CLI::App* snapshot = capture->add_subcommand("snapshot", "Create a context snapshot.");
  add_int(*snapshot, "--session");
  add_int(*snapshot, "--task");
  add_string(*snapshot, "--note");
  add_string(*snapshot, "--next-action");
  snapshot->add_option("body");
  add_json(*snapshot);
}

/// @brief The `handoff` group — transcribed from
/// zig/src/cmd/planar/handlers/handoff/cmd.zig.
///
/// A DUAL node: it has six subcommands AND its own `<task-id>` positional
/// and handler. `require_subcommand(0)` allows the bare form, and
/// `planar.cmd.planar.dispatch` routes it to the parent's handler because
/// the table has an entry for `"handoff"` — see that module's header.
///
/// ## Inherited flags: redeclared on every child
///
/// `handoff` is the FIRST group in this tree whose PARENT carries flags,
/// so it is the first to meet a CLI11/etcli difference that `workflow`,
/// `annotate`, `workspace` and `workbench` never could. etcli inherits a
/// parent's flags into every child both in the `schema` catalog AND at
/// parse time; CLI11 does neither.
///
/// Two mechanisms were tried and only one survives:
///
///   `fallthrough()`   Lets an unknown option on the child fall back to the
///                     parent, which fixes PARSING — but CLI11 then also
///                     exposes the parent's options ON the child, so the
///                     catalog gained duplicate flags AND the parent's
///                     `task-id` POSITIONAL appeared on every child. Worse,
///                     on `resume validate` — whose own positional is also
///                     named `task-id` — CLI11 threw `OptionAlreadyAdded`
///                     while the tree was still being BUILT, aborting every
///                     invocation of the binary, `planar version` included.
///   redeclaration     Declaring the parent's flags on each child fixes
///                     parsing with no positional bleed. It DOES make the
///                     flag appear twice in the catalog — once inherited,
///                     once local — and that duplication is a
///                     `planar.cliapp.schema` bug, now fixed there (see its
///                     `render_flags`) rather than worked around here.
///
/// So: declared on the parent AND on every child. Any future group with
/// parent-level flags needs the same.
/// @param root The root app to attach the group to.
auto add_handoff(CLI::App& root) -> void {
  CLI::App* handoff =
      root.add_subcommand("handoff", "Capture a context snapshot for the current session and atomically:\n"
                                     "    1. Insert a context_snapshots row.\n"
                                     "    2. Insert a handoffs row with status='pending'.\n"
                                     "    3. Validate the handoff (pending \xe2\x86\x92 validated, validated_at set).\n\n"
                                     "  Subcommands manage the handoff lifecycle: create / validate /\n"
                                     "  consume / abandon / list / show.");
  handoff->require_subcommand(0);
  add_string(*handoff, "--vendor");
  add_string(*handoff, "--note");
  add_json(*handoff);
  handoff->add_option("task-id");

  // The two parent flags every child redeclares, plus --json. See above.
  auto inherited = [](CLI::App& app) {
    add_string(app, "--vendor");
    add_string(app, "--note");
    add_json(app);
  };

  CLI::App* create = handoff->add_subcommand("create", "Create a handoff from an existing snapshot.");
  inherited(*create);
  create->add_option("snapshot-id")->required();

  CLI::App* validate = handoff->add_subcommand("validate", "Validate a pending handoff.");
  inherited(*validate);
  validate->add_option("handoff-id")->required();

  CLI::App* consume = handoff->add_subcommand("consume", "Mark a handoff as consumed.");
  inherited(*consume);
  add_int(*consume, "--session");
  consume->add_option("handoff-id")->required();

  CLI::App* abandon = handoff->add_subcommand("abandon", "Abandon a non-terminal handoff.");
  inherited(*abandon);
  add_string(*abandon, "--reason");
  abandon->add_option("handoff-id")->required();

  CLI::App* list = handoff->add_subcommand("list", "List handoffs.");
  inherited(*list);
  add_string(*list, "--status");

  CLI::App* show = handoff->add_subcommand("show", "Show a handoff's details.");
  inherited(*show);
  show->add_option("handoff-id")->required();
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
/// Hand-wired, like `add_capture`'s `commits`, because CLI11's declarative
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
  // the five retained ORDERING ANCHORS in `surface.cpp` are (see that
  // file). When the LAST wave deletes `surface.cpp` and with it the
  // `apply_surface` call, this call order becomes authoritative and must
  // then read in catalog order: init, scope, assoc, plan, task, question,
  // ... . `plan` and `task` are placed first because every verb already
  // hand-written below follows them in that order; M11.3b's three domains
  // follow them in THEIR catalog order (annotate 10th, workbench 13th,
  // models 25th), so the block already reads in the sequence the last wave
  // will need.
  //
  // M11.3b also RETIRED `add_annotate`, `add_workbench` and the
  // `add_filter_mode` helper the workbench group shared with nothing else.
  // Those ten workbench children and two annotate children were SHADOWED --
  // hand-declared here and separately described by a `node_spec` that
  // `apply_surface`'s find-or-create arm skipped. The hand and generated
  // declarations were compared field by field before the fold and agreed
  // exactly on all twelve, so nothing was decided by which one won; the
  // folded declaration carries one merged list, in catalog order.
  handlers::declare_plan(*app);
  handlers::declare_task(*app);
  handlers::declare_annotate(*app);
  handlers::declare_workbench(*app);
  handlers::declare_models(*app);

  app->add_subcommand("version", "Print the planar version, commit, and C++ toolchain.");
  add_workflow(*app);
  add_unlink(*app);
  add_skills(*app);
  add_workspace(*app);
  add_capture(*app);
  add_handoff(*app);
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
