# planar_engine_extsync — the external-plane adapters (plan 996, task 6041).
#
# Layer-2 `engine_*` module (cmake/architecture.cmake): depends on layer-1
# `adapter`, `http` and `json_text`, and on NO other `engine_*` (D18's
# same-layer prohibition). It notably does NOT depend on `db` — an adapter
# talks to a provider, never to SQLite, and keeping the edge out is what makes
# every test in this bucket runnable with no database at all.
#
# ## Two adapters, not one
#
# The task brief's advice was "landing one adapter properly beats two
# half-done", and the cut here is against the FEATURE surface rather than
# against the adapter list: each of Jira and GitHub Issues gets its complete
# four-operation core (`validate`/`pull`/`push`/`render`) and neither gets any
# of the create/propagate surface. Both are landed because the four-operation
# core is the same shape in both files and because the two are not
# substitutable — the Zig conflict-lifecycle integration test drives JIRA
# (`ext_sync_test.zig`), while `ext register github` / `ext test` (the two
# `ext` leaves that land at layer 3 this cycle) drive GITHUB. Landing only one
# would leave a live verb pointing at a missing adapter.
#
# ## What is deferred, and with which verb
#
# Each deferral is recorded against the verb that needs it, never
# speculatively:
#
#   - jira: `postComment` — `ext propagate`.
#   - github: `createIssue`/`buildCreateIssueBody`/`parseCreatedIssue` —
#     `ext create`, `ext propagate`.
#   - github: `linkSubIssue`/`createSubIssue`/`linkSubIssueProbe` —
#     `ext propagate`'s parent/child hierarchy.
#   - github: `postComment` — `ext propagate`'s decision comments.
#   - github: the whole ProjectsV2 GraphQL arm (`getAuthenticatedOwner`,
#     `createProjectV2`, `addProjectV2Item`, `setProjectV2ItemFieldValue`,
#     `getProjectV2Fields`, `graphqlDo`) — `ext propagate --github-strategy
#     projects-v2`. This is also why `planar.adapter`'s unified error set
#     carries no `GraphqlError`: no ported operation can produce one.
#   - `zig/src/engine/extsync/strategy.zig` and `projects_v2.zig` entirely —
#     the `ext propagate` verb family. `parent_issue.zig` is GONE from this
#     list: task 6353 landed the whole of its orchestration (everything
#     except the two exports that had already left at task 6335) in
#     `planar.engine.external.parent_issue`, NOT in this bucket — see that
#     module's header for why a file this SQL-heavy could not join
#     `engine_extsync` without breaking its no-`db` invariant. Its `gh_client`
#     seam is type-erased the same way `sync.cppm` erases `external_adapter`,
#     so this module is still never named by it. What still blocks `ext
#     propagate` from that file: NOTHING in `parent_issue.zig` itself. What
#     blocks it from THIS bucket: `github_adapter` has not grown
#     `createIssue`/`linkSubIssue`/`linkSubIssueProbe`/`postComment` (still
#     true, see below), so no production `gh_client` exists to wire the
#     handler to yet — that adapter work plus the handler wiring is
#     follow-up, tracked separately from parent_issue.zig's own port.
#   - `propagate.zig` PARTIALLY. Task 6335 measured this file by symbol rather
#     than by name and found it divides cleanly: `ext propagate-one` reaches
#     only `strategyForSystem` and `loadExistingMirror`, and neither reaches
#     anything else in it. Those two landed — the pure one HERE as
#     `propagate.cppm`, the SQL one in `planar.engine.external.link` as
#     `load_existing_mirror`, because this bucket has no `db` edge. The
#     remaining five functions (`selectStrategy`,
#     `countDistinctReposInFeature`, `walkTree`, `freeTree`,
#     `hasExistingMirrorLink`) plus six declarations with no caller anywhere
#     stay with `ext propagate`. `walkTree` had already left this file for
#     `planar.engine.planning.descendants` at task 6298 on the same reasoning.
#   - `parent_issue.zig`'s `recordLink` landed at task 6335, in
#     `planar.engine.external.link` as `record_mirror_link` — it was the only
#     one of that file's ~20 exports `workbench publish` needed at the time,
#     and it is 36 lines of SQL with no adapter, transport or credential
#     edge. Task 6353 landed everything else that file exports EXCEPT
#     `projects_v2.zig`'s own reuse of its `entityForCreate` (a separate,
#     smaller port that file's own coder will re-measure rather than assume
#     — see `planar.engine.external.parent_issue`'s header for what task
#     6353 measured this file at: ~1037 oracle implementation lines,
#     excluding oracle `test` blocks, once counted brace-balanced rather
#     than by whole-file `wc -l`).
#
# ## The tests do not touch the network, at two levels
#
# `jira.t.cpp` / `github.t.cpp` use a RECORDING TRANSPORT — an in-process
# `planar::http::transport` subclass that opens no socket — because what they
# pin is URL shapes, header sets, body encodings and status handling, and a
# real socket would only add flakiness to that.
#
# The REAL round trip is proven one layer down, in `src/lib/http/http.t.cpp`,
# against `fixture_server.hpp`'s in-process HTTP/1.1 server bound to
# 127.0.0.1 on a kernel-assigned port, and once more HERE in
# `jira.t.cpp`'s "over a real loopback HTTP round trip" case, which drives the
# Jira adapter through an actual `curl_transport` against that same fixture
# server. That case is what makes the statement "these adapters work over
# HTTP", which a recording stub cannot make. See fixture_server.hpp's header.
#
# STDOUT TERMINATOR CONTRACT: not applicable — this bucket renders no operator
# output. The JSON it builds is a provider REQUEST BODY, never terminated with
# a newline, and `render`'s `@return` documents it as a payload rather than as
# a rendered line.

planar_module(engine_extsync
  INTERFACE
    support.cppm
    jira.cppm
    github.cppm
    propagate.cppm
    extsync.cppm
  SOURCES
    support.cpp
    jira.cpp
    github.cpp
    propagate.cpp
  DEPENDS adapter http json_text)

target_link_libraries(planar_engine_extsync PRIVATE glaze::glaze)

if(TARGET planar_engine_extsync_tests)
  target_link_libraries(planar_engine_extsync_tests PRIVATE glaze::glaze CURL::libcurl)
endif()
