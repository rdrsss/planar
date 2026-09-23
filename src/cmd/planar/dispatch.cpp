/// @file dispatch.cpp
/// @brief Implementation of `planar.cmd.planar.dispatch`.

module planar.cmd.planar.dispatch;

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cliapp.walk;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.surface;
import planar.cmd.planar.worktree_gate;
import planar.cmd.planar.handlers.annotate;
import planar.cmd.planar.handlers.assoc;
import planar.cmd.planar.handlers.scope;
import planar.cmd.planar.handlers.search;
import planar.cmd.planar.handlers.tree;
import planar.cmd.planar.handlers.promotion;
import planar.cmd.planar.handlers.test_spec;
import planar.cmd.planar.handlers.health;
import planar.cmd.planar.handlers.audit;
import planar.cmd.planar.handlers.dashboard;
import planar.cmd.planar.handlers.report;
import planar.cmd.planar.handlers.capture;
import planar.cmd.planar.handlers.catalog;
import planar.cmd.planar.handlers.handoff;
import planar.cmd.planar.handlers.init;
import planar.cmd.planar.handlers.models;
import planar.cmd.planar.handlers.plan;
import planar.cmd.planar.handlers.resume;
import planar.cmd.planar.handlers.runs;
import planar.cmd.planar.handlers.local;
import planar.cmd.planar.handlers.config;
import planar.cmd.planar.handlers.templates;
import planar.cmd.planar.handlers.closure;
import planar.cmd.planar.handlers.groups;
import planar.cmd.planar.handlers.spec_ingest;
import planar.cmd.planar.handlers.importer;
import planar.cmd.planar.handlers.synthesize;
import planar.cmd.planar.handlers.skills;
import planar.cmd.planar.handlers.task;
import planar.cmd.planar.handlers.feedback;
import planar.cmd.planar.handlers.question;
import planar.cmd.planar.handlers.decision;
import planar.cmd.planar.handlers.scenario;
import planar.cmd.planar.handlers.artifact;
import planar.cmd.planar.handlers.drafting;
import planar.cmd.planar.handlers.links;
import planar.cmd.planar.handlers.link;
import planar.cmd.planar.handlers.unlink;
import planar.cmd.planar.handlers.version;
import planar.cmd.planar.handlers.workbench;
import planar.cmd.planar.handlers.workflow;
import planar.cmd.planar.handlers.workspace;

namespace planar::cmd {

namespace {

/// @brief The deepest node CLI11 actually matched, and the path taken to
/// reach it.
///
/// `CLI::App::get_subcommands()` returns the PARSED children at each
/// level, so following `.front()` down walks exactly the chain argv
/// selected. A node reached this way that still has children of its own
/// means the operator named a group without naming a leaf under it —
/// `run` renders that node's help page, matching what the deleted parser
/// did for a bare parent verb.
/// @param root The parsed root app.
/// @return The matched node and its root-relative path.
auto matched_node(CLI::App& root) -> std::pair<CLI::App*, std::vector<std::string>> {
  CLI::App*                node = &root;
  std::vector<std::string> path;
  while (true) {
    auto const matched = node->get_subcommands();
    if (matched.empty()) {
      return {node, path};
    }
    node = matched.front();
    path.push_back(node->get_name());
  }
}

/// @brief A `std::streambuf` that forwards every write through unchanged
/// while counting whether anything was written at all (task 6903).
///
/// Detects "the handler already wrote to stdout" AT THE DISPATCH SITE,
/// rather than special-casing individual verbs (`audit handoff-readiness
/// --json`, `templates validate --json`, and any future one): a handler
/// that fails after already emitting its own JSON payload must not also
/// get the additive `--json` error envelope appended, or a `json.loads`
/// consumer reading stdout sees two documents on one stream and breaks.
/// A handler that writes NOTHING before failing is unaffected -- it still
/// gets the envelope, which is the only document on the stream either way.
class counting_streambuf final : public std::streambuf {
public:
  explicit counting_streambuf(std::streambuf* sink) : _sink(sink) {
  }

  /// @return Whether any character passed through this buffer.
  [[nodiscard]] auto wrote_anything() const -> bool {
    return _wrote;
  }

protected:
  auto overflow(int_type ch) -> int_type override {
    if (ch == traits_type::eof()) {
      return traits_type::not_eof(ch);
    }
    _wrote = true;
    return _sink->sputc(static_cast<char>(ch));
  }

  auto xsputn(const char* s, std::streamsize count) -> std::streamsize override {
    if (count > 0) {
      _wrote = true;
    }
    return _sink->sputn(s, count);
  }

  auto sync() -> int override {
    return _sink->pubsync();
  }

private:
  std::streambuf* _sink;
  bool            _wrote = false;
};

/// @brief Run `fn`, reporting whether it wrote anything to `stream`.
/// @param stream The stream to intercept (`ctx.out()`).
/// @param fn The callable to run with `stream`'s rdbuf temporarily
/// replaced by a counting proxy.
/// @return Whatever `fn` returns.
template <class Fn> auto run_tracking_stdout_writes(std::ostream& stream, Fn&& fn) -> std::pair<std::invoke_result_t<Fn>, bool> {
  // The restore is a scope guard, not a statement after the call: a
  // handler that throws would otherwise leave `stream` pointing at a
  // destroyed `counting_streambuf`, so any later write — including one
  // during static destruction — would be a use-after-free. Handlers
  // surface failure through `std::expected` rather than exceptions, so
  // this costs nothing on the path we actually take; it removes a trap
  // for the one that we do not (reviewer, cycle 7).
  struct restore_rdbuf {
    std::ostream*   stream;
    std::streambuf* orig;
    ~restore_rdbuf() {
      stream->rdbuf(orig);
    }
  };

  auto* const        orig = stream.rdbuf();
  counting_streambuf counter{orig};
  stream.rdbuf(&counter);
  restore_rdbuf const guard{&stream, orig};
  auto                result = std::forward<Fn>(fn)();
  return {std::move(result), counter.wrote_anything()};
}

} // namespace

/// @brief A handler that always refuses with `not_implemented` (exit 64),
/// naming the verb.
///
/// The loud refusal the full-surface declaration rests on. Registered
/// explicitly, per verb, rather than left to `run`'s table-miss arm,
/// because of two distinct hazards the arm does not cover: a DUAL node
/// (subcommands AND its own handler in the oracle) never reaches the arm at
/// all — it falls to the help page and exits 0, a silent success — and an
/// unregistered leaf trips the `unregistered_leaves` gate, which is what
/// keeps the generated inventory honest as verbs get ported.
/// @param verb The root-relative path key, used verbatim in the message.
/// @return The handler.
auto not_implemented_for(std::string_view verb) -> handler_fn {
  return [body = std::format("{}: not implemented in this build", verb)](context&, const cliapp::parsed_args&) -> handler_result {
    return std::unexpected(error_from_body(domain_error_kind::not_implemented, body));
  };
}

/// @brief `explore`'s handler (decision 1003; task 6444).
///
/// `explore` stays in `unported_paths()` — there is no cockpit in this tree
/// and never will be — but unlike every other entry there, its own path is
/// NOT a refusal. The oracle's cockpit gate (non-TTY stdout, `TERM=dumb`,
/// `PLANAR_NO_TUI`, `--plain`) always refuses in a scripted/test
/// environment, and every refusal path prints the SAME thing: the verb's
/// own help page, exit 0. `--plan` / `--task` / `--scope` are declared
/// flags on this leaf (see `k_flags_44` in `surface.cpp`) so CLI11 already
/// parses and discards them before this handler runs; nothing here needs to
/// read them.
///
/// `node` is looked up once, at table-build time, via `root.get_subcommand`
/// rather than re-walked per invocation.
/// @param node The `explore` leaf's CLI11 node.
/// @return A handler that always succeeds, printing the leaf's help text.
auto explore_fallback(const CLI::App* node) -> handler_fn {
  return [node](context& ctx, const cliapp::parsed_args&) -> handler_result {
    ctx.out() << node->help();
    return {};
  };
}

auto make_handler_table(const CLI::App& root) -> handler_table {
  handler_table table;
  table.emplace("init", handlers::init);
  table.emplace("plan create", handlers::plan_create);
  table.emplace("plan show", handlers::plan_show);
  table.emplace("plan list", handlers::plan_list);
  table.emplace("plan update", handlers::plan_update);
  table.emplace("plan recompute-status", handlers::plan_recompute_status);
  // `plan step` — the whole five-leaf family, landed at task 6187 together
  // with the `plan_steps` engine it calls. Until then the entity had no
  // port at all and all five refused at exit 64.
  table.emplace("plan step add", handlers::plan_step_add);
  table.emplace("plan step list", handlers::plan_step_list);
  table.emplace("plan step done", handlers::plan_step_done);
  table.emplace("plan step skip", handlers::plan_step_skip);
  table.emplace("plan step link", handlers::plan_step_link);
  table.emplace("assoc create", handlers::assoc_create);
  table.emplace("assoc add", handlers::assoc_add);
  // `assoc members` — the family's read half, wired at task 6188. Its
  // engine call has existed since the family landed; what was missing was a
  // renderer for `project_ref` (this module's other two are singular and
  // over `association`). Five `groups_recommend_test` integration frames
  // were crashing in their FIXTURE on this verb, not in the verb they test.
  table.emplace("assoc members", handlers::assoc_members);
  // `assoc list` + `assoc remove` — the family's two CHEAP remainders,
  // landed at task 6279. `remove` was handler-only (`remove_member` shipped
  // with the family); `list` needed a ten-line kind filter beside the
  // already-present `list_all`, plus the pair of list renderers.
  //
  // `assoc list` is also `statediff.t.cpp`'s LAST `expected_unported`
  // entry, so wiring it here is what emptied that set.
  //
  // `assoc detect` stays unported: it is the ~680-line proposal engine
  // (`detectProposals`/`proposalsFromSignals`/`enrichProposals`/
  // `applyProposals`), which is the whole of the remaining work in this
  // family and shares nothing with these two.
  table.emplace("assoc list", handlers::assoc_list);
  table.emplace("assoc remove", handlers::assoc_remove);
  table.emplace("assoc detect", handlers::assoc_detect);
  // `scope` — the WHOLE five-leaf family, landed at task 6214. Two do work
  // (`show` reads the cwd-derived read set, `suggest` the project's existing
  // memberships) and three are plan-153-M5 removal refusals that must NOT be
  // left at the exit-64 default: the oracle answers exit 2 with a remedy
  // paragraph, and exit 64 would read as "coming in a later build".
  table.emplace("scope show", handlers::scope_show);
  table.emplace("scope suggest", handlers::scope_suggest);
  table.emplace("scope use", handlers::scope_use);
  table.emplace("scope pop", handlers::scope_pop);
  table.emplace("scope clear", handlers::scope_clear);
  // `search` — the whole verb, landed at task 6090 with
  // `planar.engine.search`. One leaf, and the only one of that task's nine
  // that needed a new layer-2 bucket built from scratch.
  table.emplace("search", handlers::search);
  table.emplace("tree", handlers::tree);
  // `health` and `health hygiene` — the whole family, closed at task 6357.
  // Task 6090 landed only the SUBCOMMAND; the parent stayed at the exit-64
  // default because its handler folds `engine.installedsurface.status`
  // (548 unported Zig lines) into every run and a projections-stubbed port
  // would report the wrong `overall`. Task 6357 ported that classifier
  // straight to layer 1 (`planar.installed_surface`, decision-981-shaped,
  // same move `report` used at task 6352) rather than adding a same-layer
  // `engine_health -> engine_<classifier>` edge, so both leaves now share
  // one dispatch table entry each. `health` is a DUAL node — a handler AND
  // a subcommand — same shape as `handoff`/`resume`. See handlers/health.cppm.
  table.emplace("health", handlers::health);
  table.emplace("health hygiene", handlers::health_hygiene);
  // `audit` — all five leaves. The note that used to stand here said
  // `commits` needed git subprocess REVISION WALKS. It does not, and never
  // did — the oracle handler calls `listFiltered` and `writeJsonList`
  // over rows an earlier `capture commits` wrote, and spawns nothing.
  // `capture commits` and `bench harvest` really are blocked on the walk;
  // this leaf was mis-grouped with them. Corrected at task 6272, ported at
  // 6277. `publish-decision` closed the family at task 6339, once
  // `postComment` landed on both adapters — see handlers/audit.cppm.
  table.emplace("audit session", handlers::audit_session);
  table.emplace("audit trail", handlers::audit_trail);
  table.emplace("audit commits", handlers::audit_commits);
  table.emplace("audit handoff-readiness", handlers::audit_handoff_readiness);
  table.emplace("audit publish-decision", handlers::audit_publish_decision);
  table.emplace("dashboard", handlers::dashboard);
  // `report` — landed at task 6352 with the layer-1 `introspection_preview`
  // extraction (decision 981) that lets `engine_introspect`'s `bundle`
  // carry a `preview` field without an `engine_* -> engine_*` edge. Both
  // engine buckets it composes (`engine_introspect`, DB aggregates;
  // `engine_introspection_adapters`, filesystem discovery) were already
  // ported; this handler is the D20 composition layer3 exists for.
  table.emplace("report", handlers::report);
  table.emplace("task add", handlers::task_add);
  table.emplace("task show", handlers::task_show);
  table.emplace("task packet", handlers::task_packet);
  table.emplace("task list", handlers::task_list);
  table.emplace("task update", handlers::task_update);
  table.emplace("task done", handlers::task_done);
  table.emplace("task cancel", handlers::task_cancel);
  table.emplace("task block", handlers::task_block);
  table.emplace("task reopen", handlers::task_reopen);
  // `task touches` — all four, complete at task 6330 with `infer`. The
  // note that stood here deferred `infer` as "773 lines of git-diff and
  // language-aware path inference"; that description was WRONG on both
  // counts and is corrected rather than carried forward.
  // `planning/touchinfer.zig` shells nothing, imports no git and knows no
  // languages — it splits the task's own title/body/next_action on
  // whitespace, keeps the path-shaped tokens, and stats each against the
  // repo checkout. The paraphrase had simply outlived contact with the
  // file. See touchinfer.cppm for the algorithm and for the one divergence
  // (expansion ORDER: the oracle emits readdir order, this sorts).
  //
  // The three that landed earlier also unblocked the `--touches` filter on
  // `plan list` and `task list`, which refused at exit 64 from task 6141
  // for want of `listTouching`.
  table.emplace("task facts stage", handlers::task_facts_stage);
  table.emplace("task touches add", handlers::task_touches_add);
  table.emplace("task touches infer", handlers::task_touches_infer);
  table.emplace("task touches list", handlers::task_touches_list);
  table.emplace("task touches remove", handlers::task_touches_remove);
  // `feedback triage` — all three leaves, landed at task 6303 with the
  // `engine.planning.feedback_triage` engine they call. `feedback` and
  // `feedback triage` are pure GROUPS and stay unregistered so they fall to
  // the help path at exit 0.
  table.emplace("feedback triage list", handlers::feedback_triage_list);
  table.emplace("feedback triage show", handlers::feedback_triage_show);
  table.emplace("feedback triage set", handlers::feedback_triage_set);
  // `question` — six of ten. Five landed at task 6188 with the `question`
  // engine they call; `question link` joined them at task 6193 with the
  // rest of the entity-link surface. The four still omitted are named
  // refusals, not oversights: `edit`/`view`/`diff`/`review` are the
  // workbench DRAFTING quartet (unported editflow plumbing).
  table.emplace("question add", handlers::question_add);
  table.emplace("question show", handlers::question_show);
  table.emplace("question list", handlers::question_list);
  table.emplace("question answer", handlers::question_answer);
  table.emplace("question wontfix", handlers::question_wontfix);
  // `decision` — seven of eleven, landed at task 6194 with the `decision`
  // engine they call. `decision link` is included rather than held back the
  // way `question link` was at task 6188, because the entity-link surface
  // it forwards into landed in the meantime (task 6193): holding it now
  // would leave a working `question link` beside a refusing `decision
  // link`, which is the same incoherence the earlier hold was avoiding.
  // The four omitted are the workbench DRAFTING quartet
  // (`edit`/`view`/`diff`/`review`) — unported editflow plumbing, and the
  // oracle's own four are incoherent besides (two abort with a Zig stack
  // trace, two report NotFound for a decision that exists). See
  // handlers/decision.cppm's header.
  table.emplace("decision add", handlers::decision_add);
  table.emplace("decision show", handlers::decision_show);
  table.emplace("decision list", handlers::decision_list);
  table.emplace("decision accept", handlers::decision_accept);
  table.emplace("decision supersede", handlers::decision_supersede);
  table.emplace("decision withdraw", handlers::decision_withdraw);
  table.emplace("decision link", handlers::decision_link);
  // `scenario` — six of ten, landed at task 6195 with the `scenario`
  // engine they call. `scenario link` goes in with them for the same
  // reason `decision link` did: the shared entity-link surface it forwards
  // into landed at task 6193, so holding it would leave a refusing arm
  // beside two working ones.
  //
  // The four omitted are the workbench DRAFTING quartet
  // (`edit`/`view`/`diff`/`review`) — unported `engine_workbench` plus
  // editflow. Unlike `decision`'s four, the ORACLE's scenario quartet is
  // NOT broken: `view`/`diff`/`review` on a plan-linked scenario all exit 0
  // with coherent output, because `scenario add --plan` writes its edge
  // with the `from_kind = 'test_scenario'` spelling editflow's anchor
  // resolver queries. So these four are deferred for the DEPENDENCY alone
  // and can be ported as-is once it lands. See handlers/scenario.cppm.
  //
  // `scenario list --touches` is SERVED here rather than refused at exit
  // 64 the way `plan list --touches` and `task list --touches` are: this
  // family's `listTouching` half is ported.
  table.emplace("scenario add", handlers::scenario_add);
  table.emplace("scenario show", handlers::scenario_show);
  table.emplace("scenario list", handlers::scenario_list);
  table.emplace("scenario verify", handlers::scenario_verify);
  table.emplace("scenario retire", handlers::scenario_retire);
  table.emplace("scenario link", handlers::scenario_link);
  // `artifact` — FIVE of nine, landed at task 6196 with the `artifact`
  // engine they call, completing the planning ENGINE surface. `artifact
  // link` goes in with them on the reason 6194 and 6195 established.
  //
  // The four omitted are the workbench DRAFTING quartet a FOURTH time
  // (`edit`/`view`/`diff`/`review`). The blocker has NARROWED since the
  // scenario cycle recorded it: `engine_workbench` is ported and all ten
  // `workbench` leaves are wired, so what remains is
  // `zig/src/cmd/planar/editflow.zig` (2076 lines) plus `editor.zig` (342)
  // — a CMD-layer module, not an engine one. It gates the same four leaves
  // on `question`, `decision`, `scenario` and `artifact` alike, SIXTEEN in
  // total, so it belongs to its own task rather than riding in on the one
  // scoped to this family's engine.
  //
  // The oracle's artifact quartet is also NOT uniformly coherent, which is
  // a second reason not to guess at it: `diff`/`review` on an unlinked
  // artifact report a clean `artifact N is not linked to a plan`, while
  // `view` on the SAME input dies with a raw Zig stack trace
  // (`error: NoPlanLink` plus frames). `view` and `diff` also disagree
  // about where the file lives — `diff`/`review`/`workbench push` use
  // `<feature>/<id>-<slug>.md` while `view`/`edit` use
  // `<feature>/artifacts/<id>-<slug>.md`, so a `view` leaves a file
  // `workbench push` then reports as `new_on_fs` drift. Both shapes ship.
  // See handlers/artifact.cppm.
  table.emplace("artifact add", handlers::artifact_add);
  table.emplace("artifact show", handlers::artifact_show);
  table.emplace("artifact list", handlers::artifact_list);
  table.emplace("artifact update", handlers::artifact_update);
  table.emplace("artifact link", handlers::artifact_link);
  // The DRAFTING QUARTET, all sixteen arms, landed together at task 6205
  // with the `editflow` port every one of them was blocked on. Four cycles
  // deferred them four times; `engine_workbench` landing left
  // `zig/src/cmd/planar/editflow.zig` (2076 lines) plus `editor.zig` (342)
  // as the whole remaining dependency, and it is a CMD-layer module, so it
  // is here rather than under `src/lib/`.
  //
  // THE FOUR FAMILIES AGREE, which is the finding this cycle did not
  // expect. Every prior cycle recorded a real per-family divergence and
  // the standing note for this one predicted a fifth — that `scenario`'s
  // quartet works where `decision`'s does not, because `scenario add
  // --plan` writes `from_kind = 'test_scenario'` and the others do not.
  // The database says otherwise: all four families write a `derives-from`
  // edge, `scenario` alone spells its `from_kind` `test_scenario`, and
  // `entity_link_kind` already maps it. The observed split was a
  // plan-LINKED row compared against an UNLINKED one.
  //
  // What does diverge is between VERBS, and all of it is preserved:
  //   - `view`/`edit` render THIN (title + status); `diff`/`review` use
  //     `engine.workbench.sync`'s CANONICAL renderer, which also carries
  //     `**Created:**`/`**Updated:**`. So `view` then `diff` reports a
  //     diff on the file `view` just wrote. All four families.
  //   - `artifact` alone disagrees about WHERE: `view`/`edit` write
  //     `<feature>/artifacts/<id>-<slug>.md`, `diff`/`review` read
  //     `<feature>/<id>-<slug>.md`. This is the divergence the artifact
  //     cycle recorded above; it survives the port intact.
  //   - `diff`/`review` refuse `id <= 0` at exit 2 and map failures to
  //     prose; `view`/`edit` do neither.
  //
  // ONE DELIBERATE DIVERGENCE: on an unlinked entity the oracle's
  // `view`/`edit` print a prose line, then the bare tag `error: NoPlanLink`,
  // then SEVEN Zig stack frames naming absolute paths inside the oracle's
  // own build tree. This build emits the prose line, the tag and exit 1,
  // and stops. See `editflow.cppm`'s header.
  table.emplace("question edit", handlers::question_edit);
  table.emplace("question view", handlers::question_view);
  table.emplace("question diff", handlers::question_diff);
  table.emplace("question review", handlers::question_review);
  table.emplace("decision edit", handlers::decision_edit);
  table.emplace("decision view", handlers::decision_view);
  table.emplace("decision diff", handlers::decision_diff);
  table.emplace("decision review", handlers::decision_review);
  table.emplace("scenario edit", handlers::scenario_edit);
  table.emplace("scenario view", handlers::scenario_view);
  table.emplace("scenario diff", handlers::scenario_diff);
  table.emplace("scenario review", handlers::scenario_review);
  table.emplace("artifact edit", handlers::artifact_edit);
  table.emplace("artifact view", handlers::artifact_view);
  table.emplace("artifact diff", handlers::artifact_diff);
  table.emplace("artifact review", handlers::artifact_review);
  // `plan` and `task` carry the SAME quartet over the SAME module, and the
  // remaining EIGHT arms landed at task 6208 — the oracle run 6205 held
  // them for rather than wiring them on the strength of "it compiles for
  // them too". Three arms the link-anchored four never reach were each run
  // against the oracle in a pinned arena, and all three confirmed the port:
  //   - `walk_to_anchor` from a CHILD plan, and from a GRANDCHILD, both
  //     resolve to the root plan.
  //   - The ANCHOR plan's file is the feature's `README.md`; any other
  //     plan's is `plans/<slug>.md`.
  //   - `task_workbench_dir` tries repo-scope, then `touches`, then
  //     `cross`. The precedence case the chain's shape does not settle was
  //     run too: repo-scoped `proj` PLUS `touches proj2` lands in
  //     `tasks/proj/`, so repo-scope wins.
  //
  // ONE DIVERGENCE FROM THE OTHER FOUR, and it is the finding that made
  // this a task: on a NONEXISTENT id these two report `not_found`, not
  // `no_plan_link`, because their anchor resolvers read `plans` and `tasks`
  // rather than `entity_links`. `plan diff 999` prints `no plan with id
  // 999`; `question diff 999` prints `question 999 is not linked to a
  // plan`. Both are exit 1. See `handlers/drafting.cppm`.
  //
  // What does NOT diverge, and was checked rather than assumed: unlike
  // `artifact`, `view`/`edit` and `diff`/`review` agree about WHERE the
  // file lives for both of these families. The thin-vs-canonical RENDERER
  // split still applies to them, so `plan view 1 && plan diff 1` still
  // reports the two timestamp lines as a pending change.
  table.emplace("plan edit", handlers::plan_edit);
  table.emplace("plan view", handlers::plan_view);
  table.emplace("plan diff", handlers::plan_diff);
  table.emplace("plan review", handlers::plan_review);
  table.emplace("task edit", handlers::task_edit);
  table.emplace("task view", handlers::task_view);
  table.emplace("task diff", handlers::task_diff);
  table.emplace("task review", handlers::task_review);
  // The entity-link surface — all seven arms, landed together at task
  // 6193. `engine_entitylink` was fully ported and exported no renderer,
  // which is the single reason every one of these refused at exit 64.
  // Task 6188 declined to wire `question link` alone for exactly this
  // reason: one working arm beside six refusing ones reads as a bug.
  //
  // `links update` is absent because the ORACLE hides it (a documented
  // stub deferred to M11); it is not among the four declared `links`
  // leaves. The `ext`/`sync` families are a separate slice — they render
  // `external_links`, a different table with its own envelopes.
  table.emplace("links add", handlers::links_add);
  table.emplace("links list", handlers::links_list);
  table.emplace("links remove", handlers::links_remove);
  table.emplace("links trail", handlers::links_trail);
  table.emplace("plan link", handlers::plan_link);
  table.emplace("plan descendants", handlers::plan_descendants);
  table.emplace("plan next", handlers::plan_next);
  // Task 6310 landed these two TOGETHER. Task 6298 measured that they share
  // ~300 lines of loader substrate in the oracle's `strategy.zig`, so porting
  // them in separate cycles would mean writing that loader twice or leaving
  // one leaf reaching into the other's internals. They now share one private
  // substrate in `planar.engine.planning.strategy` and agree on nothing else
  // -- see strategy.cppm for the measured disagreements.
  table.emplace("plan recommend-strategy", handlers::plan_recommend_strategy);
  table.emplace("plan divergence", handlers::plan_divergence);
  table.emplace("plan closeout", handlers::plan_closeout);
  table.emplace("task link", handlers::task_link);
  table.emplace("question link", handlers::question_link);
  table.emplace("version", handlers::version);
  // `schema` and `completion` describe the TREE, so they take it; every
  // other handler describes DATA and does not. Same shape as the
  // `planar-agent` and `planar-watch` tables.
  table.emplace("schema", [&root](context& ctx, const cliapp::parsed_args& args) -> handler_result {
    return handlers::schema(ctx, args, root);
  });
  table.emplace("completion", [&root](context& ctx, const cliapp::parsed_args& args) -> handler_result {
    return handlers::completion(ctx, args, root);
  });
  table.emplace("workflow list", handlers::workflow_list);
  table.emplace("workflow show", handlers::workflow_show);
  table.emplace("workflow run", handlers::workflow_run);
  table.emplace("annotate add", handlers::annotate_add);
  table.emplace("annotate list", handlers::annotate_list);
  table.emplace("annotate capabilities", handlers::annotate_capabilities);
  table.emplace("annotate show", handlers::annotate_show);
  table.emplace("annotate update", handlers::annotate_update);
  table.emplace("annotate remove", handlers::annotate_remove);
  table.emplace("annotate tag", handlers::annotate_tag);
  table.emplace("annotate resolve", handlers::annotate_resolve);
  table.emplace("annotate dismiss", handlers::annotate_dismiss);
  table.emplace("annotate archive", handlers::annotate_archive);
  table.emplace("annotate bulk-resolve", handlers::annotate_bulk_resolve);
  table.emplace("annotate bulk-dismiss", handlers::annotate_bulk_dismiss);
  table.emplace("annotate bulk-archive", handlers::annotate_bulk_archive);
  table.emplace("annotate verify", handlers::annotate_verify);
  table.emplace("annotate sweep", handlers::annotate_sweep);
  table.emplace("annotate command", handlers::annotate_command);
  table.emplace("annotate receipt", handlers::annotate_receipt);
  table.emplace("link", handlers::link);
  table.emplace("unlink", handlers::unlink);
  // The whole `ext` family (register/list/test/create/propagate-one) and
  // the whole `sync` family (pull/push/status/resolve) moved to
  // `planar-ext` at plan 996, task 6419. `ext propagate` was never wired
  // here (see `surface.cpp`'s `unported_paths` header) and is not wired
  // on either binary yet.
  // `promote` and `demote` are TOP-LEVEL leaves, not a family: they take no
  // subcommand, so their table key is the bare verb.
  table.emplace("promote", handlers::promote);
  table.emplace("demote", handlers::demote);
  table.emplace("test-spec status", handlers::test_spec_status);
  // `skills` has no subcommands, so it is a LEAF and needs an entry here
  // even though the verb is retired and does nothing but render its own
  // help page. See that handler's header.
  table.emplace("skills", handlers::skills);
  table.emplace("workspace doctor", handlers::workspace_doctor);
  table.emplace("workspace init", handlers::workspace_init);
  table.emplace("workspace routing show", handlers::workspace_routing_show);
  table.emplace("workspace routing build", handlers::workspace_routing_build);
  table.emplace("workspace regenerate", handlers::workspace_regenerate);
  table.emplace("workbench lint", handlers::workbench_lint);
  table.emplace("workbench pull", handlers::workbench_pull);
  table.emplace("workbench push", handlers::workbench_push);
  table.emplace("workbench status", handlers::workbench_status);
  table.emplace("workbench resolve", handlers::workbench_resolve);
  table.emplace("workbench sync", handlers::workbench_sync);
  table.emplace("workbench archive", handlers::workbench_archive);
  table.emplace("workbench restore", handlers::workbench_restore);
  table.emplace("workbench gc", handlers::workbench_gc);
  table.emplace("workbench list", handlers::workbench_list);
  table.emplace("workbench extract-questions", handlers::workbench_extract_questions);
  table.emplace("workbench edit", handlers::workbench_edit);
  // Task 6335. Carried as blocked on the create/propagate half of
  // `engine_extsync`; needed 36 of its 3665 lines (`recordLink`).
  table.emplace("workbench publish", handlers::workbench_publish);
  table.emplace("capture session", handlers::capture_session);
  table.emplace("capture end", handlers::capture_end);
  table.emplace("capture note", handlers::capture_note);
  table.emplace("capture command", handlers::capture_command);
  table.emplace("capture file", handlers::capture_file);
  table.emplace("capture snapshot", handlers::capture_snapshot);
  table.emplace("capture commits", handlers::capture_commits);
  // `handoff` and `resume` are DUAL group-and-leaf nodes: each has
  // subcommands AND its own handler. Registering the parent is what makes
  // dispatch route the bare form to the handler instead of a help page.
  table.emplace("handoff", handlers::handoff);
  table.emplace("handoff create", handlers::handoff_create);
  table.emplace("handoff validate", handlers::handoff_validate);
  table.emplace("handoff consume", handlers::handoff_consume);
  table.emplace("handoff abandon", handlers::handoff_abandon);
  table.emplace("handoff list", handlers::handoff_list);
  table.emplace("handoff show", handlers::handoff_show);
  table.emplace("resume", handlers::resume_packet);
  table.emplace("resume validate", handlers::resume_validate);
  // `bench` and `run` — one engine bucket, two surfaces over the same
  // table. `bench harvest` is deliberately absent and stays a declared
  // exit-64 refusal; its engine half was deferred WITH its git-subprocess
  // dependency in task 6095. See `handlers/runs.cppm`.
  // `models` — all fourteen leaves. `models resolve` left the unported
  // inventory at task 6343, once `engine_ingest`'s planning half
  // (`assemble_planning`, `compile_planning`) joined `engine_models`'s
  // `profile` and `roles` (task 6111): the last missing piece was the
  // `packet::evidence` -> `profile::fact` adapter, which belongs here — see
  // `handlers/models.cppm`.
  table.emplace("models registry list", handlers::models_registry_list);
  table.emplace("models registry export", handlers::models_registry_export);
  table.emplace("models registry add", handlers::models_registry_add);
  table.emplace("models registry update", handlers::models_registry_update);
  table.emplace("models registry remove", handlers::models_registry_remove);
  table.emplace("models registry bind", handlers::models_registry_bind);
  table.emplace("models registry unbind", handlers::models_registry_unbind);
  table.emplace("models registry observe", handlers::models_registry_observe);
  table.emplace("models registry eligibility", handlers::models_registry_eligibility);
  table.emplace("models registry verify-identity", handlers::models_registry_verify_identity);
  table.emplace("models evals", handlers::models_evals);
  table.emplace("models experiments", handlers::models_experiments);
  table.emplace("models outcomes", handlers::models_outcomes);
  table.emplace("models resolve", handlers::models_resolve);
  table.emplace("bench start", handlers::bench_start);
  table.emplace("bench event", handlers::bench_event);
  table.emplace("bench touch", handlers::bench_touch);
  table.emplace("bench harvest", handlers::bench_harvest);
  table.emplace("bench finish", handlers::bench_finish);
  table.emplace("bench show", handlers::bench_show);
  table.emplace("run start", handlers::run_start);
  table.emplace("run event", handlers::run_event);
  table.emplace("run finish", handlers::run_finish);
  table.emplace("run show", handlers::run_show);

  // `local` — the whole five-leaf family (task 6189). None of them opens
  // SQLite; all five are filesystem state under `$PLANAR_LOCAL_HOME`/`$HOME`.
  table.emplace("local list", handlers::local_list);
  table.emplace("local link", handlers::local_link);
  table.emplace("local unlink", handlers::local_unlink);
  table.emplace("local import", handlers::local_import);
  table.emplace("local migrate", handlers::local_migrate);
  // `templates` — the whole six-leaf family (task 6190). The RESOLUTION
  // half of the plane had been ported since task 6032
  // (`planar.engine.config.templates`); what was missing was the RENDERING
  // half — a JSON DOM that preserves key order, the `{{...}}` substituter,
  // the validator, the DB context builder and the disk extractor — which
  // landed as `planar.engine.templates` alongside these handlers.
  //
  // Only `templates render` opens SQLite, and only to read: the other five
  // work on a machine that has never run `planar init`, which is why
  // `ctx.db().opened()` is pinned false for them.
  table.emplace("templates list", handlers::templates_list);
  table.emplace("templates show", handlers::templates_show);
  table.emplace("templates render", handlers::templates_render);
  table.emplace("templates validate", handlers::templates_validate);
  table.emplace("templates init", handlers::templates_init);
  table.emplace("templates path", handlers::templates_path);
  // `config` — the whole five-leaf family (task 6259). The engine half
  // (`planar.engine.config`) landed complete in commit 82820b7; this is
  // wiring over it plus the config-file PATH, which is a process concern
  // the engine deliberately does not own.
  //
  // NONE of the five opens SQLite, and `ctx.db().opened()` is pinned false
  // after each. `config init` and `config path` are what an operator runs
  // BEFORE `planar init`; a version that opened the database would also
  // MIGRATE it.
  table.emplace("config show", handlers::config_show);
  table.emplace("config edit", handlers::config_edit);
  table.emplace("config validate", handlers::config_validate);
  table.emplace("config init", handlers::config_init);
  table.emplace("config path", handlers::config_path);
  // Derived closure extraction is AST-backed through the pinned tree-sitter
  // Zig grammar; its cross-bucket composition belongs at this cmd layer.
  table.emplace("closure compute", handlers::closure_compute);
  table.emplace("closure show", handlers::closure_show);
  // `groups recommend` — the whole `groups` family, read-only.
  table.emplace("groups recommend", handlers::groups_recommend);
  table.emplace("spec ingest", handlers::spec_ingest);
  table.emplace("import", handlers::import_repo);
  table.emplace("synthesize", handlers::synthesize);
  // `explore` stays in `unported_paths()` (decision 1003 — the cockpit is
  // dropped, not deferred) but its own fallback IS implemented: every gate
  // refusal in the oracle prints the verb's help text, not an error. See
  // `explore_fallback`'s header just above.
  table.emplace("explore", explore_fallback(root.get_subcommand("explore")));

  // Everything above is IMPLEMENTED. Everything below is DECLARED and
  // refuses at exit 64. The inventory is generated alongside the surface
  // itself (`planar.cmd.planar.surface`), so a verb that gains a real
  // handler above must be dropped from it in the same regeneration — the
  // `emplace` here is a no-op on a key already present, so a stale entry
  // cannot silently shadow a real handler, and `unported_paths` staying
  // stale in the other direction fails `unregistered_leaves`.
  for (auto const& verb : unported_paths()) {
    table.emplace(std::string{verb}, not_implemented_for(verb));
  }
  return table;
}

auto unregistered_leaves(const CLI::App& root, const handler_table& table) -> std::vector<std::string> {
  std::vector<std::string> missing;
  for (auto& key : cliapp::leaf_keys(root)) {
    if (!table.contains(key)) {
      missing.push_back(std::move(key));
    }
  }
  return missing;
}

auto unreachable_handlers(const CLI::App& root, const handler_table& table) -> std::vector<std::string> {
  // EVERY node, not only childless ones: a dual group-and-leaf node
  // (`handoff`, `resume`) carries a handler and is reachable through it.
  std::set<std::string, std::less<>> reachable;
  for (auto const& node : cliapp::all_nodes(root)) {
    reachable.insert(cliapp::path_key(node.path));
  }
  std::vector<std::string> dead;
  for (auto const& [key, unused] : table) {
    if (!reachable.contains(key)) {
      dead.push_back(key);
    }
  }
  return dead;
}

auto run(context& ctx, CLI::App& root, const handler_table& table) -> int {
  return run_detailed(ctx, root, table).code;
}

auto run_detailed(context& ctx, CLI::App& root, const handler_table& table) -> run_outcome {
  // The worktree gate fires BEFORE the parser, which is the oracle's
  // ordering and is deliberate: a planning verb run from a worktree must
  // exit 8 even when its arguments also fail to parse. See
  // `planar.cmd.planar.worktree_gate`'s header.
  if (auto const refused = worktree_gate::check(ctx, root)) {
    // NOT loggable — see `run_outcome::loggable`.
    return run_outcome{.code = *refused, .kind = std::nullopt, .loggable = false};
  }

  auto const argv = cliapp::hoist_subcommands(root, ctx.argv());
  // CLI11's vector overload consumes argv[1..] in REVERSE order and never
  // sees argv[0] (see CLI::App::parse_char_t, which builds exactly this).
  std::vector<std::string> reversed;
  if (argv.size() > 1) {
    reversed.assign(argv.rbegin(), argv.rend() - 1);
  }

  try {
    root.parse(std::move(reversed));
  } catch (const CLI::CallForHelp&) {
    auto const [node, unused] = matched_node(root);
    ctx.out() << node->help();
    return run_outcome{.code = exit_success};
  } catch (const CLI::ParseError& e) {
    // Decision 1004 (task 6271): every parse failure writes ONE line to
    // stderr, the same shape a handler refusal already writes — see this
    // module's header. The CamelCase tag (`e.get_name()`) is dropped
    // rather than kept as a second stderr line: keeping it would leave
    // parse failures as the only two-line refusal shape in the binary,
    // which is exactly the asymmetry this decision closes. Nothing is
    // written to stdout on a parse failure.
    ctx.err() << "error: " << e.what() << '\n';
    // This binary's policy is exit 1, NOT the operator binary's 2.
    return run_outcome{.code = exit_code_for(domain_error_kind::parse_error), .kind = domain_error_kind::parse_error};
  }

  auto const [node, path] = matched_node(root);
  if (!cliapp::children(*node).empty() && !table.contains(cliapp::path_key(path))) {
    // A group named without a leaf beneath it AND with no handler of its
    // own — including a bare `planar`, since this tree has no cockpit to
    // route to. A group that DOES have a handler is dual (`handoff`,
    // `resume`) and falls through to it; see this module's header.
    ctx.out() << node->help();
    return run_outcome{.code = exit_success};
  }

  auto       args  = cliapp::harvest(root);
  auto const key   = cliapp::path_key(args.path);
  auto const found = table.find(key);
  // Task 6844 (decision 1145, supersedes D5): the --json error envelope is
  // additive on stdout -- the same stream a successful handler's JSON
  // output already uses -- gated on the SAME flag. report()'s pinned
  // `error: <verb>: <Tag>` text stays on stderr, unchanged. Checked once
  // here rather than per report() call site.
  auto const want_json_envelope = cliapp::flag_bool(args, "--json");
  if (found == table.end()) {
    auto const err = error_from_body(domain_error_kind::not_implemented, "not implemented yet");
    report(err, ctx.err());
    if (want_json_envelope) {
      report_json_envelope(key, err, ctx.out());
    }
    return run_outcome{.code = exit_code(err), .kind = err.kind};
  }

  // Task 6903: detect "the handler already wrote a JSON payload to
  // stdout" HERE, once, rather than special-casing `audit
  // handoff-readiness --json` and `templates validate --json`
  // individually. One JSON document per stream is the contract a
  // `json.loads` consumer relies on; a handler that already emitted its
  // own body must not ALSO get the additive error envelope appended.
  auto [outcome, wrote_stdout] = run_tracking_stdout_writes(ctx.out(), [&] { return found->second(ctx, args); });
  if (!outcome) {
    report(outcome.error(), ctx.err());
    if (want_json_envelope && !wrote_stdout) {
      report_json_envelope(key, outcome.error(), ctx.out());
    }
    return run_outcome{.code = exit_code(outcome.error()), .kind = outcome.error().kind};
  }
  return run_outcome{.code = exit_success};
}

} // namespace planar::cmd
