/// @file jira.cpp
/// @brief Implementation of `planar.engine.extsync.jira`. See jira.cppm for
/// the port scope, the deferral, and the three easy-to-invert field mappings.

module;

#include "json_read.hpp"

module planar.engine.extsync.jira;

import std;
import planar.adapter;
import planar.http;
import planar.json_text;
import planar.engine.extsync.support;

namespace planar::engine::extsync::jira {

namespace {

using adapter::adapter_error;
using json_read::array_field;
using json_read::object_field;
using json_read::string_field;

/// @brief Accept a canonical Jira issue key.
///
/// Port of `isValidIssueKey`. The dash is found with a FORWARD search
/// (`indexOfScalar`), so `A-B-1` splits at the first dash and then fails on
/// `B-1` not being all digits.
/// @param external_id The candidate.
/// @return Whether it is a well-formed key.
auto is_valid_issue_key(std::string_view external_id) -> bool {
  auto const dash = external_id.find('-');
  if (dash == std::string_view::npos || dash == 0 || dash + 1 >= external_id.size()) {
    return false;
  }
  for (char const c : external_id.substr(0, dash)) {
    bool const ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) {
      return false;
    }
  }
  return std::ranges::all_of(external_id.substr(dash + 1), [](char c) { return c >= '0' && c <= '9'; });
}

/// @brief Flatten an Atlassian Document Format description into plain text.
///
/// Port of `extractADFText`. One newline BETWEEN blocks (never a trailing
/// one), inline text nodes concatenated with no separator, and any node that
/// is not shaped as expected silently skipped. A `null` description — which
/// is what Jira sends for an empty one — yields the empty string.
/// @param description The `fields.description` value, or null.
/// @return The flattened text.
auto extract_adf_text(const glz::generic* description) -> std::string {
  if (description == nullptr) {
    return {};
  }
  auto const* const blocks = array_field(*description, "content");
  if (blocks == nullptr) {
    return {};
  }
  std::string out;
  bool        first_line = true;
  for (auto const& block : *blocks) {
    auto const* const inner = array_field(block, "content");
    if (inner == nullptr) {
      continue;
    }
    if (!first_line) {
      out.push_back('\n');
    }
    first_line = false;
    for (auto const& node : *inner) {
      if (auto const text = string_field(node, "text")) {
        out += *text;
      }
    }
  }
  return out;
}

/// @brief Parse a Jira issue response into a `remote_state`.
/// @param fallback_external_id Used when the response omits `key`.
/// @param raw The response body.
/// @return The normalized state, or `adapter_error::parse_failed`.
auto parse_issue(std::string_view fallback_external_id, std::string_view raw)
    -> std::expected<adapter::remote_state, adapter_error> {
  auto parsed = glz::read_json<glz::generic>(raw);
  if (!parsed || !parsed->is_object()) {
    return std::unexpected(adapter_error::parse_failed);
  }
  glz::generic const& root   = *parsed;
  auto const* const   fields = object_field(root, "fields");
  if (fields == nullptr) {
    return std::unexpected(adapter_error::parse_failed);
  }

  auto const  key     = string_field(root, "key").value_or(std::string(fallback_external_id));
  auto const  summary = string_field(*fields, "summary").value_or(std::string{});
  std::string status_name;
  if (auto const* const status_obj = object_field(*fields, "status")) {
    status_name = string_field(*status_obj, "name").value_or(std::string{});
  }

  // `emailAddress` FIRST, then `accountId` — see jira.cppm's header, point 2.
  std::string assignee;
  if (auto const* const assignee_obj = object_field(*fields, "assignee")) {
    if (auto const mail = string_field(*assignee_obj, "emailAddress")) {
      assignee = *mail;
    } else if (auto const account = string_field(*assignee_obj, "accountId")) {
      assignee = *account;
    }
  }

  // The `id` STRING, parsed; unparseable is 0, not a failure. Point 3.
  std::int64_t priority = 0;
  if (auto const* const priority_obj = object_field(*fields, "priority")) {
    if (auto const id_text = string_field(*priority_obj, "id")) {
      auto const* const begin = id_text->data();
      auto const* const end   = begin + id_text->size();
      std::int64_t      value = 0;
      if (std::from_chars(begin, end, value).ec == std::errc{}) {
        priority = value;
      }
    }
  }

  return adapter::remote_state{
      .external_id = key,
      .title       = summary,
      .body        = extract_adf_text(object_field(*fields, "description")),
      .status      = std::string(map_status(status_name)),
      .assignee    = std::move(assignee),
      .priority    = priority,
      .due_at      = string_field(*fields, "duedate").value_or(std::string{}),
      // The Zig original hard-codes an empty URL here — Jira's issue payload
      // carries `self` (an API URL), not a browsable one, and the original
      // does not synthesize one. Preserved rather than "improved".
      .url        = {},
      .raw_status = status_name,
      .version    = string_field(*fields, "updated").value_or(std::string{}),
  };
}

} // namespace

auto map_status(std::string_view raw) -> std::string_view {
  if (raw == "To Do") {
    return "todo";
  }
  if (raw == "In Progress") {
    return "doing";
  }
  if (raw == "Blocked") {
    return "blocked";
  }
  if (raw == "Done") {
    return "done";
  }
  if (raw == "Cancelled") {
    return "cancelled";
  }
  if (raw == "Won't Do") {
    return "cancelled";
  }
  return "";
}

jira_adapter::jira_adapter(std::string_view base_url, adapter::auth_credential cred, http::transport& wire)
    : _base_url(support::trim_trailing_slash(base_url)), _cred(std::move(cred)), _transport(&wire) {
}

auto jira_adapter::validate(std::string_view external_id) const -> std::expected<void, adapter_error> {
  if (!is_valid_issue_key(external_id)) {
    return std::unexpected(adapter_error::invalid_external_id);
  }
  return {};
}

auto jira_adapter::pull(std::string_view external_id) const -> std::expected<adapter::remote_state, adapter_error> {
  if (auto const ok = validate(external_id); !ok) {
    return std::unexpected(ok.error());
  }
  auto const auth = support::auth_header(_cred);
  if (!auth) {
    return std::unexpected(auth.error());
  }
  auto const sent = _transport->send({
      .verb    = http::method::get,
      .url     = std::format("{}/rest/api/3/issue/{}", _base_url, external_id),
      .headers = {{.name = "Accept", .value = "application/json"}, {.name = "Authorization", .value = *auth}},
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

auto jira_adapter::push(std::string_view external_id, const adapter::field_change_set& fields) const
    -> std::expected<adapter::update_outcome, adapter_error> {
  if (auto const ok = validate(external_id); !ok) {
    return std::unexpected(ok.error());
  }

  adapter::update_outcome outcome;
  std::string             body      = R"({"fields":{)";
  bool                    has_field = false;

  if (fields.title.has_value()) {
    has_field = true;
    body += R"("summary":)";
    json_text::append_json_string(body, *fields.title);
    outcome.fields_applied.emplace_back("title");
  }
  if (fields.assignee.has_value()) {
    if (has_field) {
      body += ",";
    }
    has_field = true;
    body += R"("assignee":{"accountId":)";
    json_text::append_json_string(body, *fields.assignee);
    body += "}";
    outcome.fields_applied.emplace_back("assignee");
  }
  if (fields.priority.has_value()) {
    if (has_field) {
      body += ",";
    }
    has_field = true;
    body += R"("priority":{"name":)";
    json_text::append_json_string(body, *fields.priority);
    body += "}";
    outcome.fields_applied.emplace_back("priority");
  }
  // fields.status is deliberately unread: Jira status changes go through
  // /transitions, which this endpoint cannot express. See jira.cppm, point 1.
  body += "}}";

  if (!has_field) {
    // No request at all — not an empty PUT. This is the observable half of
    // the contract and the reason the test asserts a request COUNT.
    return adapter::update_outcome{};
  }

  auto const auth = support::auth_header(_cred);
  if (!auth) {
    return std::unexpected(auth.error());
  }
  auto const sent = _transport->send({
      .verb    = http::method::put,
      .url     = std::format("{}/rest/api/3/issue/{}", _base_url, external_id),
      .headers = {{.name = "Content-Type", .value = "application/json"}, {.name = "Authorization", .value = *auth}},
      .body    = body,
  });
  if (!sent) {
    return std::unexpected(adapter_error::transport_failed);
  }
  if (sent->status != 204) {
    return std::unexpected(adapter_error::unexpected_status);
  }
  return outcome;
}

auto jira_adapter::post_comment(std::string_view external_id, std::string_view comment) const
    -> std::expected<void, adapter_error> {
  if (auto const ok = validate(external_id); !ok) {
    return std::unexpected(ok.error());
  }

  std::string body = R"({"body":{"type":"doc","version":1,"content":[{"type":"paragraph","content":[{"type":"text","text":)";
  json_text::append_json_string(body, comment);
  body += "}]}]}}";

  auto const auth = support::auth_header(_cred);
  if (!auth) {
    return std::unexpected(auth.error());
  }
  auto const sent = _transport->send({
      .verb    = http::method::post,
      .url     = std::format("{}/rest/api/3/issue/{}/comment", _base_url, external_id),
      .headers = {{.name = "Content-Type", .value = "application/json"},
                  {.name = "Accept", .value = "application/json"},
                  {.name = "Authorization", .value = *auth}},
      .body    = body,
  });
  if (!sent) {
    return std::unexpected(adapter_error::transport_failed);
  }
  if (sent->status != 201 && sent->status != 200) {
    return std::unexpected(adapter_error::unexpected_status);
  }
  return {};
}

auto jira_adapter::render(const adapter::local_entity& local, const adapter::create_options& opts) const
    -> std::expected<std::string, adapter_error> {
  auto const issue_type  = opts.issue_type.value_or(std::string("Story"));
  auto const description = local.body.empty() ? std::string("(no description)") : local.body;

  std::string out = R"({"fields":{)";
  if (opts.project.has_value()) {
    out += R"("project":{"key":)";
    json_text::append_json_string(out, *opts.project);
    out += "},";
  }
  out += R"("summary":)";
  json_text::append_json_string(out, local.title);
  out += R"(,"issuetype":{"name":)";
  json_text::append_json_string(out, issue_type);
  out += R"(},"description":{"type":"doc","version":1,"content":[{"type":"paragraph","content":[{"type":"text","text":)";
  json_text::append_json_string(out, description);
  out += "}]}]}}}";
  return out;
}

} // namespace planar::engine::extsync::jira
