// @file github.t.cpp
// @brief Tests for `planar.engine.extsync.github` (plan 996, task 6041).
//
// HOME SAFETY. Nothing here opens a database or reads PLANAR_DB / PLANAR_HOME.
// This bucket has no `db` edge at all (see CMakeLists.txt).
//
// NETWORK SAFETY. Every case drives a `recording_transport` that opens no
// socket. "https://api.github.com" appears only as the adapter's DEFAULT base
// URL under assertion — it is never sent to, because the transport under it
// is the recording stub in every single case. The real HTTP round trip for
// this bucket is proven once, in `jira.t.cpp`'s `[loopback]` case, against the
// in-process fixture server; repeating it per adapter would test
// `curl_transport` twice and this adapter zero extra times.
//
// ORACLE PROVENANCE. Not observable through any CLI verb landing this cycle,
// so the oracle is `zig/src/engine/extsync/github.zig` plus the four unit
// tests that file carries for the ported surface, which `zig build test`
// verifies against that implementation:
//
//   - "validate enforces owner/repo#number format": `acme/api#42` accepted,
//     `PROJ-1` and `acme/api` rejected.
//   - "pull maps issue state and labels to local status": the exact fixture
//     body reproduced below, and its five expectations (external_id, title,
//     status=doing, raw_status=open:in-progress, assignee=octocat).
//   - "push sends PATCH with status mapping": one call, body containing
//     `"state":"closed"` and `"title":"Updated title"`.
//   - "render emits label set for doing status": payload containing
//     `"labels":["in-progress"]`.
//
// The full-payload assertions below go beyond those substring checks; they are
// read off the writer sequences in github.zig's `push` and `render` directly,
// so a reordering that preserves every substring still fails here.

import std;
import planar.adapter;
import planar.http;
import planar.engine.extsync.github;

#include <catch2/catch_test_macros.hpp>

namespace {

/// @brief The error of a failed `std::expected`, or unset when it SUCCEEDED.
///
/// Calling `.error()` on an expected that holds a value is undefined
/// behaviour, so `CHECK(call().error() == some_error)` does NOT reliably fail
/// when `call()` unexpectedly succeeds — it reads a dead union member and can
/// compare equal by luck. A break-probe that deleted `update_sync_state`'s
/// not-found check SURVIVED exactly that way. Routing every such assertion
/// through this helper makes an unexpected success compare against
/// `std::nullopt`, which fails loudly.
/// @param value The expected to inspect.
/// @return The error, or unset when `value` holds a value.
template <class T, class E> auto err(const std::expected<T, E>& value) -> std::optional<E> {
  if (value.has_value()) {
    return std::nullopt;
  }
  return value.error();
}

using planar::adapter::adapter_error;
using planar::adapter::auth_credential;
using planar::adapter::auth_kind;
using planar::adapter::field_change_set;
using planar::adapter::local_entity;
using planar::engine::extsync::github::github_adapter;

/// @brief A transport that records what it was asked to send and replies with
/// a canned status/body. Opens no socket.
class recording_transport final : public planar::http::transport {
public:
  std::uint16_t                                reply_status = 200;
  std::string                                  reply_body   = "{}";
  std::size_t                                  calls        = 0;
  std::optional<planar::http::transport_error> failure;
  std::optional<planar::http::method>          last_verb;
  std::string                                  last_url;
  std::optional<std::string>                   last_body;
  std::vector<planar::http::header>            last_headers;

  auto send(const planar::http::request& req) -> std::expected<planar::http::response, planar::http::transport_error> override {
    ++calls;
    if (failure) {
      return std::unexpected(*failure);
    }
    last_verb    = req.verb;
    last_url     = req.url;
    last_body    = req.body;
    last_headers = req.headers;
    return planar::http::response{.status = reply_status, .body = reply_body};
  }

  [[nodiscard]] auto header_value(std::string_view name) const -> std::optional<std::string> {
    for (auto const& field : last_headers) {
      if (field.name == name) {
        return field.value;
      }
    }
    return std::nullopt;
  }
};

// The exact fixture body from github.zig's "pull maps issue state and labels
// to local status" test, reproduced byte for byte.
constexpr std::string_view k_oracle_issue =
    R"({"number":42,"title":"Fix widget","body":"Issue body","state":"open","html_url":"https://github.com/acme/api/issues/42","assignee":{"login":"octocat"},"labels":[{"name":"in-progress"}]})";

auto bearer(std::string_view token) -> auth_credential {
  return auth_credential{.kind = auth_kind::bearer, .token = std::string(token)};
}

auto names(std::initializer_list<std::string_view> values) -> std::vector<std::string> {
  return {values.begin(), values.end()};
}

} // namespace

TEST_CASE("github validate enforces the owner/repo#number format", "[extsync][github]") {
  // github.zig, "validate enforces owner/repo#number format".
  recording_transport  wire;
  github_adapter const adapter("", bearer("tok"), wire);

  CHECK(adapter.validate("acme/api#42").has_value());
  CHECK(err(adapter.validate("PROJ-1")) == std::optional{adapter_error::invalid_external_id});
  CHECK(err(adapter.validate("acme/api")) == std::optional{adapter_error::invalid_external_id});
  // Read off parseExternalID directly: the number must parse whole and be > 0,
  // and neither owner nor repo may be empty.
  CHECK(err(adapter.validate("acme/api#0")) == std::optional{adapter_error::invalid_external_id});
  CHECK(err(adapter.validate("acme/api#-1")) == std::optional{adapter_error::invalid_external_id});
  CHECK(err(adapter.validate("acme/api#4x")) == std::optional{adapter_error::invalid_external_id});
  CHECK(err(adapter.validate("/api#4")) == std::optional{adapter_error::invalid_external_id});
  CHECK(err(adapter.validate("acme/#4")) == std::optional{adapter_error::invalid_external_id});
  // The `#` search is a REVERSE one and the `/` search a FORWARD one, so a
  // repo path with extra segments still splits.
  CHECK(adapter.validate("a/b/c#1").has_value());
  CHECK(wire.calls == 0);
}

TEST_CASE("github pull maps the oracle response's state and labels", "[extsync][github]") {
  // github.zig, "pull maps issue state and labels to local status".
  recording_transport wire;
  wire.reply_body = std::string(k_oracle_issue);
  github_adapter const adapter("https://api.github.com", bearer("tok"), wire);

  auto const state = adapter.pull("acme/api#42");
  REQUIRE(state.has_value());
  CHECK(state->external_id == "acme/api#42");
  CHECK(state->title == "Fix widget");
  CHECK(state->body == "Issue body");
  CHECK(state->status == "doing");
  CHECK(state->raw_status == "open:in-progress");
  CHECK(state->assignee == "octocat");
  CHECK(state->url == "https://github.com/acme/api/issues/42");
  // GitHub Issues carries neither, and the Zig original hard-codes both
  // rather than inventing a mapping.
  CHECK(state->priority == 0);
  CHECK(state->due_at.empty());

  CHECK(wire.last_url == "https://api.github.com/repos/acme/api/issues/42");
  CHECK(wire.header_value("Accept") == std::optional<std::string>{"application/vnd.github+json"});
}

TEST_CASE("github pull reads updated_at as the conflict-guard version", "[extsync][github]") {
  recording_transport wire;
  wire.reply_body = R"({"title":"t","state":"open","updated_at":"2026-08-01T09:00:00Z"})";
  github_adapter const adapter("", bearer("t"), wire);

  auto const state = adapter.pull("o/r#1");
  REQUIRE(state.has_value());
  CHECK(state->version == "2026-08-01T09:00:00Z");
}

TEST_CASE("github status_to_local resolves state plus labels", "[extsync][github]") {
  using planar::engine::extsync::github::status_to_local;

  CHECK(status_to_local("open", names({})) == "todo");
  CHECK(status_to_local("open", names({"in-progress"})) == "doing");
  CHECK(status_to_local("open", names({"blocked"})) == "blocked");
  CHECK(status_to_local("closed", names({})) == "done");
  CHECK(status_to_local("closed", names({"wontfix"})) == "cancelled");
  // A closed issue carrying an in-progress label is still `done` — the
  // closed arm returns before any open-side label is considered.
  CHECK(status_to_local("closed", names({"in-progress"})) == "done");
  // Both open labels present: the FIRST MATCHING LABEL IN ARRAY ORDER wins,
  // not the first check in the function. The Zig loop tests `in-progress` then
  // `blocked` INSIDE each label's iteration, so it returns on the first label
  // that matches either — which for `["blocked","in-progress"]` is `blocked`.
  // Reading the check order as the precedence order gets this backwards, and
  // it is exactly the kind of inversion a port silently introduces.
  CHECK(status_to_local("open", names({"blocked", "in-progress"})) == "blocked");
  CHECK(status_to_local("open", names({"in-progress", "blocked"})) == "doing");
  // An unrelated label changes nothing.
  CHECK(status_to_local("open", names({"bug"})) == "todo");
}

TEST_CASE("github raw_status renders the colon-joined provider string", "[extsync][github]") {
  using planar::engine::extsync::github::raw_status;

  CHECK(raw_status("open", names({})) == "open");
  CHECK(raw_status("open", names({"in-progress"})) == "open:in-progress");
  CHECK(raw_status("open", names({"blocked"})) == "open:blocked");
  CHECK(raw_status("closed", names({})) == "closed");
  CHECK(raw_status("closed", names({"wontfix"})) == "closed:wontfix");
  // Same array-order precedence as status_to_local, for the same reason.
  CHECK(raw_status("open", names({"blocked", "in-progress"})) == "open:blocked");
  CHECK(raw_status("open", names({"in-progress", "blocked"})) == "open:in-progress");
}

TEST_CASE("github state_for_status maps every local status onto state plus labels", "[extsync][github]") {
  using planar::engine::extsync::github::state_for_status;

  CHECK(state_for_status("todo").state == "open");
  CHECK(state_for_status("todo").labels.empty());
  CHECK(state_for_status("doing").state == "open");
  CHECK(state_for_status("doing").labels == std::vector<std::string_view>{"in-progress"});
  CHECK(state_for_status("blocked").labels == std::vector<std::string_view>{"blocked"});
  CHECK(state_for_status("done").state == "closed");
  CHECK(state_for_status("done").labels.empty());
  CHECK(state_for_status("cancelled").state == "closed");
  CHECK(state_for_status("cancelled").labels == std::vector<std::string_view>{"wontfix"});
  // Unrecognized is plain `open`, NOT a failure.
  CHECK(state_for_status("archived").state == "open");
  CHECK(state_for_status("archived").labels.empty());
}

TEST_CASE("github push PATCHes title and mapped state together", "[extsync][github]") {
  // github.zig, "push sends PATCH with status mapping" — one call, body
  // containing both fragments. The full payload is read off push's writer
  // sequence.
  recording_transport wire;
  wire.reply_status = 200;
  github_adapter const adapter("", bearer("tok"), wire);

  auto const outcome = adapter.push("acme/api#42", field_change_set{.title = "Updated title", .status = "done"});
  REQUIRE(outcome.has_value());
  CHECK(wire.calls == 1);
  CHECK(wire.last_verb == planar::http::method::patch);
  CHECK(wire.last_url == "https://api.github.com/repos/acme/api/issues/42");
  REQUIRE(wire.last_body.has_value());
  CHECK(*wire.last_body == R"({"title":"Updated title","state":"closed"})");
  CHECK(outcome->fields_applied == std::vector<std::string>{"title", "status"});
}

TEST_CASE("github push emits the label array for a status that has one", "[extsync][github]") {
  recording_transport  wire;
  github_adapter const adapter("", bearer("t"), wire);

  REQUIRE(adapter.push("o/r#1", field_change_set{.status = "doing"}).has_value());
  REQUIRE(wire.last_body.has_value());
  CHECK(*wire.last_body == R"({"state":"open","labels":["in-progress"]})");

  REQUIRE(adapter.push("o/r#1", field_change_set{.status = "cancelled", .assignee = "octocat"}).has_value());
  REQUIRE(wire.last_body.has_value());
  CHECK(*wire.last_body == R"({"state":"closed","labels":["wontfix"],"assignees":["octocat"]})");
}

TEST_CASE("github push sends NO request when the change set names nothing", "[extsync][github]") {
  recording_transport  wire;
  github_adapter const adapter("", bearer("t"), wire);

  auto const outcome = adapter.push("o/r#1", field_change_set{.priority = "High"});
  REQUIRE(outcome.has_value());
  CHECK(outcome->fields_applied.empty());
  // `priority` has no GitHub Issues counterpart, so a priority-only change set
  // is the empty change set as far as this adapter is concerned.
  CHECK(wire.calls == 0);
}

TEST_CASE("github push reports unexpected_status for anything but 200", "[extsync][github]") {
  recording_transport wire;
  wire.reply_status = 204;
  github_adapter const adapter("", bearer("t"), wire);
  CHECK(err(adapter.push("o/r#1", field_change_set{.title = "T"})) == std::optional{adapter_error::unexpected_status});
}

TEST_CASE("github pull maps 404 to not_found and other statuses to unexpected_status", "[extsync][github]") {
  recording_transport  wire;
  github_adapter const adapter("", bearer("t"), wire);

  wire.reply_status = 404;
  CHECK(err(adapter.pull("o/r#1")) == std::optional{adapter_error::not_found});
  wire.reply_status = 500;
  CHECK(err(adapter.pull("o/r#1")) == std::optional{adapter_error::unexpected_status});
  wire.reply_status = 200;
  wire.reply_body   = "]not json[";
  CHECK(err(adapter.pull("o/r#1")) == std::optional{adapter_error::parse_failed});
}

TEST_CASE("github post_comment POSTs to the comments endpoint with the body escaped", "[extsync][github]") {
  // github.zig, "postComment POSTs to comments endpoint with body" — pins
  // the URL shape and the escaped-quote body.
  recording_transport wire;
  wire.reply_status = 201;
  github_adapter const adapter("", bearer("tok"), wire);

  auto const result = adapter.post_comment("acme/api#42", R"(Hello "world")");
  REQUIRE(result.has_value());
  CHECK(wire.calls == 1);
  CHECK(wire.last_verb == planar::http::method::post);
  CHECK(wire.last_url == "https://api.github.com/repos/acme/api/issues/42/comments");
  REQUIRE(wire.last_body.has_value());
  CHECK(*wire.last_body == R"({"body":"Hello \"world\""})");
  CHECK(wire.header_value("Accept") == std::optional<std::string>{"application/vnd.github+json"});
}

TEST_CASE("github post_comment refuses a malformed external id before sending", "[extsync][github]") {
  recording_transport  wire;
  github_adapter const adapter("", bearer("t"), wire);
  CHECK(err(adapter.post_comment("not-an-id", "hi")) == std::optional{adapter_error::invalid_external_id});
  CHECK(wire.calls == 0);
}

TEST_CASE("github post_comment accepts 200 as well as 201", "[extsync][github]") {
  recording_transport wire;
  wire.reply_status = 200;
  github_adapter const adapter("", bearer("t"), wire);
  CHECK(adapter.post_comment("o/r#1", "hi").has_value());
}

TEST_CASE("github post_comment reports unexpected_status for anything else", "[extsync][github]") {
  recording_transport wire;
  wire.reply_status = 422;
  github_adapter const adapter("", bearer("t"), wire);
  CHECK(err(adapter.post_comment("o/r#1", "hi")) == std::optional{adapter_error::unexpected_status});
}

TEST_CASE("github render emits the label set for a doing task", "[extsync][github]") {
  // github.zig, "render emits label set for doing status".
  recording_transport  wire;
  github_adapter const adapter("", bearer("tok"), wire);

  auto const payload = adapter.render(local_entity{.kind = "task", .id = 7, .title = "T", .body = "B", .status = "doing"}, {});
  REQUIRE(payload.has_value());
  CHECK(*payload == R"({"title":"T","body":"B","labels":["in-progress"]})");
}

TEST_CASE("github render omits labels and substitutes the empty-body placeholder", "[extsync][github]") {
  recording_transport  wire;
  github_adapter const adapter("", bearer("t"), wire);

  auto const payload = adapter.render(local_entity{.kind = "task", .id = 1, .title = "T", .status = "todo"}, {});
  REQUIRE(payload.has_value());
  CHECK(*payload == R"json({"title":"T","body":"(no description)"})json");
  // `done` also has no label — it is `closed` on the push side, but a NEW
  // issue is always open, so render has nothing to say about it.
  auto const done = adapter.render(local_entity{.kind = "task", .id = 1, .title = "T", .body = "B", .status = "done"}, {});
  REQUIRE(done.has_value());
  CHECK(*done == R"({"title":"T","body":"B"})");
}

TEST_CASE("github adapter defaults an empty base URL to api.github.com and trims a slash", "[extsync][github]") {
  {
    recording_transport  wire;
    github_adapter const adapter("", bearer("t"), wire);
    REQUIRE(adapter.pull("o/r#1").has_value());
    CHECK(wire.last_url == "https://api.github.com/repos/o/r/issues/1");
  }
  {
    recording_transport  wire;
    github_adapter const adapter("https://ghe.example.com/api/v3/", bearer("t"), wire);
    REQUIRE(adapter.pull("o/r#1").has_value());
    CHECK(wire.last_url == "https://ghe.example.com/api/v3/repos/o/r/issues/1");
  }
}

TEST_CASE("github adapter refuses a bearer credential with no token before sending", "[extsync][github]") {
  recording_transport  wire;
  github_adapter const adapter("", auth_credential{.kind = auth_kind::bearer}, wire);
  CHECK(err(adapter.pull("o/r#1")) == std::optional{adapter_error::invalid_auth});
  CHECK(wire.calls == 0);
}

// ---- create_issue (plan 1009, task 6408) -----------------------------------

TEST_CASE("github create_issue POSTs to the issues endpoint and parses number and node_id", "[extsync][github]") {
  // github.zig, "createIssue parses number and node_id from REST response".
  recording_transport wire;
  wire.reply_status = 201;
  wire.reply_body   = R"({"number":42,"node_id":"NODE_42","html_url":"https://github.com/acme/api/issues/42"})";
  github_adapter const adapter("", bearer("tok"), wire);

  auto const created = adapter.create_issue("acme", "api", "Hello", "World", {});
  REQUIRE(created.has_value());
  CHECK(created->number == 42);
  CHECK(created->node_id == "NODE_42");

  CHECK(wire.calls == 1);
  CHECK(wire.last_verb == planar::http::method::post);
  CHECK(wire.last_url == "https://api.github.com/repos/acme/api/issues");
  REQUIRE(wire.last_body.has_value());
  // The POST body carries title + body and omits `labels` entirely when
  // empty — read off buildCreateIssueBody's writer sequence directly.
  CHECK(*wire.last_body == R"({"title":"Hello","body":"World"})");
  CHECK(wire.header_value("Accept") == std::optional<std::string>{"application/vnd.github+json"});
  CHECK(wire.header_value("Content-Type") == std::optional<std::string>{"application/json"});
}

TEST_CASE("github create_issue emits the labels array when present", "[extsync][github]") {
  // github.zig, "createIssue emits labels array when present".
  recording_transport wire;
  wire.reply_status = 201;
  wire.reply_body   = R"({"number":7,"node_id":"NODE_7"})";
  github_adapter const adapter("", bearer("tok"), wire);

  auto const labels  = names({"bug", "p1"});
  auto const created = adapter.create_issue("o", "r", "T", "B", labels);
  REQUIRE(created.has_value());
  REQUIRE(wire.last_body.has_value());
  CHECK(*wire.last_body == R"({"title":"T","body":"B","labels":["bug","p1"]})");
}

TEST_CASE("github create_issue reports unexpected_status for anything but 201", "[extsync][github]") {
  recording_transport  wire;
  github_adapter const adapter("", bearer("t"), wire);

  wire.reply_status = 200;
  CHECK(err(adapter.create_issue("o", "r", "T", "B", {})) == std::optional{adapter_error::unexpected_status});
  wire.reply_status = 422;
  CHECK(err(adapter.create_issue("o", "r", "T", "B", {})) == std::optional{adapter_error::unexpected_status});
  wire.reply_status = 500;
  CHECK(err(adapter.create_issue("o", "r", "T", "B", {})) == std::optional{adapter_error::unexpected_status});
}

TEST_CASE("github create_issue reports parse_failed for a malformed or number-less body", "[extsync][github]") {
  recording_transport  wire;
  github_adapter const adapter("", bearer("t"), wire);

  wire.reply_status = 201;
  wire.reply_body   = "]not json[";
  CHECK(err(adapter.create_issue("o", "r", "T", "B", {})) == std::optional{adapter_error::parse_failed});

  wire.reply_body = R"({"node_id":"N1"})"; // `number` missing entirely.
  CHECK(err(adapter.create_issue("o", "r", "T", "B", {})) == std::optional{adapter_error::parse_failed});

  wire.reply_body = R"({"number":"42"})"; // `number` present but wrong-typed.
  CHECK(err(adapter.create_issue("o", "r", "T", "B", {})) == std::optional{adapter_error::parse_failed});
}

TEST_CASE("github create_issue tolerates a response with no node_id", "[extsync][github]") {
  recording_transport wire;
  wire.reply_status = 201;
  wire.reply_body   = R"({"number":9})";
  github_adapter const adapter("", bearer("t"), wire);

  auto const created = adapter.create_issue("o", "r", "T", "B", {});
  REQUIRE(created.has_value());
  CHECK(created->number == 9);
  CHECK(created->node_id.empty());
}

TEST_CASE("github create_issue refuses a bearer credential with no token before sending", "[extsync][github]") {
  recording_transport  wire;
  github_adapter const adapter("", auth_credential{.kind = auth_kind::bearer}, wire);
  CHECK(err(adapter.create_issue("o", "r", "T", "B", {})) == std::optional{adapter_error::invalid_auth});
  CHECK(wire.calls == 0);
}

// ---- link_sub_issue (plan 1009, task 6408) ---------------------------------

TEST_CASE("github link_sub_issue POSTs sub_issue_id to the sub_issues endpoint", "[extsync][github]") {
  // github.zig, "linkSubIssue 201 success".
  recording_transport wire;
  wire.reply_status = 201;
  wire.reply_body   = R"({"id":2})";
  github_adapter const adapter("", bearer("tok"), wire);

  auto const linked = adapter.link_sub_issue("o", "r", 1, 2);
  REQUIRE(linked.has_value());
  CHECK(wire.calls == 1);
  CHECK(wire.last_verb == planar::http::method::post);
  CHECK(wire.last_url == "https://api.github.com/repos/o/r/issues/1/sub_issues");
  REQUIRE(wire.last_body.has_value());
  CHECK(*wire.last_body == R"({"sub_issue_id":2})");
}

TEST_CASE("github link_sub_issue also accepts 200", "[extsync][github]") {
  recording_transport wire;
  wire.reply_status = 200;
  github_adapter const adapter("", bearer("t"), wire);
  CHECK(adapter.link_sub_issue("o", "r", 1, 2).has_value());
}

TEST_CASE("github link_sub_issue maps 404 to not_found", "[extsync][github]") {
  // github.zig, "linkSubIssue 404 maps to NotFound".
  recording_transport wire;
  wire.reply_status = 404;
  github_adapter const adapter("", bearer("t"), wire);
  CHECK(err(adapter.link_sub_issue("o", "r", 1, 2)) == std::optional{adapter_error::not_found});
}

TEST_CASE("github link_sub_issue reports unexpected_status for anything else", "[extsync][github]") {
  recording_transport wire;
  wire.reply_status = 500;
  github_adapter const adapter("", bearer("t"), wire);
  CHECK(err(adapter.link_sub_issue("o", "r", 1, 2)) == std::optional{adapter_error::unexpected_status});
}

TEST_CASE("github link_sub_issue refuses a bearer credential with no token before sending", "[extsync][github]") {
  recording_transport  wire;
  github_adapter const adapter("", auth_credential{.kind = auth_kind::bearer}, wire);
  CHECK(err(adapter.link_sub_issue("o", "r", 1, 2)) == std::optional{adapter_error::invalid_auth});
  CHECK(wire.calls == 0);
}

// ---- link_sub_issue_probe (plan 1009, task 6408) ---------------------------
//
// This probe's whole purpose is telling apart "sub-issues unsupported"
// (404) from "sub-issues supported" (anything else, including the 422 a
// bogus issue-0 payload legitimately provokes on a supporting account) —
// see this method's doc comment in github.cppm. Both arms are covered
// below, plus the exact request shape, so a canned-response fixture that
// replies the same regardless of input could not pass both.

TEST_CASE("github link_sub_issue_probe posts the bogus issue-0 payload to the sub_issues endpoint", "[extsync][github]") {
  recording_transport wire;
  wire.reply_status = 422;
  github_adapter const adapter("", bearer("tok"), wire);

  auto const probed = adapter.link_sub_issue_probe("acme", "api");
  REQUIRE(probed.has_value());
  CHECK(wire.calls == 1);
  CHECK(wire.last_verb == planar::http::method::post);
  CHECK(wire.last_url == "https://api.github.com/repos/acme/api/issues/0/sub_issues");
  REQUIRE(wire.last_body.has_value());
  CHECK(*wire.last_body == R"({"sub_issue_id":0})");
}

TEST_CASE("github link_sub_issue_probe: 404 means the endpoint is not enabled", "[extsync][github]") {
  // github.zig, "linkSubIssueProbe 404 means endpoint not enabled".
  recording_transport wire;
  wire.reply_status = 404;
  github_adapter const adapter("", bearer("t"), wire);
  CHECK(err(adapter.link_sub_issue_probe("o", "r")) == std::optional{adapter_error::not_found});
}

TEST_CASE("github link_sub_issue_probe: 422 means the endpoint is enabled", "[extsync][github]") {
  // github.zig, "linkSubIssueProbe 422 means endpoint enabled".
  recording_transport wire;
  wire.reply_status = 422;
  github_adapter const adapter("", bearer("t"), wire);
  CHECK(adapter.link_sub_issue_probe("o", "r").has_value());
}

TEST_CASE("github link_sub_issue_probe: 200 also means the endpoint is enabled", "[extsync][github]") {
  // Any non-404 status is "supported" — not just 422. This pins the
  // asymmetry at a SECOND status so a mutation collapsing the check to
  // "only 422 is supported" cannot survive.
  recording_transport wire;
  wire.reply_status = 200;
  github_adapter const adapter("", bearer("t"), wire);
  CHECK(adapter.link_sub_issue_probe("o", "r").has_value());
}

TEST_CASE("github link_sub_issue_probe refuses a bearer credential with no token before sending", "[extsync][github]") {
  recording_transport  wire;
  github_adapter const adapter("", auth_credential{.kind = auth_kind::bearer}, wire);
  CHECK(err(adapter.link_sub_issue_probe("o", "r")) == std::optional{adapter_error::invalid_auth});
  CHECK(wire.calls == 0);
}

TEST_CASE("github preserves certificate verification failure in the operator diagnostic", "[extsync][github]") {
  recording_transport wire;
  wire.failure = planar::http::transport_error::certificate_verification_failed;
  planar::engine::extsync::github::github_adapter adapter{
      "https://github.invalid", {.kind = planar::adapter::auth_kind::bearer, .token = "fixture-token"}, wire};
  auto const pulled = adapter.pull("acme/api#42");
  REQUIRE_FALSE(pulled.has_value());
  CHECK(planar::adapter::adapter_error_name(pulled.error()) == "CertificateVerificationFailed");
}
