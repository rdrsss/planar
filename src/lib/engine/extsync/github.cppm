/// @file github.cppm
/// @brief `planar.engine.extsync.github` — the GitHub Issues
/// operational-plane adapter (plan 996, task 6041).
///
/// Behavior-preserving port (D2) of the four-operation core of
/// `zig/src/engine/extsync/github.zig` (a 1240-line file): `init`,
/// `validate`, `pull`, `push`, `render`, plus `parseExternalID`,
/// `stateForStatus`, `labelsForStatus`, `statusToLocal`, `rawStatus` and
/// `parseIssue`, PLUS (plan 1009, task 6408) `createIssue`,
/// `buildCreateIssueBody`, `parseCreatedIssue`, `linkSubIssue` and
/// `linkSubIssueProbe` — the four production methods `ext propagate`'s
/// parent/child hierarchy needs. `createSubIssue` is NOT ported: it is a
/// thin `createIssue` + `linkSubIssue` fusion with no test of its own beyond
/// the two it calls, and `planar.engine.external.parent_issue`'s `gh_client`
/// seam (task 6353) composes the two calls itself rather than needing a
/// fused helper — see that module's header.
///
/// ## What is NOT ported, and with which verb it is deferred
///
/// Roughly 460 of that file's remaining lines implement a surface no verb in
/// this cycle reaches, and every one of them is deferred WITH its verb rather
/// than speculatively:
///
///   - The entire ProjectsV2 GraphQL arm (`getAuthenticatedOwner`,
///     `createProjectV2`, `addProjectV2Item`, `setProjectV2ItemFieldValue`,
///     `getProjectV2Fields`, `graphqlDo`) — `ext propagate --github-strategy
///     projects-v2`. It is also why `github.Error` declares a `GraphqlError`
///     tag that `planar.adapter`'s unified error set does not: no ported
///     operation can produce it.
///
/// ## Status is a THREE-WAY mapping and each direction is separate
///
/// GitHub has no status field — it has `state` (open/closed) plus labels.
/// Three distinct functions cross that gap and none of them is the inverse of
/// another:
///
///   - `state_for_status` (push): local -> `{state, labels?}`. `doing` and
///     `blocked` are BOTH `open` and differ only by label; `done` and
///     `cancelled` are both `closed`.
///   - `status_to_local` (pull): `{state, labels}` -> local. `closed` +
///     `wontfix` is `cancelled`, any other `closed` is `done`; for `open` the
///     FIRST MATCHING LABEL IN ARRAY ORDER decides, else `todo`. Array order,
///     not check order: the Zig loop tests `in-progress` and then `blocked`
///     inside EACH label's iteration, so `["blocked","in-progress"]` reads as
///     `blocked`. Reading the check order as the precedence order inverts this
///     — a first draft of github.t.cpp asserted `doing` here and the test
///     caught it.
///   - `raw_status` (pull): `{state, labels}` -> the colon-joined provider
///     string the operator sees (`open:in-progress`, `closed:wontfix`).
///
/// An unrecognized local status pushes as plain `open` — it does NOT fail.
///
/// ## `post_comment`, landed at plan 996 task 6339
///
/// A fifth, adapter-specific method (not part of `external_adapter`),
/// exactly like `jira_adapter::post_comment` — see that class's header for
/// why. Its only ported caller is `audit publish-decision`, reached through
/// `adapter_handle::post_comment`. Unlike `validate`, this does NOT call
/// `validate()` as a named step — it inlines the same `parse_external_id`
/// check the Zig original does, with the same effect.
///
/// ## `push` overwrites the whole label set
///
/// The `labels` array a status push emits REPLACES every label on the issue,
/// because that is what GitHub's PATCH endpoint does with a `labels` key.
/// The Zig original does this and it is preserved; it is a real, documented
/// behavior of this adapter rather than an oversight.
module;

export module planar.engine.extsync.github;

import std;
import planar.adapter;
import planar.http;

namespace planar::engine::extsync::github {

/// @brief The API root used when the registered system carries no base URL.
export inline constexpr std::string_view k_api_base_default = "https://api.github.com";

/// @brief The `{number, node_id}` pair `POST /repos/{o}/{r}/issues` returns.
///
/// Port of the Zig `GithubAdapter.CreatedIssue`. `node_id` is the GraphQL
/// node id the (unported) ProjectsV2 arm would need; it defaults to empty
/// when the response omits it, matching `parseCreatedIssue`'s tolerance —
/// see that function's port, `parse_created_issue`, below.
export struct created_issue {
  std::int64_t number = 0; ///< The issue number within its repo.
  std::string  node_id;    ///< The GraphQL node id; may be empty.
};

/// @brief The GitHub Issues adapter.
///
/// Holds a reference to the transport, not ownership — see `jira_adapter` for
/// the same argument.
export class github_adapter final : public adapter::external_adapter {
private:
  std::string              _base_url;
  adapter::auth_credential _cred;
  http::transport*         _transport;

public:
  /// @brief Construct an adapter.
  /// @param base_url The API root. An EMPTY view becomes
  /// `k_api_base_default`; one trailing slash is trimmed.
  /// @param cred The credential every request presents.
  /// @param wire The transport to send through; must outlive this adapter.
  github_adapter(std::string_view base_url, adapter::auth_credential cred, http::transport& wire);

  /// @brief Accept an `owner/repo#number` external id.
  ///
  /// The `#` is found with a REVERSE search, so an owner or repo containing
  /// `#` still splits at the last one. The number must parse and be > 0.
  /// @param external_id The candidate id.
  /// @return Success, or `adapter_error::invalid_external_id`.
  [[nodiscard]] auto validate(std::string_view external_id) const -> std::expected<void, adapter::adapter_error> override;

  /// @brief `GET {base}/repos/{owner}/{repo}/issues/{number}`.
  ///
  /// 404 is `not_found`; anything but 200 or 404 is `unexpected_status`.
  /// @param external_id The issue id.
  /// @return The normalized state, or the failure.
  [[nodiscard]] auto pull(std::string_view external_id) const
      -> std::expected<adapter::remote_state, adapter::adapter_error> override;

  /// @brief `PATCH {base}/repos/{owner}/{repo}/issues/{number}`.
  ///
  /// Sends nothing when the change set names none of title/status/assignee.
  /// Expects 200. See this module's header about the label set.
  /// @param external_id The issue id.
  /// @param fields The requested changes.
  /// @return What was applied, or the failure.
  [[nodiscard]] auto push(std::string_view external_id, const adapter::field_change_set& fields) const
      -> std::expected<adapter::update_outcome, adapter::adapter_error> override;

  /// @brief Render the `POST /issues` creation payload for `local`.
  ///
  /// An empty body becomes `(no description)`; the `labels` array appears
  /// only for a status that has one. `create_options` is IGNORED — the Zig
  /// original discards it (`_: extsync.CreateOptions`), because the repo is
  /// already encoded in the URL rather than in the payload.
  /// @param local The local entity.
  /// @param opts Ignored; present for the interface.
  /// @return The JSON payload.
  [[nodiscard]] auto render(const adapter::local_entity& local, const adapter::create_options& opts) const
      -> std::expected<std::string, adapter::adapter_error> override;

  /// @brief `POST {base}/repos/{owner}/{repo}/issues/{number}/comments`.
  ///
  /// Parses `external_id` first (the same check `validate` runs), so a
  /// malformed id never reaches the transport. Accepts 200 or 201; anything
  /// else is `unexpected_status`. Always issues exactly one request — no
  /// "already posted" check, matching the Zig original.
  /// @param external_id The `owner/repo#number` id.
  /// @param body The comment body.
  /// @return Success, or the failure.
  [[nodiscard]] auto post_comment(std::string_view external_id, std::string_view body) const
      -> std::expected<void, adapter::adapter_error>;

  /// @brief `POST {base}/repos/{owner}/{repo}/issues` — create a regular
  /// issue (plan 1009, task 6408).
  ///
  /// The `labels` array is emitted only when non-empty. Expects 201;
  /// anything else is `unexpected_status`. On success, parses `{number,
  /// node_id}` out of the response body — `node_id` defaults to empty
  /// rather than failing when the field is absent (needed for ProjectsV2
  /// wiring this cycle does not reach, so a payload lacking it must not
  /// break sub-issue creation).
  /// @param owner The repo owner.
  /// @param repo The repo name.
  /// @param title The issue title.
  /// @param body The issue body.
  /// @param labels Labels to attach on create.
  /// @return The created issue's `{number, node_id}`, or the failure.
  [[nodiscard]] auto create_issue(std::string_view owner, std::string_view repo, std::string_view title,
                                  std::string_view body, std::span<const std::string> labels) const
      -> std::expected<created_issue, adapter::adapter_error>;

  /// @brief `POST {base}/repos/{owner}/{repo}/issues/{parent}/sub_issues` —
  /// parent `child_number` under `parent_number` (plan 1009, task 6408).
  ///
  /// A 404 response is `not_found` — the caller uses this to detect
  /// "sub-issue endpoint unavailable on this account". Any other non-
  /// (200|201) status is `unexpected_status`.
  /// @param owner The repo owner.
  /// @param repo The repo name.
  /// @param parent_number The parent issue number.
  /// @param child_number The child issue number.
  /// @return Success, or the failure.
  [[nodiscard]] auto link_sub_issue(std::string_view owner, std::string_view repo, std::int64_t parent_number,
                                    std::int64_t child_number) const -> std::expected<void, adapter::adapter_error>;

  /// @brief Probe whether the sub-issue REST endpoint is enabled for
  /// `owner/repo`, without creating any real relation (plan 1009, task
  /// 6408).
  ///
  /// `POST {base}/repos/{owner}/{repo}/issues/0/sub_issues` with
  /// `{"sub_issue_id":0}`. A 404 is `not_found` (endpoint not enabled for
  /// the account). ANY OTHER status — including a 422 the bogus issue id
  /// 0 provokes — is treated as SUCCESS: the endpoint exists, it just
  /// rejected this particular bogus payload. That asymmetry is the Zig
  /// oracle's own behavior (`linkSubIssueProbe`), preserved rather than
  /// tightened.
  /// @param owner The repo owner.
  /// @param repo The repo name.
  /// @return Success when the endpoint is enabled, or `not_found`
  /// otherwise (or a transport-level failure).
  [[nodiscard]] auto link_sub_issue_probe(std::string_view owner, std::string_view repo) const
      -> std::expected<void, adapter::adapter_error>;
};

/// @brief The `{state, labels}` pair a local status pushes as.
export struct status_mapping {
  std::string_view              state;  ///< `open` or `closed`.
  std::vector<std::string_view> labels; ///< The labels to SET; empty means the payload omits the key entirely.
};

/// @brief Map a local status to GitHub's `state` plus labels (push side).
/// @param status The local status.
/// @return The mapping; unrecognized statuses map to plain `open`.
export auto state_for_status(std::string_view status) -> status_mapping;

/// @brief Map GitHub's `state` plus labels to a local status (pull side).
/// @param state The issue state.
/// @param labels The issue's label names.
/// @return The local status.
export auto status_to_local(std::string_view state, std::span<const std::string> labels) -> std::string_view;

/// @brief The operator-facing provider status string (pull side).
/// @param state The issue state.
/// @param labels The issue's label names.
/// @return `open`, `open:in-progress`, `open:blocked`, `closed` or
/// `closed:wontfix`.
export auto raw_status(std::string_view state, std::span<const std::string> labels) -> std::string_view;

} // namespace planar::engine::extsync::github
