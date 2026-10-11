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
///     this binary — `workbench publish` is its other caller and did not move,
///     and D18 forbids a
///     `cmd_planar -> cmd_planar_ext` edge — so
///     `src/cmd/planar-ext/handlers/shared/ext_adapter_factory.cppm` is a
///     deliberate duplicate, not a shared import. Same shape for the
///     cross-scope guard, which the `planar-ext sync` verbs still need:
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
      {"planar assoc", "Manage associations (many-to-many scope tags for repos)."},
      {"planar assoc list", "List all known associations."},
      {"planar assoc create", "Create a new association."},
      {"planar assoc add", "Add a repo to an association."},
      {"planar assoc remove", "Remove a repo from an association."},
      {"planar assoc members", "List all project members of an association."},
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
      {"planar audit handoff-readiness", "Check resume-readiness for all in-flight tasks."},
      {"planar health", "Report database, handoff, and installed-projection health."},
      {"planar health hygiene", "Report stale plan, task, and question lifecycle state without mutating it."},
      {"planar dashboard", "Operator situational-awareness view of in-flight plans (and, with --agents, live claims)."},
      {"planar spec", "Spec pipeline commands (ingest)."},
      {"planar spec ingest", "Decompose workbench spec documents into the task graph."},
      {"planar test-spec", "Test-spec coverage inspectors."},
      {"planar test-spec status", "Print per-milestone test-spec coverage for an anchor plan."},
      {"planar config", "Manage Planar configuration."},
      {"planar config show", "Print the resolved configuration."},
      {"planar config edit", "Edit the configuration file in $EDITOR."},
      {"planar config validate", "Validate configuration file syntax."},
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
      {"planar import", "Import an existing repo's state into Planar."},
      {"planar synthesize", "Synthesize fresh planning artifacts from a repo's docs + code + git history."},
      {"planar version", "Print the planar version, commit, and C++ toolchain."},
      {"planar update", "Update the Planar installation to a published release."},
      {"planar completion", "Generate the autocompletion script for the specified shell."},
      {"planar schema", "Print the full command tree as a JSON catalog (flags, aliases, positionals)."},
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
      {"planar help", "Print the root help page (same as `planar --help`)."},
  };
  return k_summaries;
}

/// @brief The root-relative path keys that are DECLARED but not
/// IMPLEMENTED — every unported leaf, plus the unported dual nodes.
///
/// Empty. `explore`, the last entry (decisions 980 and 1003: the cockpit is
/// not ported), was deleted outright in plan 1142 (task 7463), so every
/// declared leaf now has a handler. The array stays as a named,
/// scanner-recognized initializer, the same shape `planar-agent` uses, so
/// `unported_inventory.t.cpp` can still read it; `unregistered_leaves` keeps
/// the gate honest if a leaf is declared without a handler.
/// @return The empty inventory.
export auto unported_paths() -> std::span<std::string_view const> {
  static constexpr std::string_view k_unported[] = {std::string_view{}};
  return std::span<std::string_view const>{k_unported}.first(0);
}
} // namespace planar::cmd
