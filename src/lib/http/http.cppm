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
///
/// ## The download policy is a separate entry point
///
/// `download()` is the policy for fetching release assets (`planar update`).
/// It follows redirects, because GitHub answers release URLs with 302s to its
/// object store, and it is safe to do so because it has no way to set a
/// header: no `Authorization` header is ever sent. libcurl's own
/// `FOLLOWLOCATION` cannot check the authority of each hop, so redirects are
/// followed by hand: the first URL and every `Location` target pass
/// `check_download_url()` before curl sees them, and an HTTPS chain can never
/// step down to HTTP or `file`. The whole request is bounded by 10 minutes
/// plus a low-speed abort, instead of the adapter's 30 seconds.
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

/// @brief The scheme of a URL accepted by the download policy.
export enum class url_scheme : std::uint8_t {
  https, ///< Production transport.
  http,  ///< Fixture transport; loopback hosts only.
  file,  ///< Fixture transport; local files only.
};

/// @brief Validate one URL against the download policy's authority and protocol rules.
///
/// Accepts `https://host[:port]/...` for any plain DNS or IPv4 host, `http://`
/// only with the exact host `127.0.0.1` or `localhost`, and `file://` only with
/// an empty authority or exactly `localhost`. The port, when present, is
/// digits only and 1..65535. Userinfo, bracketed hosts, whitespace, control
/// characters, backslashes, percent-encoded hosts and misleading suffixes such
/// as `localhost.evil.com` are refused.
/// @param url The absolute URL.
/// @param previous The scheme of the hop that produced `url` as a redirect
/// target, or unset for the initial URL. A redirect to `file` is always
/// refused, and a hop that follows `https` must itself be `https`.
/// @return The URL's scheme, or a message naming the refusal.
export auto check_download_url(std::string_view url, std::optional<url_scheme> previous = std::nullopt)
    -> std::expected<url_scheme, std::string>;

/// @brief Bounds for one `download`.
export struct download_policy {
  std::chrono::seconds total_timeout{600};     ///< One deadline for the whole redirect chain; each hop gets the time left.
  std::chrono::seconds connect_timeout{30};    ///< Connection establishment bound per hop.
  std::uint32_t        low_speed_limit = 1024; ///< Bytes per second below which the transfer counts as stalled.
  std::chrono::seconds low_speed_time{60};     ///< How long the rate may stay below `low_speed_limit` before abort.
  std::uint32_t        max_redirects = 10;     ///< Redirect hops followed before giving up.
};

/// @brief Why a `download` failed.
export enum class download_error_kind : std::uint8_t {
  invalid_url,                     ///< The initial URL failed `check_download_url`.
  redirect_refused,                ///< A redirect target failed `check_download_url`.
  too_many_redirects,              ///< The chain exceeded `max_redirects`.
  timeout,                         ///< The total bound or the low-speed bound fired.
  certificate_verification_failed, ///< The peer certificate could not be verified.
  transport_failed,                ///< Any other failure to obtain a response.
};

/// @brief A failed `download`.
export struct download_error {
  download_error_kind kind = download_error_kind::transport_failed; ///< What went wrong.
  std::string         url;     ///< The URL being fetched when it went wrong (the refused target for a refused redirect).
  std::string         message; ///< A human-readable explanation that names the URL.
};

/// @brief A completed `download`.
export struct download_result {
  std::uint16_t status = 0;    ///< The final HTTP status (200 for a read `file://` fixture).
  std::string   body;          ///< The whole final body.
  std::string   final_url;     ///< The URL that produced the body.
  std::uint32_t redirects = 0; ///< How many redirects were followed.
};

/// @brief GET `url` under the download policy.
///
/// The entry point for release-asset downloads. Redirects are followed with
/// every hop validated by `check_download_url`; no header is ever sent, so no
/// `Authorization`; the request obeys `policy`'s total and low-speed bounds. A
/// non-2xx final status is a result, not an error, as with `curl_transport`.
/// Adapters must keep using `curl_transport`, which does not follow redirects.
/// @param url The absolute initial URL.
/// @param policy The bounds to apply.
/// @return The final response, or the failure.
export auto download(std::string_view url, const download_policy& policy = {}) -> std::expected<download_result, download_error>;

/// @brief Percent-encode `s` for use inside a URL query component.
///
/// Port of the Zig original's `urlEncodeQuery`, including its one deliberate
/// oddity: a space becomes `%20`, not `+`. The unreserved set is
/// `A-Z a-z 0-9 - _ . ~`; everything else becomes `%XX` with UPPERCASE hex.
/// @param s The raw bytes.
/// @return The encoded form.
export auto url_encode_query(std::string_view s) -> std::string;

} // namespace planar::http
