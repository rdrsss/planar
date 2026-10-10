/// @file http.cppm
/// @brief `planar.http` — the HTTP request/response vocabulary and the
/// libcurl-backed transport every external-plane adapter sends through
/// (plan 996, task 6041).
///
/// Behavior-preserving port (D2) of the transport half of
/// `zig/src/engine/extsync/common.zig` (`Method`, `Header`, `Request`,
/// `Response`, `Transport`) plus the `std.http.Client` construction the Zig
/// `adapter_factory` did inline.
///
/// ## Why this is a LAYER-1 module and not part of an engine bucket
///
/// Two layer-2 buckets need this vocabulary and D18 forbids an edge between
/// them: `engine_extsync` (the Jira/GitHub adapters, which SEND requests) and
/// — indirectly, through `planar.adapter` — `engine_external` (the sync
/// engine, which drives an adapter). Same shape as `scope_ref` and
/// `json_text` before it (D19): a primitive two engine buckets share moves
/// DOWN a layer rather than being duplicated or reached for sideways.
///
/// ## `transport` is a virtual interface, not a comptime duck type
///
/// The Zig original threads adapters through `extsync.dispatch`, a comptime
/// `@hasDecl` check over an `anytype` — there is no runtime interface at all,
/// and `Transport` itself is a hand-rolled `*anyopaque` + function-pointer
/// pair because Zig has no other way to erase the concrete transport. C++ has
/// one: an abstract base class. That is the whole of the divergence. The
/// observable contract (a `send` that takes a request and returns a status +
/// body, or fails) is identical, and it is what makes the in-process fixture
/// server in `fixture_server.hpp` and the recording stubs in the adapter tests
/// substitutable for `curl_transport` without the adapters knowing.
///
/// ## The 30-second timeout is the task's contract, not a default
///
/// `k_default_timeout` is 30 seconds and `curl_transport` applies it as BOTH
/// `CURLOPT_TIMEOUT` (whole-request wall clock) and `CURLOPT_CONNECTTIMEOUT`
/// (connection establishment). One without the other is not a 30-second
/// bound: a connect that never completes is not covered by `CURLOPT_TIMEOUT`
/// on every backend/version combination, and a connect bound alone says
/// nothing about a server that accepts and then stalls. Both are set.
///
/// ## Redirects are NOT followed
///
/// `CURLOPT_FOLLOWLOCATION` is deliberately left off. The Zig original's
/// `std.http.Client.fetch` call sites do not enable redirect following
/// either, and a silently-followed redirect would send the `Authorization`
/// header this transport carries to whatever host the redirect names.
module;

export module planar.http;

import std;

namespace planar::http {

/// @brief The HTTP verbs the adapter boundary uses. Mirrors the Zig
/// original's `Method` exactly — there is no `DELETE`, because no adapter
/// operation issues one.
export enum class method : std::uint8_t {
  get,
  post,
  put,
  patch,
};

/// @brief The wire spelling of a method.
/// @param m The method.
/// @return The uppercase verb.
export auto method_to_text(method m) -> std::string_view;

/// @brief One request or response header.
export struct header {
  std::string name;  ///< The field name.
  std::string value; ///< The field value.
};

/// @brief An outbound HTTP request.
export struct request {
  method                     verb = method::get; ///< The HTTP verb.
  std::string                url;                ///< The absolute request URL.
  std::vector<header>        headers;            ///< Request headers, sent in order.
  std::optional<std::string> body;               ///< The request body, when the verb carries one.
};

/// @brief An inbound HTTP response.
///
/// Only the status and the body are modelled, because that is all the Zig
/// original's `Response` carries and all any adapter reads.
export struct response {
  std::uint16_t status = 0; ///< The HTTP status code.
  std::string   body;       ///< The whole response body.
};

/// @brief Why a `send` failed to produce a response at all.
///
/// Certificate verification failures retain their cause for operator
/// diagnostics. Other failures keep the generic transport error.
export enum class transport_error : std::uint8_t {
  send_failed,                     ///< No response was obtained (DNS, connect, timeout, TLS, write).
  certificate_verification_failed, ///< The peer certificate could not be verified.
};

/// @brief The abstract transport an adapter sends through.
export class transport {
public:
  transport()                            = default;
  transport(const transport&)            = delete;
  transport& operator=(const transport&) = delete;
  transport(transport&&)                 = delete;
  transport& operator=(transport&&)      = delete;
  virtual ~transport();

  /// @brief Perform one request.
  /// @param req The request to send.
  /// @return The response, or a transport error. A non-2xx
  /// status is a RESPONSE, not an error — status interpretation belongs to
  /// the adapter, exactly as in the Zig original.
  virtual auto send(const request& req) -> std::expected<response, transport_error> = 0;
};

/// @brief The whole-request and connect timeout every adapter runs under.
///
/// 30 seconds, per the M7 acceptance criterion. See this module's header for
/// why it is applied to both curl options rather than one.
export inline constexpr std::chrono::seconds k_default_timeout{30};

/// @brief A `transport` backed by the vendored libcurl.
///
/// Each `send` runs on its own easy handle, so one instance is safe to reuse
/// serially. It is NOT safe to share across threads.
export class curl_transport final : public transport {
private:
  std::chrono::seconds _timeout;

public:
  /// @brief Construct a transport.
  ///
  /// Also performs libcurl's one-time global initialization, exactly once per
  /// process, under a `std::once_flag`.
  /// @param timeout The whole-request and connect timeout. Defaults to
  /// `k_default_timeout`; a caller overriding it is opting out of the M7
  /// contract and should say why at the call site.
  explicit curl_transport(std::chrono::seconds timeout = k_default_timeout);

  /// @brief Perform one request over libcurl.
  /// @param req The request to send.
  /// @return The response, or a transport error.
  auto send(const request& req) -> std::expected<response, transport_error> override;

  /// @brief The configured timeout.
  /// @return The whole-request and connect timeout in seconds.
  [[nodiscard]] auto timeout() const noexcept -> std::chrono::seconds;
};

/// @brief Percent-encode `s` for use inside a URL query component.
///
/// Port of the Zig original's `urlEncodeQuery`, including its one deliberate
/// oddity: a space becomes `%20`, not `+`. The unreserved set is
/// `A-Z a-z 0-9 - _ . ~`; everything else becomes `%XX` with UPPERCASE hex.
/// @param s The raw bytes.
/// @return The encoded form.
export auto url_encode_query(std::string_view s) -> std::string;

} // namespace planar::http
