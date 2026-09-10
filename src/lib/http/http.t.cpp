// @file http.t.cpp
// @brief Tests for `planar.http` (plan 996, task 6041).
//
// HOME SAFETY. Nothing here opens a database, reads PLANAR_DB / PLANAR_HOME,
// or constructs a path. The only external resource touched is a socket bound
// to 127.0.0.1 on a kernel-assigned port by `fixture_server.hpp`.
//
// NETWORK SAFETY. Every request below is sent to `server.base_url()`, which
// is `http://127.0.0.1:<ephemeral>`. No test in this file names a real host,
// and the one test that deliberately fails a send targets a loopback port
// that was just closed. Grep this file for "https://" — the only occurrences
// are inside `url_encode_query` inputs, which are never sent anywhere.
//
// ORACLE PROVENANCE. Three separate contracts are pinned here and they have
// different oracles:
//
//   1. `method_to_text` / `url_encode_query` are ports of Zig functions with
//      the same names in zig/src/engine/extsync/common.zig. `urlEncodeQuery`
//      carries a Zig unit test asserting
//      "repo:acme/api is:open" -> "repo%3Aacme%2Fapi%20is%3Aopen"; that exact
//      pair is re-asserted below, and it is what pins BOTH the uppercase-hex
//      choice and the %20-not-+ choice.
//   2. The 30-second timeout is the M7 acceptance criterion's own wording
//      ("30s timeouts"), not something read off the oracle binary.
//   3. `curl_transport`'s behavior has no Zig counterpart to diff against —
//      the Zig side used `std.http.Client` — so it is pinned against
//      observable HTTP: what the fixture server actually received.

import std;
import planar.http;

#include "fixture_server.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

using planar::http::fixture::canned_response;
using planar::http::fixture::captured_request;

} // namespace

TEST_CASE("method_to_text renders the uppercase wire verb", "[http]") {
  CHECK(planar::http::method_to_text(planar::http::method::get) == "GET");
  CHECK(planar::http::method_to_text(planar::http::method::post) == "POST");
  CHECK(planar::http::method_to_text(planar::http::method::put) == "PUT");
  CHECK(planar::http::method_to_text(planar::http::method::patch) == "PATCH");
}

TEST_CASE("url_encode_query percent-encodes with uppercase hex and %20 for space", "[http]") {
  // The exact pair the Zig original's own unit test asserts.
  CHECK(planar::http::url_encode_query("repo:acme/api is:open") == "repo%3Aacme%2Fapi%20is%3Aopen");
  // Unreserved set passes through untouched.
  CHECK(planar::http::url_encode_query("aZ09-_.~") == "aZ09-_.~");
  // Uppercase hex, not lowercase: a lowercase table would render "%7b".
  CHECK(planar::http::url_encode_query("{") == "%7B");
}

TEST_CASE("curl_transport defaults to the 30-second M7 timeout", "[http]") {
  planar::http::curl_transport const wire;
  CHECK(wire.timeout() == std::chrono::seconds{30});
}

TEST_CASE("curl_transport GETs from the loopback fixture and returns status and body", "[http]") {
  planar::http::fixture::server server([](const captured_request& req) {
    return canned_response{.status = 200, .body = std::format("{{\"seen\":\"{}\"}}", req.target)};
  });

  planar::http::curl_transport wire;
  auto const got = wire.send({.verb = planar::http::method::get, .url = server.base_url() + "/rest/api/3/issue/PROJ-1"});

  REQUIRE(got.has_value());
  CHECK(got->status == 200);
  CHECK(got->body == "{\"seen\":\"/rest/api/3/issue/PROJ-1\"}");
  CHECK(server.request_count() == 1);
}

TEST_CASE("curl_transport sends the request headers it was given", "[http]") {
  std::mutex                                       lock;
  std::vector<std::pair<std::string, std::string>> seen;
  planar::http::fixture::server                    server([&](const captured_request& req) {
    std::scoped_lock const guard(lock);
    seen = req.headers;
    return canned_response{.status = 200, .body = "{}"};
  });

  planar::http::curl_transport wire;
  auto const                   got = wire.send({
      .verb    = planar::http::method::get,
      .url     = server.base_url() + "/x",
      .headers = {{.name = "Authorization", .value = "Bearer sekrit"}, {.name = "Accept", .value = "application/json"}},
  });
  REQUIRE(got.has_value());

  std::scoped_lock const guard(lock);
  auto const             find = [&](std::string_view name) -> std::optional<std::string> {
    for (auto const& [key, value] : seen) {
      if (key == name) {
        return value;
      }
    }
    return std::nullopt;
  };
  CHECK(find("authorization") == std::optional<std::string>{"Bearer sekrit"});
  CHECK(find("accept") == std::optional<std::string>{"application/json"});
}

TEST_CASE("curl_transport sends PUT and PATCH bodies under the right verb", "[http]") {
  std::mutex  lock;
  std::string verb;
  std::string body;

  planar::http::fixture::server server([&](const captured_request& req) {
    std::scoped_lock const guard(lock);
    verb = req.verb;
    body = req.body;
    return canned_response{.status = 204, .body = ""};
  });

  planar::http::curl_transport wire;

  auto const put = wire.send({
      .verb = planar::http::method::put,
      .url  = server.base_url() + "/issue/PROJ-1",
      .body = std::string(R"({"fields":{"summary":"t"}})"),
  });
  REQUIRE(put.has_value());
  CHECK(put->status == 204);
  {
    std::scoped_lock const guard(lock);
    CHECK(verb == "PUT");
    CHECK(body == R"({"fields":{"summary":"t"}})");
  }

  auto const patched = wire.send({
      .verb = planar::http::method::patch,
      .url  = server.base_url() + "/issues/7",
      .body = std::string(R"({"title":"t"})"),
  });
  REQUIRE(patched.has_value());
  {
    std::scoped_lock const guard(lock);
    CHECK(verb == "PATCH");
    CHECK(body == R"({"title":"t"})");
  }
}

TEST_CASE("curl_transport delivers a body larger than one socket read", "[http]") {
  // The fixture server reads until the header terminator, then keeps reading
  // until `Content-Length` bytes of body have arrived. A small body lands in
  // the SAME read as the headers, so every other case in this file passes
  // whether or not that second loop exists at all — a break-probe that set
  // `content_length = 0` survived them all. This case makes the body span
  // several reads (the fixture reads in 4096-byte chunks), so the loop is the
  // only thing that can deliver it whole.
  //
  // It also proves the RESPONSE side reassembles: 200 KB comes back through
  // libcurl's write callback in many chunks.
  std::string const big_request(200000, 'q');
  std::mutex        lock;
  std::size_t       seen_request_bytes = 0;

  planar::http::fixture::server server([&](const captured_request& req) {
    {
      std::scoped_lock const guard(lock);
      seen_request_bytes = req.body.size();
    }
    return canned_response{.status = 200, .body = std::string(200000, 'r')};
  });

  planar::http::curl_transport wire;
  auto const                   got = wire.send({
      .verb = planar::http::method::put,
      .url  = server.base_url() + "/big",
      .body = big_request,
  });
  REQUIRE(got.has_value());
  CHECK(got->body.size() == 200000);
  CHECK(got->body == std::string(200000, 'r'));
  std::scoped_lock const guard(lock);
  CHECK(seen_request_bytes == big_request.size());
}

TEST_CASE("curl_transport reports a non-2xx status as a response, not a failure", "[http]") {
  // The status/error split is a contract, not an implementation detail: every
  // adapter distinguishes 404 (NotFound) from 500 (UnexpectedStatus) from a
  // dead socket (TransportFailed), and it can only do that if the transport
  // hands non-2xx back as an ordinary response.
  planar::http::fixture::server server(
      [](const captured_request&) { return canned_response{.status = 404, .body = R"({"errorMessages":["no"]})"}; });

  planar::http::curl_transport wire;
  auto const                   got = wire.send({.verb = planar::http::method::get, .url = server.base_url() + "/missing"});
  REQUIRE(got.has_value());
  CHECK(got->status == 404);
  CHECK(got->body == R"({"errorMessages":["no"]})");
}

TEST_CASE("curl_transport reports send_failed when nothing is listening", "[http]") {
  // Learn a port that WAS bound, then let the server go out of scope so
  // nothing is listening on it. Still loopback — this never leaves the host.
  std::string dead_url;
  {
    planar::http::fixture::server const server([](const captured_request&) { return canned_response{}; });
    dead_url = server.base_url() + "/gone";
  }

  planar::http::curl_transport wire{std::chrono::seconds{2}};
  auto const                   got = wire.send({.verb = planar::http::method::get, .url = dead_url});
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error() == planar::http::transport_error::send_failed);
}
