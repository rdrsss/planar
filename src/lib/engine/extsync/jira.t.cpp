// @file jira.t.cpp
// @brief Tests for `planar.engine.extsync.jira` (plan 996, task 6041).
//
// HOME SAFETY. Nothing here opens a database or reads PLANAR_DB / PLANAR_HOME.
// This bucket has no `db` edge at all (see CMakeLists.txt), so there is no
// SQLite handle to point anywhere.
//
// NETWORK SAFETY. Most cases drive a `recording_transport` that opens no
// socket whatsoever. The one case that performs a REAL HTTP round trip sends
// to `planar::http::fixture::server`'s `base_url()`, which is
// `http://127.0.0.1:<kernel-assigned>`. The literal "https://acme.atlassian.net"
// appears below only as a base-URL STRING handed to an adapter whose transport
// is the recording stub — grep for it and check: every occurrence is paired
// with `recording_transport`, never with `curl_transport`.
//
// ORACLE PROVENANCE. This adapter's behavior is not observable through any
// CLI verb that lands this cycle (`ext create` / `ext propagate` / `sync pull`
// are all deferred at layer 3), so the oracle is not a captured binary run —
// it is `zig/src/engine/extsync/jira.zig` itself PLUS the five unit tests that
// file carries, which `zig build test` verifies against that implementation:
//
//   - "validate accepts canonical Jira issue keys" pins PROJ-123 accepted,
//     "proj-123" and "PROJ" rejected.
//   - "pull maps Jira response into RemoteState" pins the exact fixture body
//     reproduced below and its six expected outputs (external_id, title, body,
//     status, assignee, priority).
//   - "push skips status-only update and sends title update" pins BOTH the
//     zero-request behavior for a status-only change AND the
//     `"summary":"Updated title"` body fragment.
//   - "render applies default issue type and description placeholder" pins
//     `"name":"Story"` and `(no description)`.
//
// Each assertion below names which of those it comes from. The exact byte
// strings for `render` and `push` bodies go further than the Zig tests do
// (they check substrings) — those full-payload assertions are read off the
// writer sequence in jira.zig directly and exist so a reordering that keeps
// every substring intact still fails.

import std;
import planar.adapter;
import planar.http;
import planar.engine.extsync.jira;

#include "../../http/fixture_server.hpp"

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
using planar::adapter::create_options;
using planar::adapter::field_change_set;
using planar::adapter::local_entity;
using planar::engine::extsync::jira::jira_adapter;

/// @brief A transport that records what it was asked to send and replies with
/// a canned status/body. Opens no socket.
class recording_transport final : public planar::http::transport {
public:
  std::uint16_t                       reply_status = 200;
  std::string                         reply_body;
  std::size_t                         calls = 0;
  std::optional<planar::http::method> last_verb;
  std::string                         last_url;
  std::optional<std::string>          last_body;
  std::vector<planar::http::header>   last_headers;

  auto send(const planar::http::request& req) -> std::expected<planar::http::response, planar::http::transport_error> override {
    ++calls;
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

/// @brief A transport that always fails, for the TransportFailed arm.
class dead_transport final : public planar::http::transport {
public:
  auto send(const planar::http::request&) -> std::expected<planar::http::response, planar::http::transport_error> override {
    return std::unexpected(planar::http::transport_error::send_failed);
  }
};

// The exact fixture body from jira.zig's "pull maps Jira response into
// RemoteState" test, reproduced byte for byte.
constexpr std::string_view k_oracle_issue =
    R"({"key":"PROJ-1","fields":{"summary":"Test issue","description":{"type":"doc","version":1,"content":[{"type":"paragraph","content":[{"type":"text","text":"Issue body text"}]}]},"status":{"name":"In Progress"},"assignee":{"accountId":"acc","emailAddress":"dev@example.com"},"priority":{"id":"2","name":"High"},"duedate":"2026-06-30"}})";

auto bearer(std::string_view token) -> auth_credential {
  return auth_credential{.kind = auth_kind::bearer, .token = std::string(token)};
}

} // namespace

TEST_CASE("jira validate accepts canonical issue keys and rejects the rest", "[extsync][jira]") {
  // jira.zig, "validate accepts canonical Jira issue keys".
  recording_transport wire;
  jira_adapter const  adapter("https://acme.atlassian.net", bearer("t"), wire);

  CHECK(adapter.validate("PROJ-123").has_value());
  CHECK(err(adapter.validate("proj-123")) == std::optional{adapter_error::invalid_external_id});
  CHECK(err(adapter.validate("PROJ")) == std::optional{adapter_error::invalid_external_id});
  // Beyond the Zig test, read off isValidIssueKey directly: neither side may
  // be empty, and the tail must be all digits.
  CHECK(err(adapter.validate("-1")) == std::optional{adapter_error::invalid_external_id});
  CHECK(err(adapter.validate("PROJ-")) == std::optional{adapter_error::invalid_external_id});
  CHECK(err(adapter.validate("PROJ-1a")) == std::optional{adapter_error::invalid_external_id});
  CHECK(adapter.validate("P_R0J-9").has_value());
  CHECK(wire.calls == 0);
}

TEST_CASE("jira pull maps the oracle response into remote_state", "[extsync][jira]") {
  // jira.zig, "pull maps Jira response into RemoteState" — same fixture body,
  // same six expectations, plus raw_status/due_at/version which that test does
  // not assert but parseIssue plainly sets.
  recording_transport wire;
  wire.reply_status = 200;
  wire.reply_body   = std::string(k_oracle_issue);

  jira_adapter const adapter("https://acme.atlassian.net", bearer("tok"), wire);
  auto const         state = adapter.pull("PROJ-1");
  REQUIRE(state.has_value());

  CHECK(state->external_id == "PROJ-1");
  CHECK(state->title == "Test issue");
  CHECK(state->body == "Issue body text");
  CHECK(state->status == "doing");
  // emailAddress wins over accountId, which the fixture supplies BOTH of.
  CHECK(state->assignee == "dev@example.com");
  CHECK(state->priority == 2);
  CHECK(state->due_at == "2026-06-30");
  CHECK(state->raw_status == "In Progress");
  CHECK(state->url.empty());

  CHECK(wire.last_url == "https://acme.atlassian.net/rest/api/3/issue/PROJ-1");
  CHECK(wire.last_verb == planar::http::method::get);
  CHECK(wire.header_value("Authorization") == std::optional<std::string>{"Bearer tok"});
  CHECK(wire.header_value("Accept") == std::optional<std::string>{"application/json"});
}

TEST_CASE("jira pull reads fields.updated as the conflict-guard version", "[extsync][jira]") {
  // Not asserted by any Zig unit test, but load-bearing: resolve_conflict
  // refuses to authorize either side when `version` is empty, so an adapter
  // that dropped `updated` would silently disarm the guard.
  recording_transport wire;
  wire.reply_body = R"({"key":"S-1","fields":{"summary":"t","updated":"2026-07-13T13:00:00Z","status":{"name":"To Do"}}})";
  jira_adapter const adapter("https://x", bearer("t"), wire);

  auto const state = adapter.pull("S-1");
  REQUIRE(state.has_value());
  CHECK(state->version == "2026-07-13T13:00:00Z");
  CHECK(state->status == "todo");
}

TEST_CASE("jira pull flattens a multi-block ADF description with one newline BETWEEN blocks", "[extsync][jira]") {
  // Read off extractADFText: blocks are joined by a single '\n', inline text
  // nodes inside a block are concatenated with NO separator, there is no
  // TRAILING newline, and any node that is not shaped as expected is silently
  // skipped rather than failing the whole pull.
  recording_transport wire;
  wire.reply_body = R"({"key":"P-1","fields":{"summary":"t","description":{"type":"doc","version":1,"content":[)"
                    R"({"type":"paragraph","content":[{"type":"text","text":"first "},{"type":"text","text":"line"}]},)"
                    R"({"type":"paragraph","content":[{"type":"text","text":"second"}]},)"
                    R"({"type":"rule"},)"
                    R"({"type":"paragraph","content":[{"type":"text","text":"third"}]}]}}})";
  jira_adapter const adapter("https://x", bearer("t"), wire);

  auto const state = adapter.pull("P-1");
  REQUIRE(state.has_value());
  // Three blocks contribute; the `rule` block has no `content` array and is
  // skipped WITHOUT contributing a newline of its own.
  CHECK(state->body == "first line\nsecond\nthird");
}

TEST_CASE("jira map_status covers the six names and maps the rest to empty", "[extsync][jira]") {
  using planar::engine::extsync::jira::map_status;
  CHECK(map_status("To Do") == "todo");
  CHECK(map_status("In Progress") == "doing");
  CHECK(map_status("Blocked") == "blocked");
  CHECK(map_status("Done") == "done");
  CHECK(map_status("Cancelled") == "cancelled");
  CHECK(map_status("Won't Do") == "cancelled");
  // EMPTY, not "todo": the sync engine reads an empty remote status as "the
  // remote has no opinion" and skips it, where "todo" would be a real change.
  CHECK(map_status("Backlog").empty());
  CHECK(map_status("").empty());
}

TEST_CASE("jira pull maps a null description and absent assignee to empty", "[extsync][jira]") {
  // The shape the Zig integration fixture actually serves
  // (ext_sync_test.zig's FakeSyncJira sends description/assignee/priority/
  // duedate all as JSON null). A parser that treated null as a type error
  // would fail the whole pull.
  recording_transport wire;
  wire.reply_body =
      R"({"key":"SYNC-1","fields":{"summary":"Baseline","updated":"2026-07-13T13:00:00Z","status":{"name":"To Do"},"description":null,"assignee":null,"priority":null,"duedate":null}})";
  jira_adapter const adapter("https://x", bearer("t"), wire);

  auto const state = adapter.pull("SYNC-1");
  REQUIRE(state.has_value());
  CHECK(state->title == "Baseline");
  CHECK(state->body.empty());
  CHECK(state->assignee.empty());
  CHECK(state->priority == 0);
  CHECK(state->due_at.empty());
}

TEST_CASE("jira pull maps 404 to not_found and other statuses to unexpected_status", "[extsync][jira]") {
  recording_transport wire;
  jira_adapter const  adapter("https://x", bearer("t"), wire);

  wire.reply_status = 404;
  CHECK(err(adapter.pull("PROJ-1")) == std::optional{adapter_error::not_found});
  wire.reply_status = 500;
  CHECK(err(adapter.pull("PROJ-1")) == std::optional{adapter_error::unexpected_status});
  wire.reply_status = 200;
  wire.reply_body   = "not json";
  CHECK(err(adapter.pull("PROJ-1")) == std::optional{adapter_error::parse_failed});
}

TEST_CASE("jira pull reports transport_failed when the send fails", "[extsync][jira]") {
  dead_transport     wire;
  jira_adapter const adapter("https://x", bearer("t"), wire);
  CHECK(err(adapter.pull("PROJ-1")) == std::optional{adapter_error::transport_failed});
}

TEST_CASE("jira push sends NO request for a status-only change", "[extsync][jira]") {
  // jira.zig, "push skips status-only update and sends title update", first
  // half. Asserting the request COUNT is the point — a version that sent an
  // empty {"fields":{}} PUT would return the same empty outcome and pass a
  // return-value-only check.
  recording_transport wire;
  wire.reply_status = 204;
  jira_adapter const adapter("https://acme.atlassian.net", bearer("tok"), wire);

  auto const outcome = adapter.push("PROJ-1", field_change_set{.status = "done"});
  REQUIRE(outcome.has_value());
  CHECK(outcome->fields_applied.empty());
  CHECK(wire.calls == 0);
}

TEST_CASE("jira push PUTs the summary field and reports it applied", "[extsync][jira]") {
  // jira.zig, same test, second half — which checks the substring
  // "\"summary\":\"Updated title\"". The full-payload assertion here is read
  // off the writer sequence in jira.zig's push directly.
  recording_transport wire;
  wire.reply_status = 204;
  jira_adapter const adapter("https://acme.atlassian.net", bearer("tok"), wire);

  auto const outcome = adapter.push("PROJ-1", field_change_set{.title = "Updated title"});
  REQUIRE(outcome.has_value());
  CHECK(outcome->fields_applied == std::vector<std::string>{"title"});
  CHECK(wire.calls == 1);
  CHECK(wire.last_verb == planar::http::method::put);
  CHECK(wire.last_url == "https://acme.atlassian.net/rest/api/3/issue/PROJ-1");
  REQUIRE(wire.last_body.has_value());
  CHECK(*wire.last_body == R"({"fields":{"summary":"Updated title"}})");
  CHECK(wire.header_value("Content-Type") == std::optional<std::string>{"application/json"});
}

TEST_CASE("jira push emits assignee and priority in declaration order", "[extsync][jira]") {
  recording_transport wire;
  wire.reply_status = 204;
  jira_adapter const adapter("https://x", bearer("t"), wire);

  auto const outcome =
      adapter.push("P-1", field_change_set{.title = "T", .status = "done", .assignee = "acc-1", .priority = "High"});
  REQUIRE(outcome.has_value());
  // `status` is absent from fields_applied: Jira transitions are a different
  // endpoint, so the push cannot claim to have applied it.
  CHECK(outcome->fields_applied == std::vector<std::string>{"title", "assignee", "priority"});
  REQUIRE(wire.last_body.has_value());
  CHECK(*wire.last_body == R"({"fields":{"summary":"T","assignee":{"accountId":"acc-1"},"priority":{"name":"High"}}})");
}

TEST_CASE("jira push reports unexpected_status for anything but 204", "[extsync][jira]") {
  recording_transport wire;
  wire.reply_status = 200;
  jira_adapter const adapter("https://x", bearer("t"), wire);
  CHECK(err(adapter.push("P-1", field_change_set{.title = "T"})) == std::optional{adapter_error::unexpected_status});
}

TEST_CASE("jira render applies the default issue type and description placeholder", "[extsync][jira]") {
  // jira.zig, "render applies default issue type and description placeholder"
  // — which checks the substrings "\"name\":\"Story\"" and "(no description)".
  recording_transport wire;
  jira_adapter const  adapter("https://acme.atlassian.net", bearer("t"), wire);

  auto const payload = adapter.render(local_entity{.kind = "task", .id = 42, .title = "New task", .status = "todo"}, {});
  REQUIRE(payload.has_value());
  CHECK(
      *payload ==
      R"json({"fields":{"summary":"New task","issuetype":{"name":"Story"},"description":{"type":"doc","version":1,"content":[{"type":"paragraph","content":[{"type":"text","text":"(no description)"}]}]}}})json");
  CHECK(wire.calls == 0);
}

TEST_CASE("jira render emits the project key only when one is supplied", "[extsync][jira]") {
  recording_transport wire;
  jira_adapter const  adapter("https://x", bearer("t"), wire);

  auto const payload = adapter.render(local_entity{.kind = "task", .id = 1, .title = "T", .body = "B"},
                                      create_options{.issue_type = "Bug", .project = "SYNC"});
  REQUIRE(payload.has_value());
  CHECK(
      *payload ==
      R"({"fields":{"project":{"key":"SYNC"},"summary":"T","issuetype":{"name":"Bug"},"description":{"type":"doc","version":1,"content":[{"type":"paragraph","content":[{"type":"text","text":"B"}]}]}}})");
}

TEST_CASE("jira render escapes operator text through planar.json_text", "[extsync][jira]") {
  recording_transport wire;
  jira_adapter const  adapter("https://x", bearer("t"), wire);

  auto const payload = adapter.render(local_entity{.kind = "task", .id = 1, .title = R"(say "hi")", .body = "a\nb"}, {});
  REQUIRE(payload.has_value());
  CHECK(payload->find(R"("summary":"say \"hi\"")") != std::string::npos);
  CHECK(payload->find(R"("text":"a\nb")") != std::string::npos);
}

TEST_CASE("jira adapter trims one trailing slash from the base URL", "[extsync][jira]") {
  recording_transport wire;
  wire.reply_body = R"({"key":"P-1","fields":{"summary":"t"}})";
  jira_adapter const adapter("https://acme.atlassian.net/", bearer("t"), wire);

  REQUIRE(adapter.pull("P-1").has_value());
  CHECK(wire.last_url == "https://acme.atlassian.net/rest/api/3/issue/P-1");
}

TEST_CASE("jira adapter refuses a bearer credential with no token before sending", "[extsync][jira]") {
  // InvalidAuth, not UnexpectedStatus: the distinction is observable because
  // it is decided before any request leaves.
  recording_transport wire;
  jira_adapter const  adapter("https://x", planar::adapter::auth_credential{.kind = auth_kind::bearer}, wire);

  CHECK(err(adapter.pull("P-1")) == std::optional{adapter_error::invalid_auth});
  CHECK(wire.calls == 0);
}

TEST_CASE("jira adapter sends basic auth as standard padded base64", "[extsync][jira]") {
  recording_transport wire;
  wire.reply_body = R"({"key":"P-1","fields":{"summary":"t"}})";
  jira_adapter const adapter(
      "https://x", planar::adapter::auth_credential{.kind = auth_kind::basic, .user = "dev@example.com", .pass = "tok"}, wire);

  REQUIRE(adapter.pull("P-1").has_value());
  // base64("dev@example.com:tok"), standard alphabet with padding.
  CHECK(wire.header_value("Authorization") == std::optional<std::string>{"Basic ZGV2QGV4YW1wbGUuY29tOnRvaw=="});
}

TEST_CASE("jira basic auth uses the STANDARD base64 alphabet, not the URL-safe one", "[extsync][jira]") {
  // The previous case cannot make this statement: the two alphabets differ
  // ONLY at indices 62 and 63 (`+` `/` versus `-` `_`), and
  // base64("dev@example.com:tok") happens to contain neither. A break-probe
  // swapping in the URL-safe alphabet survived it. `u:?aa~` was chosen because
  // its encoding contains BOTH characters — computed with
  // `python3 -c "import base64; print(base64.b64encode(b'u:?aa~').decode())"`.
  recording_transport wire;
  wire.reply_body = R"({"key":"P-1","fields":{"summary":"t"}})";
  jira_adapter const adapter("https://x", planar::adapter::auth_credential{.kind = auth_kind::basic, .user = "u", .pass = "?aa~"},
                             wire);

  REQUIRE(adapter.pull("P-1").has_value());
  CHECK(wire.header_value("Authorization") == std::optional<std::string>{"Basic dTo/YWF+"});

  // And the padding arms: a 1-byte and a 2-byte tail produce `==` and `=`.
  // base64("u:a") == "dTph", base64("u:ab") == "dTphYg==" ... verified the
  // same way.
  recording_transport one;
  one.reply_body = R"({"key":"P-1","fields":{"summary":"t"}})";
  jira_adapter const pad_two("https://x", planar::adapter::auth_credential{.kind = auth_kind::basic, .user = "u", .pass = "a"},
                             one);
  REQUIRE(pad_two.pull("P-1").has_value());
  CHECK(one.header_value("Authorization") == std::optional<std::string>{"Basic dTph"});

  recording_transport two;
  two.reply_body = R"({"key":"P-1","fields":{"summary":"t"}})";
  jira_adapter const pad_one("https://x", planar::adapter::auth_credential{.kind = auth_kind::basic, .user = "u", .pass = "ab"},
                             two);
  REQUIRE(pad_one.pull("P-1").has_value());
  CHECK(two.header_value("Authorization") == std::optional<std::string>{"Basic dTphYg=="});
}

TEST_CASE("jira basic auth refuses a credential missing user or pass", "[extsync][jira]") {
  recording_transport wire;
  jira_adapter const  no_pass("https://x", planar::adapter::auth_credential{.kind = auth_kind::basic, .user = "u"}, wire);
  CHECK(err(no_pass.pull("P-1")) == std::optional{adapter_error::invalid_auth});
  CHECK(wire.calls == 0);
}

TEST_CASE("jira adapter works over a real loopback HTTP round trip", "[extsync][jira][loopback]") {
  // The one case in this file that opens a socket. It exists because every
  // other case above bypasses `curl_transport` entirely, so none of them can
  // say the adapter works over HTTP at all — only that it formats requests
  // correctly. Bound to 127.0.0.1 on a kernel-assigned port; see
  // fixture_server.hpp's header for why that is structurally offline.
  std::mutex  lock;
  std::string seen_target;
  std::string seen_auth;
  std::string seen_put_body;

  planar::http::fixture::server server([&](const planar::http::fixture::captured_request& req) {
    std::scoped_lock const guard(lock);
    seen_target = req.target;
    seen_auth   = req.header_value("authorization").value_or("");
    if (req.verb == "PUT") {
      seen_put_body = req.body;
      return planar::http::fixture::canned_response{.status = 204, .body = ""};
    }
    return planar::http::fixture::canned_response{
        .status = 200,
        .body =
            R"({"key":"SYNC-1","fields":{"summary":"Baseline","updated":"2026-07-13T13:00:00Z","status":{"name":"To Do"},"description":null,"assignee":null,"priority":null,"duedate":null}})"};
  });

  planar::http::curl_transport wire;
  jira_adapter const           adapter(server.base_url(), bearer("test-token"), wire);

  auto const state = adapter.pull("SYNC-1");
  REQUIRE(state.has_value());
  CHECK(state->title == "Baseline");
  CHECK(state->status == "todo");
  CHECK(state->version == "2026-07-13T13:00:00Z");
  {
    std::scoped_lock const guard(lock);
    CHECK(seen_target == "/rest/api/3/issue/SYNC-1");
    CHECK(seen_auth == "Bearer test-token");
  }

  auto const outcome = adapter.push("SYNC-1", field_change_set{.title = "Local edit"});
  REQUIRE(outcome.has_value());
  CHECK(outcome->fields_applied == std::vector<std::string>{"title"});
  {
    std::scoped_lock const guard(lock);
    CHECK(seen_put_body == R"({"fields":{"summary":"Local edit"}})");
  }
  CHECK(server.request_count() == 2);
}
