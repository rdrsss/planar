/// @file github.cpp
/// @brief Implementation of `planar.engine.extsync.github`. See github.cppm
/// for the port scope, the five deferred surfaces, and the three-way status
/// mapping.

module;

#include "json_read.hpp"

module planar.engine.extsync.github;

import std;
import planar.adapter;
import planar.http;
import planar.json_text;
import planar.engine.extsync.support;

namespace planar::engine::extsync::github {

namespace {

using adapter::adapter_error;
using json_read::array_field;
using json_read::number_field;
using json_read::object_field;
using json_read::string_field;

/// @brief The three parts of an `owner/repo#number` external id.
struct parsed_external_id {
  std::string_view owner;      ///< The repository owner.
  std::string_view repo;       ///< The repository name.
  std::int64_t     number = 0; ///< The issue number.
};

/// @brief Split an `owner/repo#number` external id.
///
/// Port of `parseExternalID`. The `#` search is a REVERSE search
/// (`lastIndexOfScalar`) and the `/` search inside the path is a FORWARD one,
/// so `a/b/c#1` yields owner `a`, repo `b/c`.
/// @param external_id The candidate id.
/// @return The parts, or unset when malformed.
auto parse_external_id(std::string_view external_id) -> std::optional<parsed_external_id> {
  auto const hash = external_id.rfind('#');
  if (hash == std::string_view::npos || hash == 0 || hash + 1 >= external_id.size()) {
    return std::nullopt;
  }
  auto const path    = external_id.substr(0, hash);
  auto const num_str = external_id.substr(hash + 1);
  auto const slash   = path.find('/');
  if (slash == std::string_view::npos || slash == 0 || slash + 1 >= path.size()) {
    return std::nullopt;
  }
  std::int64_t number = 0;
  auto const   result = std::from_chars(num_str.data(), num_str.data() + num_str.size(), number);
  if (result.ec != std::errc{} || result.ptr != num_str.data() + num_str.size() || number <= 0) {
    return std::nullopt;
  }
  return parsed_external_id{.owner = path.substr(0, slash), .repo = path.substr(slash + 1), .number = number};
}

/// @brief The labels a status renders with on the CREATE path.
///
/// Distinct from `state_for_status`: `render` emits labels but no `state`
/// (a new issue is always open), so `todo` and `done` both produce no labels
/// key at all.
/// @param status The local status.
/// @return The labels, empty when the status has none.
auto labels_for_status(std::string_view status) -> std::vector<std::string_view> {
  if (status == "doing") {
    return {"in-progress"};
  }
  if (status == "blocked") {
    return {"blocked"};
  }
  if (status == "cancelled") {
    return {"wontfix"};
  }
  return {};
}

/// @brief Append `,"labels":[...]` to `out` when `labels` is non-empty.
/// @param out The buffer to append to.
/// @param labels The label names.
auto append_labels(std::string& out, std::span<const std::string_view> labels) -> void {
  if (labels.empty()) {
    return;
  }
  out += R"(,"labels":[)";
  for (std::size_t i = 0; i < labels.size(); ++i) {
    if (i != 0) {
      out += ",";
    }
    json_text::append_json_string(out, labels[i]);
  }
  out += "]";
}

/// @brief Parse a GitHub issue response into a `remote_state`.
/// @param external_id Echoed back as the state's `external_id`.
/// @param raw The response body.
/// @return The normalized state, or `adapter_error::parse_failed`.
auto parse_issue(std::string_view external_id, std::string_view raw) -> std::expected<adapter::remote_state, adapter_error> {
  auto parsed = glz::read_json<glz::generic>(raw);
  if (!parsed || !parsed->is_object()) {
    return std::unexpected(adapter_error::parse_failed);
  }
  glz::generic const& root = *parsed;

  std::string assignee;
  if (auto const* const assignee_obj = object_field(root, "assignee")) {
    assignee = string_field(*assignee_obj, "login").value_or(std::string{});
  }

  std::vector<std::string> labels;
  if (auto const* const label_array = array_field(root, "labels")) {
    for (auto const& item : *label_array) {
      if (auto const name = string_field(item, "name")) {
        labels.push_back(*name);
      }
    }
  }

  // `state` defaults to "open" when absent, matching the Zig original — an
  // issue payload without a state is treated as open rather than as a parse
  // failure.
  auto const state = string_field(root, "state").value_or(std::string("open"));

  return adapter::remote_state{
      .external_id = std::string(external_id),
      .title       = string_field(root, "title").value_or(std::string{}),
      .body        = string_field(root, "body").value_or(std::string{}),
      .status      = std::string(status_to_local(state, labels)),
      .assignee    = std::move(assignee),
      // GitHub Issues carries no priority or due date; the Zig original hard-
      // codes both rather than inventing a mapping.
      .priority   = 0,
      .due_at     = {},
      .url        = string_field(root, "html_url").value_or(std::string{}),
      .raw_status = std::string(raw_status(state, labels)),
      .version    = string_field(root, "updated_at").value_or(std::string{}),
  };
}

/// @brief Render the `POST /repos/{o}/{r}/issues` creation body.
///
/// Port of `buildCreateIssueBody`. Labels are emitted as a JSON array only
/// when non-empty, matching `render`'s `append_labels` reuse of the same
/// shape.
/// @param title The issue title.
/// @param body The issue body.
/// @param labels Labels to attach; omitted from the payload when empty.
/// @return The JSON payload.
auto build_create_issue_body(std::string_view title, std::string_view body, std::span<const std::string> labels)
    -> std::string {
  std::string out = R"({"title":)";
  json_text::append_json_string(out, title);
  out += R"(,"body":)";
  json_text::append_json_string(out, body);
  if (!labels.empty()) {
    out += R"(,"labels":[)";
    for (std::size_t i = 0; i < labels.size(); ++i) {
      if (i != 0) {
        out += ",";
      }
      json_text::append_json_string(out, labels[i]);
    }
    out += "]";
  }
  out += "}";
  return out;
}

/// @brief Parse `{number, node_id}` out of an issue-create response body.
///
/// Port of `parseCreatedIssue`. `number` missing or not a JSON number is
/// `parse_failed`; `node_id` missing or not a string defaults to empty
/// rather than failing — the Zig original's own tolerance, kept because a
/// payload without `node_id` still carries everything sub-issue linking
/// needs.
/// @param raw The response body.
/// @return The created issue, or `adapter_error::parse_failed`.
auto parse_created_issue(std::string_view raw) -> std::expected<created_issue, adapter_error> {
  auto parsed = glz::read_json<glz::generic>(raw);
  if (!parsed || !parsed->is_object()) {
    return std::unexpected(adapter_error::parse_failed);
  }
  glz::generic const& root = *parsed;
  auto const           number = number_field(root, "number");
  if (!number.has_value()) {
    return std::unexpected(adapter_error::parse_failed);
  }
  return created_issue{.number = *number, .node_id = string_field(root, "node_id").value_or(std::string{})};
}

} // namespace

auto state_for_status(std::string_view status) -> status_mapping {
  if (status == "todo") {
    return {.state = "open", .labels = {}};
  }
  if (status == "doing") {
    return {.state = "open", .labels = {"in-progress"}};
  }
  if (status == "blocked") {
    return {.state = "open", .labels = {"blocked"}};
  }
  if (status == "done") {
    return {.state = "closed", .labels = {}};
  }
  if (status == "cancelled") {
    return {.state = "closed", .labels = {"wontfix"}};
  }
  return {.state = "open", .labels = {}};
}

auto status_to_local(std::string_view state, std::span<const std::string> labels) -> std::string_view {
  if (state == "closed") {
    for (auto const& label : labels) {
      if (label == "wontfix") {
        return "cancelled";
      }
    }
    return "done";
  }
  // Order matters: an issue carrying BOTH `in-progress` and `blocked` reads
  // as `doing`, because the Zig original tests `in-progress` first inside the
  // same loop iteration.
  for (auto const& label : labels) {
    if (label == "in-progress") {
      return "doing";
    }
    if (label == "blocked") {
      return "blocked";
    }
  }
  return "todo";
}

auto raw_status(std::string_view state, std::span<const std::string> labels) -> std::string_view {
  if (state == "closed") {
    for (auto const& label : labels) {
      if (label == "wontfix") {
        return "closed:wontfix";
      }
    }
    return "closed";
  }
  for (auto const& label : labels) {
    if (label == "in-progress") {
      return "open:in-progress";
    }
    if (label == "blocked") {
      return "open:blocked";
    }
  }
  return "open";
}

github_adapter::github_adapter(std::string_view base_url, adapter::auth_credential cred, http::transport& wire)
    : _base_url(support::trim_trailing_slash(base_url.empty() ? k_api_base_default : base_url)), _cred(std::move(cred)),
      _transport(&wire) {
}

auto github_adapter::validate(std::string_view external_id) const -> std::expected<void, adapter_error> {
  if (!parse_external_id(external_id).has_value()) {
    return std::unexpected(adapter_error::invalid_external_id);
  }
  return {};
}

auto github_adapter::pull(std::string_view external_id) const -> std::expected<adapter::remote_state, adapter_error> {
  auto const parts = parse_external_id(external_id);
  if (!parts.has_value()) {
    return std::unexpected(adapter_error::invalid_external_id);
  }
  auto const auth = support::auth_header(_cred);
  if (!auth) {
    return std::unexpected(auth.error());
  }
  auto const sent = _transport->send({
      .verb    = http::method::get,
      .url     = std::format("{}/repos/{}/{}/issues/{}", _base_url, parts->owner, parts->repo, parts->number),
      .headers = {{.name = "Accept", .value = "application/vnd.github+json"}, {.name = "Authorization", .value = *auth}},
  });
  if (!sent) {
    return std::unexpected(adapter_error::transport_failed);
  }
  if (sent->status == 404) {
    return std::unexpected(adapter_error::not_found);
  }
  if (sent->status != 200) {
    return std::unexpected(adapter_error::unexpected_status);
  }
  return parse_issue(external_id, sent->body);
}

auto github_adapter::push(std::string_view external_id, const adapter::field_change_set& fields) const
    -> std::expected<adapter::update_outcome, adapter_error> {
  auto const parts = parse_external_id(external_id);
  if (!parts.has_value()) {
    return std::unexpected(adapter_error::invalid_external_id);
  }

  adapter::update_outcome outcome;
  std::string             body      = "{";
  bool                    has_field = false;

  if (fields.title.has_value()) {
    has_field = true;
    body += R"("title":)";
    json_text::append_json_string(body, *fields.title);
    outcome.fields_applied.emplace_back("title");
  }
  if (fields.status.has_value()) {
    auto const mapping = state_for_status(*fields.status);
    if (has_field) {
      body += ",";
    }
    has_field = true;
    body += R"("state":)";
    json_text::append_json_string(body, mapping.state);
    // Replaces the issue's ENTIRE label set — see github.cppm's header.
    append_labels(body, mapping.labels);
    outcome.fields_applied.emplace_back("status");
  }
  if (fields.assignee.has_value()) {
    if (has_field) {
      body += ",";
    }
    has_field = true;
    body += R"("assignees":[)";
    json_text::append_json_string(body, *fields.assignee);
    body += "]";
    outcome.fields_applied.emplace_back("assignee");
  }
  body += "}";

  if (!has_field) {
    return adapter::update_outcome{};
  }

  auto const auth = support::auth_header(_cred);
  if (!auth) {
    return std::unexpected(auth.error());
  }
  auto const sent = _transport->send({
      .verb    = http::method::patch,
      .url     = std::format("{}/repos/{}/{}/issues/{}", _base_url, parts->owner, parts->repo, parts->number),
      .headers = {{.name = "Content-Type", .value = "application/json"},
                  {.name = "Accept", .value = "application/vnd.github+json"},
                  {.name = "Authorization", .value = *auth}},
      .body    = body,
  });
  if (!sent) {
    return std::unexpected(adapter_error::transport_failed);
  }
  if (sent->status != 200) {
    return std::unexpected(adapter_error::unexpected_status);
  }
  return outcome;
}

auto github_adapter::post_comment(std::string_view external_id, std::string_view body) const
    -> std::expected<void, adapter_error> {
  auto const parts = parse_external_id(external_id);
  if (!parts.has_value()) {
    return std::unexpected(adapter_error::invalid_external_id);
  }
  auto const url = std::format("{}/repos/{}/{}/issues/{}/comments", _base_url, parts->owner, parts->repo, parts->number);

  std::string payload = R"({"body":)";
  json_text::append_json_string(payload, body);
  payload += "}";

  auto const auth = support::auth_header(_cred);
  if (!auth) {
    return std::unexpected(auth.error());
  }
  auto const sent = _transport->send({
      .verb    = http::method::post,
      .url     = url,
      .headers = {{.name = "Content-Type", .value = "application/json"},
                  {.name = "Accept", .value = "application/vnd.github+json"},
                  {.name = "Authorization", .value = *auth}},
      .body    = payload,
  });
  if (!sent) {
    return std::unexpected(adapter_error::transport_failed);
  }
  if (sent->status != 201 && sent->status != 200) {
    return std::unexpected(adapter_error::unexpected_status);
  }
  return {};
}

auto github_adapter::create_issue(std::string_view owner, std::string_view repo, std::string_view title,
                                  std::string_view body, std::span<const std::string> labels) const
    -> std::expected<created_issue, adapter_error> {
  auto const payload = build_create_issue_body(title, body, labels);

  auto const auth = support::auth_header(_cred);
  if (!auth) {
    return std::unexpected(auth.error());
  }
  auto const sent = _transport->send({
      .verb    = http::method::post,
      .url     = std::format("{}/repos/{}/{}/issues", _base_url, owner, repo),
      .headers = {{.name = "Content-Type", .value = "application/json"},
                  {.name = "Accept", .value = "application/vnd.github+json"},
                  {.name = "Authorization", .value = *auth}},
      .body    = payload,
  });
  if (!sent) {
    return std::unexpected(adapter_error::transport_failed);
  }
  if (sent->status != 201) {
    return std::unexpected(adapter_error::unexpected_status);
  }
  return parse_created_issue(sent->body);
}

auto github_adapter::link_sub_issue(std::string_view owner, std::string_view repo, std::int64_t parent_number,
                                    std::int64_t child_number) const -> std::expected<void, adapter_error> {
  auto const payload = std::format(R"({{"sub_issue_id":{}}})", child_number);

  auto const auth = support::auth_header(_cred);
  if (!auth) {
    return std::unexpected(auth.error());
  }
  auto const sent = _transport->send({
      .verb    = http::method::post,
      .url     = std::format("{}/repos/{}/{}/issues/{}/sub_issues", _base_url, owner, repo, parent_number),
      .headers = {{.name = "Content-Type", .value = "application/json"},
                  {.name = "Accept", .value = "application/vnd.github+json"},
                  {.name = "Authorization", .value = *auth}},
      .body    = payload,
  });
  if (!sent) {
    return std::unexpected(adapter_error::transport_failed);
  }
  if (sent->status == 404) {
    return std::unexpected(adapter_error::not_found);
  }
  if (sent->status != 200 && sent->status != 201) {
    return std::unexpected(adapter_error::unexpected_status);
  }
  return {};
}

auto github_adapter::link_sub_issue_probe(std::string_view owner, std::string_view repo) const
    -> std::expected<void, adapter_error> {
  auto const auth = support::auth_header(_cred);
  if (!auth) {
    return std::unexpected(auth.error());
  }
  auto const sent = _transport->send({
      .verb    = http::method::post,
      .url     = std::format("{}/repos/{}/{}/issues/0/sub_issues", _base_url, owner, repo),
      .headers = {{.name = "Content-Type", .value = "application/json"},
                  {.name = "Accept", .value = "application/vnd.github+json"},
                  {.name = "Authorization", .value = *auth}},
      .body    = R"({"sub_issue_id":0})",
  });
  if (!sent) {
    return std::unexpected(adapter_error::transport_failed);
  }
  if (sent->status == 404) {
    return std::unexpected(adapter_error::not_found);
  }
  // Every other status (including 422 for the bogus issue id) means the
  // endpoint exists — see this method's doc comment.
  return {};
}

auto github_adapter::render(const adapter::local_entity& local, const adapter::create_options& opts) const
    -> std::expected<std::string, adapter_error> {
  // The Zig original discards CreateOptions here (`_: extsync.CreateOptions`)
  // — the repository is in the URL, not the payload.
  (void)opts;
  auto const description = local.body.empty() ? std::string("(no description)") : local.body;

  std::string out = R"({"title":)";
  json_text::append_json_string(out, local.title);
  out += R"(,"body":)";
  json_text::append_json_string(out, description);
  append_labels(out, labels_for_status(local.status));
  out += "}";
  return out;
}

} // namespace planar::engine::extsync::github
