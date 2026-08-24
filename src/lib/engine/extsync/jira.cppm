/// @file jira.cppm
/// @brief `planar.engine.extsync.jira` — the Jira operational-plane adapter
/// (plan 996, task 6041).
///
/// Behavior-preserving port (D2) of the four-operation core of
/// `zig/src/engine/extsync/jira.zig`: `init`, `validate`, `pull`, `push`,
/// `render`, and the `parseIssue` / `extractADFText` / `mapStatus` /
/// `isValidIssueKey` helpers behind them.
///
/// ## What is NOT ported, and with which verb it is deferred
///
///   - `postComment` — the Atlassian-Document-Format comment POST. It is not
///     part of the four-operation adapter interface; its only caller is
///     `ext propagate`, which is deferred (see the bucket's CMakeLists.txt).
///     Deferred WITH that verb rather than speculatively.
///
/// Everything else in jira.zig is here.
///
/// ## The three field mappings that are easy to get subtly wrong
///
///   1. **A status-only push sends NOTHING.** Jira status changes go through
///      `/transitions`, not through the issue-update endpoint, so the Zig
///      original discards `fields.status` (`_ = fields.status;`) and, if that
///      was the only field requested, returns an empty `UpdateOutcome`
///      WITHOUT issuing a request. `push` here does the same, and its test
///      asserts the request count, not just the return value — a version that
///      sent an empty `{"fields":{}}` PUT would return the same outcome.
///   2. **`assignee` reads `emailAddress` first, `accountId` second.** Both
///      may be present; the Zig original prefers the email. Reversing the
///      preference silently changes what every conflict evidence blob records.
///   3. **`priority` is the `id` STRING parsed as an integer**, not the
///      `name`, and an unparseable id is 0 rather than a failure.
///
/// ## `version` is the conflict engine's whole safety story
///
/// `remote_state::version` is Jira's `fields.updated`. `resolve_conflict`
/// refuses to authorize either side when it is empty (see
/// `planar.engine.external.sync`), so an adapter that dropped it would turn
/// every guarded resolution into an unguarded one. It is read as a plain
/// string field with no interpretation.
module;

export module planar.engine.extsync.jira;

import std;
import planar.adapter;
import planar.http;

namespace planar::engine::extsync::jira {

/// @brief The Jira adapter.
///
/// Holds a reference to the transport, not ownership: the caller owns the
/// transport and outlives the adapter. That is what lets a test swap a
/// recording stub or an in-process fixture server in without the adapter
/// knowing.
export class jira_adapter final : public adapter::external_adapter {
private:
  std::string              _base_url;
  adapter::auth_credential _cred;
  http::transport*         _transport;

public:
  /// @brief Construct an adapter.
  /// @param base_url The Jira site root (e.g. `https://acme.atlassian.net`).
  /// One trailing slash is trimmed.
  /// @param cred The credential every request presents.
  /// @param wire The transport to send through; must outlive this adapter.
  jira_adapter(std::string_view base_url, adapter::auth_credential cred, http::transport& wire);

  /// @brief Accept a canonical Jira issue key (`PROJ-123`).
  ///
  /// Purely syntactic: uppercase letters, digits and `_` before the LAST
  /// leading dash position, digits only after it, and neither side empty.
  /// Lowercase is rejected (`proj-123` is not a key).
  /// @param external_id The candidate key.
  /// @return Success, or `adapter_error::invalid_external_id`.
  [[nodiscard]] auto validate(std::string_view external_id) const -> std::expected<void, adapter::adapter_error> override;

  /// @brief `GET {base}/rest/api/3/issue/{key}` and normalize the response.
  ///
  /// 404 is `not_found`; any status other than 200 or 404 is
  /// `unexpected_status`; a body that is not a JSON object with a `fields`
  /// object is `parse_failed`.
  /// @param external_id The issue key.
  /// @return The normalized state, or the failure.
  [[nodiscard]] auto pull(std::string_view external_id) const
      -> std::expected<adapter::remote_state, adapter::adapter_error> override;

  /// @brief `PUT {base}/rest/api/3/issue/{key}` with the requested fields.
  ///
  /// Sends nothing at all when the change set names no field this endpoint
  /// can carry (see this module's header, point 1). Expects 204.
  /// @param external_id The issue key.
  /// @param fields The requested changes.
  /// @return What was applied, or the failure.
  [[nodiscard]] auto push(std::string_view external_id, const adapter::field_change_set& fields) const
      -> std::expected<adapter::update_outcome, adapter::adapter_error> override;

  /// @brief Render the `POST /issue` creation payload for `local`.
  ///
  /// Issue type defaults to `Story`; an empty body becomes the literal
  /// `(no description)`; the `project` key is omitted entirely when unset.
  /// @param local The local entity.
  /// @param opts The creation options.
  /// @return The JSON payload.
  [[nodiscard]] auto render(const adapter::local_entity& local, const adapter::create_options& opts) const
      -> std::expected<std::string, adapter::adapter_error> override;
};

/// @brief Map a Jira status name to Planar's status vocabulary.
///
/// Exposed because it is a table, not an implementation detail: the six
/// recognized names are a contract with the operator, and an unrecognized
/// name maps to the EMPTY string, which the sync engine reads as "remote has
/// no opinion about status" rather than as a status change.
/// @param raw The Jira status name.
/// @return The local status, or empty when unmappable.
export auto map_status(std::string_view raw) -> std::string_view;

} // namespace planar::engine::extsync::jira
