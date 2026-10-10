/// @file surface.cppm
/// @brief `planar.cmd.planar.surface` — the two out-of-band catalog tables
/// the `planar` binary needs, plus its unported-leaf inventory.
///
/// ## What used to be here, and where it went
///
/// This module used to also export `surface_nodes()`: a generated
/// `node_spec` table describing every command path the Zig oracle
/// declared, which `tree.cpp` applied over its own hand-written tree via
/// `planar.cliapp.surface::apply_surface`. `scripts/gen-cli-surface.py`
/// produced it from `zig/zig-out/bin/planar schema`; the oracle was
/// deleted at the M10 cutover (task 6045), so the script could no longer
/// run and the table became hand-maintained ground truth at task 6267.
///
/// Plan 1051 M11.3 folded it away over six waves (decision 1068), each
/// moving a set of top-level domains into hand-written declarations next
/// to their handlers under `handlers/`: `plan`/`task` (6631), then
/// `models`/`annotate`/`workbench` (6632), then the drafting quartet
/// `question`/`scenario`/`decision`/`artifact` (6633), then
/// `assoc`/`workspace`/`handoff`/`capture`/`templates`/`bench` (6634),
/// then `scope`/`audit`/`config`/`local`/`links`/`run`/`feedback` (6635),
/// and finally the thirty-three remaining singletons (6636). That last
/// wave deleted `surface.cpp` and the `apply_surface` call with it, which
/// is why `main.cppm`'s call order is now the root's authoritative child
/// order.
///
/// ## One deliberate asymmetry the fold leaves standing (task 6667)
///
/// `handlers/handoff.cppm`'s `declare_handoff` (wave 6634) and
/// `handlers/health.cpp`'s `declare_health` (wave 6635) encode OPPOSITE
/// orders for where a group's inherited flags land relative to a child's
/// own: `handoff` puts the parent's flags first, `health` appends them
/// after `hygiene`'s own. Both are correct — each faithfully reproduces
/// what shipped before the fold, from a different generated-tree
/// precedent (`apply_surface`'s ancestor-flags pass fired only on
/// `health`) — and both sites carry a header that cross-references the
/// other and says neither order is a convention. Recorded again here, at
/// the fold's own retrospective, because the two are pinned only by a
/// per-leaf `--help` hash invisible in the `schema` catalog
/// (`render_flags` dedupes to inherited-first regardless), and that kind
/// of silent asymmetry is exactly what a future author "harmonizes" once
/// its two call sites stop being read together.
///
/// The functions below are defined directly in this interface unit rather
/// than in a separate `surface.cpp` — the same single-file-module shape
/// `planar.cmd.planar_agent.surface` and `planar.cmd.planar_watch.surface`
/// already use, and for the same reason: there is no longer enough
/// implementation here to earn a split translation unit.
///
/// ## THE `ext` AND `sync` FAMILIES, and why they are absent
///
/// The deleted `surface.cpp` carried a dozen `// k_path_NNN … removed at
/// plan 996, task 6419` provenance comments, and their referents left with
/// the file. They were the only record of why those verbs appear on no
/// binary's surface, so the substance is kept here:
///
///   * The whole `ext` group (`register` and its `jira`/`github` leaves,
///     `list`, `test`, `create`, `propagate-one`) and the whole `sync`
///     group (`pull`, `push`, `status`, `resolve`) MOVED to `planar-ext`
///     at task 6419 — see `src/cmd/planar-ext/handlers/ext/command.cppm` and
///     `handlers/sync/command.cppm`. They are not missing; they are on the other binary.
///   * `ext propagate`, the family's one remaining UNPORTED leaf, moved
///     with them conceptually but is declared on NEITHER binary yet. It
///     stays blocked on the surfaces `unported_paths`'s own comment always
///     named (the feature-tree walk, and the create/propagate half of
///     `engine_extsync`), and lands on `planar-ext` once a later step of
///     task 6412's extraction closes that gap. It is deliberately absent
///     from `unported_paths()` below for that reason: that inventory is
///     DECLARED-but-unimplemented, and `ext propagate` is not declared
///     anywhere.
///   * `ext register jira`/`github`'s auth-resolution FACTORY stayed on
///     this binary — `audit publish-decision` and `workbench publish` are
///     its other two callers and neither moved, and D18 forbids a
///     `cmd_planar -> cmd_planar_ext` edge — so
///     `src/cmd/planar-ext/handlers/shared/ext_adapter_factory.cppm` is a
///     deliberate duplicate, not a shared import. Same shape for the
///     cross-scope guard, which `feedback triage set` still needs:
///     `src/cmd/planar-ext/scope.cppm` duplicates it.
///
/// ## What a declared-but-unported verb DOES
///
/// It refuses, loudly, at exit 64. `unported_paths()` is the explicit
/// inventory, and `planar.cmd.planar.dispatch` registers a
/// `not_implemented` handler for every entry. Two distinct hazards make
/// that registration load-bearing rather than decorative:
///
///   * a LEAF left out of the table would still exit 64 via `run`'s
///     table-miss arm — but it would also trip the "every leaf in the tree
///     has a handler" gate, which is the thing that keeps the inventory
///     honest as verbs get ported;
///   * a DUAL node (subcommands AND its own handler in the oracle) left
///     out of the table falls to the HELP path and exits 0 — a silent
///     success. Measured by invoking every one of the oracle's 38 group
///     nodes against a scratch arena: exactly three are dual — `resume`
///     (exit 1 here, scope error), `handoff` (exit 2, no active session)
///     and `health` (exit 0 WITH a report). Every other group renders help
///     at exit 0, which is what an unregistered group does, so those are
///     correct unregistered.
module;

export module planar.cmd.planar.surface;

import std;

namespace planar::cmd {

/// @brief The flags whose declared default is the EMPTY STRING.
///
/// CLI11 cannot tell "no default" from "a default that is the empty
/// string" — `Option::get_default_str()` answers `""` to both — so the
/// four flags the oracle declares that way are supplied as data, exactly
/// like the summaries below. See `planar.cliapp.schema`'s three-argument
/// `schema_json` for the encodings that were rejected first (task 6130).
/// @return `(full command path, flag long name)` pairs.
export auto surface_empty_string_defaults() -> std::span<std::pair<std::string_view, std::string_view> const> {
  // Measured, not guessed: these are the ONLY flags across all three
  // oracle catalogs whose `"default"` is `""` rather than `null` or a
  // typed literal. Verified by diffing this binary's `schema` output
  // against `zig/zig-out/bin/planar schema` byte for byte — before task
  // 6130 those four values were the entire 8-byte difference.
  static constexpr std::pair<std::string_view, std::string_view> k_empty_defaults[] = {
      {"planar workbench edit", "--editor"},
      {"planar workflow run", "--args"},
      {"planar workflow run", "--worktree"},
      {"planar workflow run", "--sandbox-root"},
  };
  return k_empty_defaults;
}

/// @brief The oracle's one-line `summary` for each command path.
///
/// `CLI::App` holds a single description string, so the short summary has
/// nowhere to live on the tree and is handed to
/// `planar.cliapp.schema::schema_json` as data instead. 57 `planar` nodes
/// have a summary that differs from their long description, 28 of them
/// leaves — which is why, unlike `planar-agent` (task 6614) and
/// `planar-watch` (task 6613), this table SURVIVES the M11 fold and
/// `handlers/catalog.cpp` keeps calling the multi-argument `schema_json`
/// overload. Deleting it would move the pinned catalog and violate
/// decision 1068.
/// @return `(full command path, summary)` pairs.
export auto surface_summaries() -> std::span<std::pair<std::string_view, std::string_view> const> {
  static constexpr std::pair<std::string_view, std::string_view> k_summaries[] = {
      {"planar", "Planning + agent operations CLI."},
      {"planar init", "Initialize the Planar database and register the current directory as a project."},
      {"planar scope", "Inspect the cwd-derived scope and suggest memberships."},
      {"planar scope show", "Show the cwd-derived scope (and any --scope override)."},
      {"planar scope suggest", "Suggest scope associations based on cwd."},
      {"planar scope use", "Removed in plan 153 M5 — see `planar scope show`."},
      {"planar scope pop", "Removed in plan 153 M5 — see `planar scope show`."},
      {"planar scope clear", "Removed in plan 153 M5 — see `planar scope show`."},
      {"planar assoc", "Manage associations (many-to-many scope tags for repos)."},
      {"planar assoc list", "List all known associations."},
      {"planar assoc create", "Create a new association."},
      {"planar assoc add", "Add a repo to an association."},
      {"planar assoc remove", "Remove a repo from an association."},
      {"planar assoc members", "List all project members of an association."},
      {"planar assoc detect", "Propose (or apply) auto-detected associations for the current directory."},
      {"planar plan", "Manage plans and plan steps."},
      {"planar plan create", "Create a new plan."},
      {"planar plan show", "Show a plan's details, steps, and child plans."},
      {"planar plan list", "List plans."},
      {"planar plan update", "Update mutable fields on a plan."},
      {"planar plan edit", "Edit a plan in $EDITOR (editor-first flow)."},
      {"planar plan view", "View a plan's workbench file."},
      {"planar plan diff", "Diff plan against database version."},
      {"planar plan review", "Reviewer entry point for plan diff."},
      {"planar plan link", "Create an entity link from a plan to another entity."},
      {"planar plan next", "Bucketed claim-aware view of next work on a plan (available / claimed / stale / blocked)."},
      {"planar plan recommend-strategy", "Recommend an execution strategy: compute the parallel-eligible subset of a plan's open "
                                         "tasks via the six parallelizability rules."},
      {"planar plan divergence", "Report the declared-vs-derived closure divergence for a plan's open tasks (decision D4)."},
      {"planar plan recompute-status", "Recompute a plan's roll-up status (--plan <id> or --all)."},
      {"planar plan closeout",
       "Delivery-evidence gate: report whether a plan is ready to close and (without --dry-run) mark it done."},
      {"planar plan step", "Manage plan steps."},
      {"planar plan step add", "Append a new step to a plan."},
      {"planar plan step list", "List steps of a plan."},
      {"planar plan step done", "Mark a plan step as done."},
      {"planar plan step skip", "Mark a plan step as skipped."},
      {"planar plan step link", "Associate a plan step with the task that materializes it."},
      {"planar plan descendants", "Emit the anchor plan's full subtree in dependency-topological order. READ-ONLY."},
      {"planar task", "Manage tasks."},
      {"planar task add", "Create a new task."},
      {"planar task show", "Show full task details."},
      {"planar task packet", "Compile the authoritative current routing packet for a task."},
      {"planar task list", "List tasks."},
      {"planar task update", "Update mutable fields on a task."},
      {"planar task edit", "Edit a task in $EDITOR (editor-first flow)."},
      {"planar task view", "View task's workbench file."},
      {"planar task diff", "Diff task against its database-stored version."},
      {"planar task review", "Reviewer entry point for task diff."},
      {"planar task done", "Mark a task as done."},
      {"planar task cancel", "Cancel a task."},
      {"planar task block", "Mark a task as blocked and record the blocking relationship."},
      {"planar task link", "Create an entity link from a task to another entity."},
      {"planar task reopen", "Reopen a done or cancelled task with an audit-trail entry."},
      {"planar task touches", "Manage repo-touches links on a task."},
      {"planar task facts stage", "Stage this task's routing facts under operator provenance."},
      {"planar task touches add", "Link a task to a repo via a 'touches' relationship."},
      {"planar task touches infer", "Propose path-level touches from the task's own text (preview by default)."},
      {"planar task touches list", "List the repo- and path-level touches declared on a task."},
      {"planar task touches remove", "Remove a 'touches' link between a task and a repo."},
      {"planar question", "Manage questions."},
      {"planar question add", "Create a new question."},
      {"planar question edit", "Edit a question in $EDITOR (editor-first flow)."},
      {"planar question view", "View question's workbench file."},
      {"planar question diff", "Diff question against database version."},
      {"planar question review", "Reviewer entry point for question diff."},
      {"planar question answer", "Record an answer to a question."},
      {"planar question wontfix", "Mark a question as wontfix."},
      {"planar question list", "List questions."},
      {"planar question show", "Show a question's details."},
      {"planar question link", "Create an entity link from a question to another entity."},
      {"planar scenario", "Manage test scenarios."},
      {"planar scenario add", "Create a new test scenario."},
      {"planar scenario edit", "Edit a scenario in $EDITOR (editor-first flow)."},
      {"planar scenario view", "View scenario's workbench file."},
      {"planar scenario diff", "Diff scenario against database version."},
      {"planar scenario review", "Reviewer entry point for scenario diff."},
      {"planar scenario verify", "Record a test run for a scenario (--outcome pass|fail|error|skipped; defaults to pass)."},
      {"planar scenario retire", "Mark a scenario as retired."},
      {"planar scenario list", "List scenarios."},
      {"planar scenario show", "Show a scenario's details."},
      {"planar scenario link", "Create an entity link from a scenario to another entity."},
      {"planar decision", "Manage decision records."},
      {"planar decision add", "Create a new decision record."},
      {"planar decision show", "Show a decision's details."},
      {"planar decision list", "List decisions."},
      {"planar decision accept", "Accept a proposed decision."},
      {"planar decision supersede", "Mark a decision as superseded by a newer decision."},
      {"planar decision withdraw", "Withdraw a decision."},
      {"planar decision edit", "Edit a decision in $EDITOR (editor-first flow)."},
      {"planar decision view", "View decision's workbench file."},
      {"planar decision diff", "Diff decision against database version."},
      {"planar decision review", "Reviewer entry point for decision diff."},
      {"planar decision link", "Create an entity link from a decision to another entity."},
      {"planar artifact", "Manage artifacts (tech specs, ADRs, design notes, etc.)."},
      {"planar artifact add", "Register a new artifact."},
      {"planar artifact show", "Show an artifact's metadata and body."},
      {"planar artifact list", "List artifacts."},
      {"planar artifact update", "Update mutable fields on an artifact."},
      {"planar artifact edit", "Edit an artifact in $EDITOR (editor-first flow)."},
      {"planar artifact view", "View the artifact's workbench file in $PAGER."},
      {"planar artifact diff", "Show a unified diff between the DB's artifact content and the workbench file."},
      {"planar artifact review", "Reviewer entry point for artifact diff."},
      {"planar artifact link", "Create an entity link from an artifact to another entity."},
      {"planar annotate", "Manage source annotations."},
      {"planar annotate add", "Create a new annotation."},
      {"planar annotate show", "Show an annotation."},
      {"planar annotate list", "List annotations."},
      {"planar annotate capabilities", "Describe annotation read and command support."},
      {"planar annotate update", "Update an annotation."},
      {"planar annotate remove", "Remove an annotation."},
      {"planar annotate tag", "Add or remove a tag on an annotation."},
      {"planar annotate resolve", "Mark an annotation as resolved."},
      {"planar annotate dismiss", "Dismiss an annotation."},
      {"planar annotate archive", "Archive an annotation."},
      {"planar annotate bulk-resolve", "Resolve every active annotation matching the filter."},
      {"planar annotate bulk-dismiss", "Dismiss every active annotation matching the filter."},
      {"planar annotate bulk-archive", "Archive every annotation matching the filter (including non-active rows)."},
      {"planar annotate verify", "Verify annotation anchors against workspace state."},
      {"planar annotate sweep", "Sweep stale annotations (resolved/dismissed older than --since-days)."},
      {"planar promote", "Promote an entity to an association scope."},
      {"planar demote", "Demote an entity back to global scope."},
      {"planar workbench", "Manage workbench sync for plan feature directories."},
      {"planar workbench lint", "Validate workbench Markdown frontmatter without syncing."},
      {"planar workbench pull", "Apply FS→DB changes; report DB→FS drift."},
      {"planar workbench push", "Apply DB→FS changes atomically; report FS→DB drift."},
      {"planar workbench status", "Show drift and conflicts without writing."},
      {"planar workbench resolve", "Settle a sync conflict by choosing FS or DB."},
      {"planar workbench sync", "Atomically apply FS and DB changes via a unified sync."},
      {"planar workbench archive", "Archive a feature's workbench filesystem tree."},
      {"planar workbench restore", "Restore an archived feature's workbench tree."},
      {"planar workbench gc", "Remove FS files whose backing entity is terminal in the DB."},
      {"planar workbench list", "List features with workbench trees."},
      {"planar workbench publish", "Render and push workbench files to external system."},
      {"planar workbench extract-questions", "Parse Open questions from top-level workbench specs (read-only)."},
      {"planar workbench edit", "Edit a feature's workbench files in $EDITOR."},
      {"planar workspace", "Manage workspace state directories and their AGENTS.md surfaces."},
      {"planar workspace init", "Initialize a workspace (org-level association)."},
      {"planar workspace doctor", "Scan and fix workspace registration and state consistency."},
      {"planar workspace routing", "Manage workspace routing table."},
      {"planar workspace routing build", "Build routing table from workspace membership."},
      {"planar workspace routing show", "Display current routing table."},
      {"planar workspace regenerate", "Regenerate AGENTS.md from current state."},
      {"planar ext", "Manage external operational-plane systems (Jira, GitHub Issues, etc.)."},
      {"planar ext register", "Register an external system."},
      {"planar ext register jira", "Register a Jira instance as an external system."},
      {"planar ext register github", "Register a GitHub Issues repository as an external system."},
      {"planar ext list", "List registered external systems."},
      {"planar ext test", "Test connection to an external system."},
      {"planar ext create", "Create an external counterpart for a local entity."},
      {"planar ext propagate-one", "Render + POST + record one entity counterpart; idempotent skip on existing link."},
      {"planar ext propagate", "Propagate a feature (plan + descendants) to an external system."},
      {"planar link", "Link a local entity to an external-system ticket."},
      {"planar unlink", "Remove an external_links row by link id."},
      {"planar links", "List or remove internal entity_links relationships."},
      {"planar links add", "Create an entity_links row between two entities."},
      {"planar links list", "List entity_links where the given entity is source or target."},
      {"planar links remove", "Delete an entity_links row by its id."},
      {"planar links trail", "Show the audit trail for an entity_links row."},
      {"planar sync", "Pull and push data between the local plane and external systems."},
      {"planar sync pull", "Pull remote state for one or more external links."},
      {"planar sync push", "Push local changes for one or more external links."},
      {"planar sync status", "Report sync status for links."},
      {"planar sync resolve", "Settle a sync conflict on a link."},
      {"planar resume", "Produce a structured resume packet for the specified task."},
      {"planar resume validate", "Check if a task is resumable."},
      {"planar handoff", "Capture a context snapshot and create a validated handoff record."},
      {"planar handoff create", "Create a handoff from an existing snapshot."},
      {"planar handoff validate", "Validate a pending handoff."},
      {"planar handoff consume", "Mark a handoff as consumed."},
      {"planar handoff abandon", "Abandon a non-terminal handoff."},
      {"planar handoff list", "List handoffs."},
      {"planar handoff show", "Show a handoff's details."},
      {"planar capture", "Manage explicit session capture."},
      {"planar capture session", "Open or reuse a session for the current (vendor, vendor-session-id) tuple."},
      {"planar capture commits", "Record explicit git commits into a session."},
      {"planar capture end", "End the active or specified session."},
      {"planar capture note", "Append a narrative note to the active session."},
      {"planar capture command", "Append a command to the active session."},
      {"planar capture file", "Attach a file to the active session."},
      {"planar capture snapshot", "Create a context snapshot."},
      {"planar audit", "Cross-plane audit trail commands."},
      {"planar audit trail",
       "Show audit history for an entity (audit_log + entity_links) or an external link (external_links + sync_events)."},
      {"planar audit commits", "List commits attributed to sessions and claims."},
      {"planar audit session", "Show the timeline for a session."},
      {"planar audit publish-decision", "Post the decision body to linked operational-plane targets."},
      {"planar audit handoff-readiness", "Check resume-readiness for all in-flight tasks."},
      {"planar health", "Report database, handoff, and installed-projection health."},
      {"planar health hygiene", "Report stale plan, task, and question lifecycle state without mutating it."},
      {"planar models", "Discover installed provider CLIs and their model catalogs."},
      {"planar models evals",
       "Aggregate completed dispatch outcomes into a per-(work-type, candidate) scorecard and preview-only recommendation."},
      {"planar models resolve", "Resolve a role's routing tier from its authoritative packet, or report the fallback and why."},
      {"planar models experiments", "List declared routing experiments and how much evidence each has produced."},
      {"planar models outcomes", "List recorded terminal outcomes, including excluded ones and why they were excluded."},
      {"planar models registry", "Manage opaque operator candidates and host observations."},
      {"planar models registry list", "List registrations, bindings, and latest observations."},
      {"planar models registry add", "Register one exact opaque candidate identifier."},
      {"planar models registry update", "Update enabled state and deterministic fallback order."},
      {"planar models registry remove", "Remove a candidate when no immutable evidence references it."},
      {"planar models registry bind", "Allow one role and tier for a candidate."},
      {"planar models registry unbind", "Remove one explicit role and tier binding."},
      {"planar models registry observe", "Append an exact, versioned host capability observation."},
      {"planar models registry eligibility", "Report every independent eligibility gate and named exclusion reason."},
      {"planar models registry verify-identity", "Compare requested and actual spawn identity without aliasing."},
      {"planar models registry export", "Export the versioned registry compatibility document."},
      {"planar dashboard", "Operator situational-awareness view of in-flight plans (and, with --agents, live claims)."},
      {"planar spec", "Spec pipeline commands (ingest)."},
      {"planar spec ingest", "Decompose workbench spec documents into the task graph."},
      {"planar test-spec", "Test-spec coverage inspectors."},
      {"planar test-spec status", "Print per-milestone test-spec coverage for an anchor plan."},
      {"planar config", "Manage Planar configuration."},
      {"planar config show", "Print the resolved configuration."},
      {"planar config edit", "Edit the configuration file in $EDITOR."},
      {"planar config validate", "Validate configuration file syntax."},
      {"planar config init", "Initialize the configuration file."},
      {"planar config path", "Show the configuration file path."},
      {"planar templates", "Inspect, validate, and render Planar JSON templates."},
      {"planar templates list", "List available templates."},
      {"planar templates show", "Show a template's raw JSON."},
      {"planar templates render", "Render a template against a database entity (dry run; no writes)."},
      {"planar templates validate", "Validate template syntax."},
      {"planar templates init", "Extract default templates to disk."},
      {"planar templates path", "Show template resolution paths."},
      {"planar tree", "Render plans, tasks, artifacts, decisions, scenarios, and questions as a hierarchical tree."},
      {"planar search", "Full-text search across plans, tasks, questions, scenarios, decisions, and artifacts."},
      {"planar local", "Manage user-local sandbox skills and agents under ~/.planar/local/."},
      {"planar local list", "List locally-installed skills and agents."},
      {"planar local link", "Create or reuse symlinks from vendor paths to local source."},
      {"planar local unlink", "Remove symlinks from vendor paths."},
      {"planar local import", "Import a skill or agent from an external directory."},
      {"planar local migrate", "Migrate skills/agents to new Planar version."},
      {"planar skills", "Retired: the skill tree is skills/planar/; planar health reports install drift."},
      {"planar import", "Import an existing repo's state into Planar."},
      {"planar synthesize", "Synthesize fresh planning artifacts from a repo's docs + code + git history."},
      {"planar version", "Print the planar version, commit, and C++ toolchain."},
      {"planar update", "Update the Planar installation to a published release."},
      {"planar completion", "Generate the autocompletion script for the specified shell."},
      {"planar schema", "Print the full command tree as a JSON catalog (flags, aliases, positionals)."},
      {"planar report", "Emit the diagnostic bundle: invocation and closed claim-failure aggregates plus health metrics."},
      {"planar bench", "Record and query benchmark run data (measurement rig)."},
      {"planar bench start", "Mint a new run record and print its run_uid."},
      {"planar bench event", "Append a journal event to a run."},
      {"planar bench touch", "Record a declared or actual file touch for a run."},
      {"planar bench harvest", "Harvest git diff as actual touches for a run/task."},
      {"planar bench finish", "Set the terminal status on a run."},
      {"planar bench show", "Show a run's full state (header + events + touches)."},
      {"planar closure", "Compute and inspect a task's derived symbol-level closure."},
      {"planar closure compute", "Run the extractor over a task's seeds and persist the closure."},
      {"planar closure show", "Read back a task's persisted closure rows."},
      {"planar run", "Record and query operational run traces."},
      {"planar run start", "Mint a new operational run record and print its run_uid as JSON."},
      {"planar run event", "Append a journal event to a run (seq auto-incremented)."},
      {"planar run finish", "Set the terminal status on a run."},
      {"planar run show", "Show a run's full state (header + events)."},
      {"planar groups", "Recommend task slices that minimize closure replication."},
      {"planar groups recommend", "Recommend closure-minimizing task slices for a plan."},
      {"planar explore", "Launch Planar Explorer (reserved; currently prints its help)."},
      {"planar workflow", "Discover, inspect, and run Lua workflows for planar-execute."},
      {"planar workflow list", "List shipped and sandbox workflows."},
      {"planar workflow show", "Show @meta and source path for a named workflow."},
      {"planar workflow run", "Resolve a workflow by name and exec it via planar-execute."},
      {"planar feedback", "Manage structured feedback."},
      {"planar feedback triage", "Review structured feedback triage."},
      {"planar feedback triage list", "List triaged findings."},
      {"planar feedback triage show", "Show a triaged finding."},
      {"planar feedback triage set", "Set operator-confirmed triage fields."},
      {"planar help", "Print the root help page (same as `planar --help`)."},
  };
  return k_summaries;
}

/// @brief The root-relative path keys that are DECLARED but not
/// IMPLEMENTED — every unported leaf, plus the unported dual nodes.
///
/// `explore` is the sole surviving entry (decisions 980 and 1003: the
/// cockpit is not ported), and `unported_inventory.t.cpp` pins exactly
/// that.
/// @return The inventory, sorted.
export auto unported_paths() -> std::span<std::string_view const> {
  static constexpr std::string_view k_unported[] = {
      // The `question`, `decision`, `scenario` and `artifact` DRAFTING
      // quartets — sixteen leaves — left this inventory at task 6205 with
      // the `editflow` port they were all blocked on.
      //
      // `plan edit | view | diff | review` and `task edit | view | diff |
      // review` — the remaining EIGHT — left it at task 6208, which is the
      // oracle run task 6205 held them for. The three paths that hold-note
      // named were each RUN against the oracle in a pinned arena before
      // wiring, and all three confirmed the already-ported arms:
      //   - `walk_to_anchor` from a CHILD plan and from a GRANDCHILD both
      //     land on the root (`anchor_plan_id: 1` for plans 2 and 3).
      //   - `README.md` is the anchor plan's OWN file; a non-anchor plan is
      //     `plans/<slug>.md`.
      //   - `task_workbench_dir` tries repo-scope, then a `touches` edge,
      //     then `cross` — confirmed per arm INCLUDING the precedence case
      //     the chain's shape alone does not settle: a task that is both
      //     repo-scoped to `proj` and `touches proj2` lands in
      //     `tasks/proj/`, so repo-scope wins.
      //
      // What the run DID find is a per-family divergence, and it is the
      // reason this was worth a task rather than a one-line wiring change:
      // for these two families a NONEXISTENT id reports `not_found`, not
      // `no_plan_link`. `plan diff 999` says `no plan with id 999` where
      // `question diff 999` says `question 999 is not linked to a plan`.
      // The `not_found` arm that `editflow.cpp`'s `prose_error` documents
      // as unreachable for the other four is the ONLY arm these two reach.
      // `assoc list` and `assoc remove` left this inventory at task 6279,
      // and `assoc detect` -- the family's last and largest remainder, the
      // ~680-line proposal engine (`detectProposals` /
      // `proposalsFromSignals` / `enrichProposals` / `applyProposals`) --
      // left it at task 6325. The `assoc` family is now fully ported and
      // contributes nothing to this array.
      //
      // `audit commits` left this inventory at task 6277. It had been
      // listed as blocked on the git-walk seam alongside `capture commits`
      // and `bench harvest`; that grouping was wrong (its handler spawns
      // nothing and reads rows the walk WRITES) and was corrected at task
      // 6272. The other two stay — they are genuinely blocked.
      // `audit publish-decision` left this inventory at task 6339, closing
      // the `audit` family. It had been carried as needing an adapter
      // INSTANCE — the auth-resolving factory landed at task 6258 — and
      // what actually remained was `postComment` on both adapters (~81
      // lines, not part of the four-operation `external_adapter`
      // interface) plus this 176-line handler. See handlers/audit.cppm.
      // `audit trail` left this inventory at task 6262.
      // `capture commits` left this inventory at task 6358. It had been
      // carried as blocked on 1205 lines of git-subprocess walking in
      // zig/src/engine/runtime/sessioncommits.zig, with "no process-spawn
      // seam in this tree" as the reason. Tasks 6128/6137 had already
      // closed that seam (`planar.git`) for `capture session` and the
      // worktree gate; this task reached it a second hop out through
      // `sessioncommits.cppm`'s new `resolve_repo_root_strict` /
      // `walk_strict` / `resolve_shas` / `record_count` (added to the SAME
      // `engine_runtime` CMake target `capture` already lived in — no new
      // engine-to-engine edge).
      // `bench harvest` left this inventory at task 6362, the last leaf
      // that note's "the other two stay" referred to. It had been carried
      // as blocked on the same git-subprocess seam, and by the time this
      // task landed the seam (`planar.git`, tasks 6128/6137, already used
      // by four other consumers) was the ONLY thing missing — the FAIL-
      // SOFT walk and claim-window fold the note above worried about
      // belong to `capture commits`' different oracle module
      // (`sessioncommits.zig`), not to `harvest.zig`, which shells exactly
      // two git subcommands (`diff --name-only`, `ls-files --others`) and
      // writes through the already-ported `touch_idempotent`. See
      // `src/engine/runs/harvest.cppm` for the port and
      // `src/engine/runs/CMakeLists.txt` for the closing account.
      // The five `config` leaves -- `show`, `edit`, `validate`, `init`,
      // `path` -- left this inventory at task 6259. Their engine half
      // (`planar.engine.config`) had been complete since commit 82820b7;
      // what landed was the wiring plus the config-file PATH, the two
      // starter blobs, and `config validate`'s four-step rule set.
      // `dashboard` left this inventory at task 6329. It had been parked
      // behind "the absent layer-3 cmd surface" since task 6102 — a
      // blocker layer 3's arrival at task 6105 removed and nobody
      // revisited. Every engine symbol it needs was already present.
      // `demote` and `promote` left this inventory at task 6299, together
      // with `test-spec status` below. All three were handler-only: their
      // engines — `planar.engine.promotion` (task 6094) and
      // `planar.engine.planning.test_spec_status` — had been ported in FULL
      // including their output renderers, so the cycle wired three leaves
      // and wrote no engine code.
      "explore",
      // `ext create` left this inventory at task 6295. It had been carried
      // as blocked on the create/propagate half of `engine_extsync` and
      // needed ZERO of those lines: its only uses of that surface were
      // `common.{LocalEntity, CreateOptions, Header}`, all three already
      // present as `adapter::local_entity` / `adapter::create_options` /
      // `http::header`. The real precondition was two `adapter_handle`
      // accessors. Its two siblings below stayed until task 6419, when the
      // whole `ext`/`sync` family (including `ext propagate-one`, already
      // ported by then) moved to `planar-ext`. `ext propagate` moved with
      // them conceptually but is not yet wired on EITHER binary — it
      // leaves THIS inventory not because it landed, but because `ext` no
      // longer has a home on `planar` at all.
      //
      // `ext propagate-one` left this inventory at task 6335, and it is the
      // SIXTH over-stated blocker of this milestone. It was carried under the
      // whole create/propagate half of `engine_extsync` — 3665 lines across
      // five files — and measured by SYMBOL it reaches exactly two functions,
      // ~40 lines: `strategyForSystem` (pure; landed as
      // `planar.engine.extsync.propagate`) and `loadExistingMirror` (SQL;
      // landed in `planar.engine.external.link`, because `engine_extsync`
      // carries no `db` edge). It reaches NOTHING in `parent_issue.zig` or
      // `projects_v2.zig` — the 2394 lines the task brief flagged as
      // "possibly not needed"; they are not needed, confirmed by call graph
      // rather than assumed. Same correction as `audit commits` (6272), the
      // `sync` trio (6294) and `plan descendants` (6298): a LEAF's
      // dependencies inferred from its MODULE's.
      //
      // `ext propagate` STAYS. It is the one leaf of the four that genuinely
      // wants the bulk — `selectStrategy`, `walkTree`, all of `strategy.zig`,
      // and both GitHub-specific files.
      //
      // Task 6353 landed ONE of those two GitHub-specific files in full:
      // `parent_issue.zig`'s entire orchestration (~1037 oracle
      // implementation lines, brace-balanced rather than whole-file
      // `wc -l` — the SAME 27% over-count task 6111 had already measured
      // for this pair) is now `planar.engine.external.parent_issue`, NOT a
      // new corner of `engine_extsync` — see that module's header for why:
      // the file is majority SQL and `engine_extsync` carries no `db`
      // edge, the same reason `record_mirror_link`/`load_existing_mirror`
      // live in `engine_external` rather than here. `projects_v2.zig`
      // (~1050 oracle implementation lines by the same brace-balanced
      // count) is UNTOUCHED — it is not a re-export of `parent_issue.zig`;
      // it duplicates that file's `entityForCreate` under its own name
      // (`parent_issue_entityForCreate`) and adds the ProjectsV2 GraphQL
      // surface (`getAuthenticatedOwner`, `createProjectV2`,
      // `getProjectV2Fields`, `addProjectV2Item`,
      // `setProjectV2ItemFieldValue`) on top.
      //
      // `ext propagate` STILL stays, for three independent reasons, only
      // one of which task 6353 touched:
      //   1. `projects_v2.zig` — untouched, per above.
      //   2. `selectStrategy` / `strategy.zig` (578 lines) — the ADR-0006
      //      repo-count bucketing that picks which of the two GitHub
      //      strategies (or the Jira one) applies. Untouched.
      //   3. NO PRODUCTION `gh_client` exists to drive
      //      `parent_issue::propagate_parent_issue_with_repo` with:
      //      `github_adapter` (engine_extsync/github.cppm) has not grown
      //      `createIssue`/`linkSubIssue`/`linkSubIssueProbe`/
      //      `postComment` — every one of them is still listed as
      //      deferred-with-`ext-propagate` in that bucket's own
      //      CMakeLists.txt, unchanged by this task. The CLI handler
      //      wiring itself (`ext.cpp`'s dispatch, matching
      //      `zig/src/cmd/planar/handlers/ext/propagate.zig`'s ~380-line
      //      `runParentIssueStrategy` bridge) is also not started.
      //
      // Worth reading before touching either creation path: `propagate-one`
      // is the IDEMPOTENT one, structurally — `load_existing_mirror` runs
      // before the template is loaded and before any adapter exists, and a
      // repeat returns `op:"skipped"` sending nothing. That makes it the
      // correct shape already present in this tree for defects 6312 and 6313
      // (`ext create` POSTs before validating `--role`, and POSTs a SECOND
      // ticket on repeat). Both defects remain reproduced deliberately in
      // `ext_create`; neither is visible to the state differential, because
      // both trees POST identically — only a fixture server's request log
      // shows them. `workbench publish` is a THIRD shape again: it REFUSES on
      // an existing link rather than skipping.
      // The three `feedback triage` leaves left this inventory at task 6303,
      // together with the `engine.planning.feedback_triage` engine that was
      // the whole of what blocked them.
      // `health` left this inventory at task 6357, closing the family (task
      // 6090 had already landed `health hygiene`). The blocker was real,
      // not over-stated this time: the handler folds
      // `engine.installedsurface.status` (548 Zig lines of manifest-driven
      // filesystem classification) into every run, and porting `check`
      // without it would report a permanently-stubbed `projection_freshness`
      // and get `overall` — the field the leaf's exit-1-on-degraded
      // contract reads — wrong on any machine with a managed install. What
      // unblocked it was the SAME move task 6352 used for `report`
      // (decision 981): rather than a same-layer `engine_health ->
      // engine_<classifier>` edge (D15/D18 FATAL on that), the classifier
      // was ported straight to layer 1 as `planar.installed_surface`
      // (src/lib/installed_surface) — it holds no `db` edge, so there was
      // nothing pulling it toward `engine_health` in the first place. See
      // handlers/health.cppm and src/engine/health/health.cppm.
      // `import` is wired at task 6106: its filesystem-only request/cache
      // engine stays below the handler, while this layer composes the
      // deterministic plan write without a D15 peer dependency.
      // `report` left this inventory at task 6352, once `engine_introspect`
      // (task 6121, DB aggregates) and `engine_introspection_adapters`
      // (tasks 6102 and 6352, filesystem discovery) were both complete and
      // decision 981's layer-1 `introspection_preview` extraction let
      // `bundle::preview` reach across the D15-forbidden `engine_* ->
      // engine_*` gap between them. See src/cmd/planar/handlers/report.cppm.
      // `link` left this inventory at task 6301. It was never engine-blocked:
      // `external::link::create`, `external::system::show_by_slug` and the
      // three `*_from_text` enums were all present, and the belief that it
      // was blocked rested on the ORACLE'S OWN HEADER COMMENT claiming
      // `--propagate` "refuses with NotImplemented" — which is false; the
      // code propagates. That one FLAG is the only blocked part, and it is
      // refused at exit 64 as a recorded divergence. See handlers/link.cppm.
      // `models resolve` left this inventory at task 6343, the family's
      // fourteenth and last leaf. Task 6111 had already landed its two PURE
      // halves (`profile`, `roles`) in `engine_models`; what remained was
      // `assemble_planning`/`compile_planning` in `engine_ingest`, plus this
      // handler's `packet::evidence` -> `profile::fact` adapter.
      //
      // The inherited ~300-line estimate UNDER-STATED it, and by a wider
      // margin than the usual colocated-Zig-test inflation runs the other
      // way: this port measured ~450 implementation lines in packet.cpp
      // alone. The gap is the counting-query duplication the estimate's own
      // note anticipated but did not size — the oracle's
      // `planningCoverageEvidence` calls `test_spec_status.compute` in three
      // lines because Zig has no same-layer-edge prohibition; this tree's
      // D15/D18 FATAL on that edge, so the three queries
      // (`milestone_task_count`, `milestone_scenario_count`,
      // `milestone_tasks_covered_count`) are reproduced by hand, in full,
      // rather than called (~90 of the ~450 lines by itself).
      //
      // Landed alongside a deliberate non-decision: `profile::profile`'s
      // `cohort` field stays unported (documented in profile.cppm) because
      // the oracle's `Cohort` has seven fields and this tree's
      // `ranking::cohort` has eight — constructing one here would mean
      // inventing the eighth. Nothing on this leaf's path needs it.
      // `plan descendants` left this inventory at task 6298, and its four
      // family siblings below did NOT. It had been grouped with them and
      // with `ext propagate` as blocked on the create/propagate half of
      // `engine_extsync`; for THIS leaf that was false. Its whole engine
      // need is `walkTree` -- 78 of `propagate.zig`'s 409 lines, three SQL
      // queries reaching no adapter, transport, credential or template.
      // `strategyForSystem`, `selectStrategy`, `countDistinctReposInFeature`,
      // `hasExistingMirrorLink` and `loadExistingMirror` are all unreached
      // from it, so `ext propagate` stays below with the rest of that file.
      //
      // The walk landed as `planar.engine.planning.descendants`, NOT under
      // `engine_extsync`, because that bucket's stated invariant is that it
      // carries NO `db` edge (see its CMakeLists) and this is nothing but
      // SQL. Third instance of the same correction as `audit commits`
      // (task 6272) and the `sync` write trio (task 6294): a LEAF's
      // dependencies inferred from its MODULE's.
      // `plan closeout` LEFT this inventory at task 6317, and with it the
      // `plan` family has no unported leaf at all. It was the last of the
      // four task 6298 measured, and the only one of them that WRITES —
      // `CLAUDE.md` calls it the authoritative gate for closing a plan, so
      // every arm was captured in a pinned scratch arena against a
      // deliberately disposable fixture, never against a real database.
      //
      // Three things about it are worth reading before anything else
      // touches `agent_work_claims`:
      //   - THE THREE CLAIM-READING QUERIES DISAGREE ABOUT `status`. The
      //     COUNT query filters `status='active'`; the two LOCALITY queries
      //     behind the advisory git layer do not, so a `released` claim
      //     contributes git evidence while contributing no count. Measured,
      //     not inferred.
      //   - `--dry-run` EXITS 0 EVEN WHEN THE GATE FAILS, and the verb's own
      //     help string above says the opposite. That sentence is stale in
      //     the oracle and is reproduced verbatim; the handler comment
      //     beside the check has the real rule and the measurement agrees
      //     with the comment. Fifth time this milestone that prose lost to
      //     code.
      //   - THE ALREADY-TERMINAL SHORT-CIRCUIT RETURNS A DIFFERENT SHAPE:
      //     `git_evidence: []` where a live evaluation with no locality data
      //     returns a ONE-entry synthetic `(none)` row. Same plan, before
      //     and after closing.
      //
      // It is also the ONE leaf in this tree with a deliberate behavioural
      // divergence from the oracle, confined to advisory fields. See
      // `src/engine/planning/closeout.cppm`'s DIVERGENCE section and the
      // note in that bucket's CMakeLists before assuming a git-evidence
      // difference is a bug.
      //
      // `plan next` LEFT this inventory at task 6309 — it was the fourth
      // and cheapest of the group, and it went exactly as sized:
      // `agentactivity::next_work` plus handler rendering, no surprises in
      // the engine half.
      //
      // `plan divergence` and `plan recommend-strategy` LEFT this inventory
      // TOGETHER at task 6310, which is how 6298 sized them: both sit on
      // `engine/planning/strategy.zig` and share ~300 lines of loader
      // substrate (`loadOpenTasks`/`loadTouches`/`loadClosureTouches`), so
      // splitting them across two cycles would have meant writing that
      // loader twice or leaving one leaf reaching into the other's
      // internals. One cycle, one private substrate in
      // `planar.engine.planning.strategy`, two entry points.
      //
      // The pairing was right for the loader and wrong for everything else:
      // the two verbs agree only on the candidate set and the two refusal
      // arms. `divergence` runs NO unilateral rule (measured: a 7-task
      // fixture where `recommend-strategy` serialized five reported
      // `declared_overlaps:0 derived_overlaps:0 flips:0`), an empty touch
      // set means opposite things to the two, `--closure-source` exists on
      // only one of them, and `jaccard` renders shortest-round-trip in JSON
      // but fixed-4-decimal in text. See strategy.cppm.
      //
      // `spec ingest` left this inventory at task 6365. Its brief carried the
      // now-familiar hypothesis that this is handler wiring over an already-
      // ported engine (`engine_ingest`'s parse/diff/coverage/render/
      // materialize read side, landed task 6035) -- true for PREVIEW mode,
      // and wrong for `--apply`: `engine_ingest`'s own CMakeLists.txt already
      // documented `apply.zig` (1616 Zig lines) as a genuine, architectural
      // non-port, because it composes SIX layer-2 `engine_*` peers
      // (`engine_planning`'s plan/task/decision/question/scenario CRUD,
      // `engine_entitylink`, `engine_runtime.session`) that D15/D18 forbid
      // another layer-2 bucket from depending on. What THAT note got wrong
      // was a stale premise, not the architecture: it said three of the six
      // callees "do not exist in the C++ tree yet" (decision, question,
      // scenario); all three had landed by this task. The fix that note
      // already named -- land the composition at LAYER 3, the D20 shape
      // `annotate add` and `unlink` pioneered -- is what this task did:
      // `handlers/spec_ingest.cpp` composes `engine_planning`,
      // `engine_entitylink` and `engine_runtime` directly, with no
      // `engine_ingest` module touched or extended. The handler's outer
      // `planar.db` transaction and the DB layer's nested-savepoint support
      // reproduce the oracle's single all-or-nothing `--apply` write set;
      // idempotency and rollback both have dedicated leaf tests.
      // `sync pull`, `sync push` and `sync resolve` left this inventory at
      // task 6294. All three were briefed as blocked on the unported
      // create/propagate half of `engine_extsync`; none of them touches it.
      // What they call is `engine.external.sync.{pullLink, pushLink,
      // resolveConflict}` — a DIFFERENT module that was already ported in
      // full — so the whole cycle was handler wiring plus one missing
      // cmd-layer helper (`guard_with_membership`, which moved to
      // `planar.cmd.planar.scope` at task 6303 when `feedback triage set`
      // became its second caller family). See handlers/sync.cppm.
      //
      // `sync status` left this inventory at task 6298. Task 6294's note
      // called it "NOT a fourth free leaf" and the shape bore that out --
      // it renders `engine.external.sync.status` rows, a listing none of
      // pull/push/resolve produces, takes `--entity` rather than a
      // positional, and runs NO cross-scope guard. But its ENGINE half was
      // already complete (`sync::status` + `link::list_filter` shipped with
      // the module), so what it needed was rendering, not engine work.
      // `task packet` left this inventory at task 6324. Task 6298 had
      // verified it BLOCKED on `engine/routing/packet.zig`'s 1674 lines and
      // was right about the size and wrong about the block: the LEAF needs
      // only that file's TASK half, and the PLANNING half it shares a file
      // with belonged to `models resolve`, deferred at the time with
      // `roles.zig` and `profile.zig` (the PLANNING half landed later, at
      // task 6343, in the SAME `packet.cppm`/`.cpp` — see the `models
      // resolve` note above). It landed in `engine_ingest` rather than a new
      // `engine_routing` bucket because its freshness computation is defined
      // in terms of `materialize`'s digests and D15/D18 FATAL on a
      // layer-2-to-layer-2 edge — see src/engine/ingest/CMakeLists.txt.
      // `task touches infer` left this inventory at task 6330, completing
      // the `task touches` family. Its deferral note called it "773 lines
      // of git-diff and language-aware path inference"; running the oracle
      // showed that to be wrong on both counts — it shells nothing, imports
      // no git and knows no languages. See dispatch.cpp's registration
      // comment and touchinfer.cppm.
      // `test-spec status` left this inventory at task 6299 — see the note
      // beside `explore` above.
      // `tree` left this inventory at task 6278. Its whole product is a
      // RENDERED hierarchy, so every expected byte — connectors, indent
      // extensions, dirs-first grouping, the summary footer — was captured
      // from the oracle against a fixture three levels deep with siblings
      // at more than one level, never reconstructed from what looked
      // reasonable. The capture also settled what no sibling verb could
      // have told it: on this one verb `--kind ''` REFUSES (exit 2),
      // `--status ''` matches nothing (exit 0), and `--scope ''` means
      // GLOBAL (exit 0) — three different meanings for the same empty
      // value. `--sort` is accepted and INERT in the oracle, and is
      // reproduced that way deliberately; see
      // `src/engine/tree/CMakeLists.txt`.
      // `workbench edit` and `workbench extract-questions` LEFT this
      // inventory at task 6302. Neither was ever architecturally blocked:
      // `edit` is push -> spawn `$EDITOR` on the feature directory -> pull
      // over the already-ported `editor::spawn_inherit`, and
      // `extract-questions` needed only a pure text walk, now
      // `planar.engine.workbench.questions`. `publish` stays, and is the
      // only workbench leaf with a real blocker: it needs
      // `extsync.parent_issue.recordLink` and `create_remote`, and lands
      // with the adapters.
      //
      // It LEFT this inventory at task 6335, and that deferral note was the
      // most misleading of the four: read as "lands with the adapters" it
      // implied the 1205-line `parent_issue.zig`. The leaf's whole reach into
      // that file is `recordLink` — 36 lines of SQL with no adapter,
      // transport, credential or template edge — now
      // `engine::external::link::record_mirror_link`, which is NOT `create`
      // with different arguments: it writes the `sync_events` audit row too.
      // `create_remote` was already in this tree, TU-private to `ext.cpp`; it
      // is now shared out of `ext_adapter_factory` for its three callers.
      // `workspace routing show` left this inventory at task 6110 and
      // `workspace routing build` at task 6275, closing the family's
      // read/write loop: the decoder and both render arms, then the builder
      // that WRITES the file they read. `build` was deferred on SIZE alone
      // (1410 lines, no architectural blocker) and that sizing held —
      // SQLite plus filesystem, no new dependency.
      //
      // `workspace regenerate` left this inventory at task 6364. See
      // `planar.engine.workspace.regenerate`'s header for the ported
      // hand-rolled template engine and its one deliberate reproduced quirk.
      //
      // `workspace init` is the family's LAST remaining leaf, and its
      // blocker is now strictly smaller than it was: it is layer-3 blocked
      // because it COMPOSES scan + registration + routing build +
      // regenerate + symlink install, and two of those four now exist in
      // this tree (`routing build` at task 6275, `regenerate` here).
  };
  return k_unported;
}
} // namespace planar::cmd
