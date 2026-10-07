/// @file http.cpp
/// @brief Implementation of `planar.http`. See http.cppm for the layering
/// argument and the timeout contract.

module;

#include <curl/curl.h>

module planar.http;

import std;

namespace planar::http {

namespace {

/// @brief libcurl's `CURLOPT_WRITEFUNCTION` sink: append into a std::string.
/// @param data The received bytes.
/// @param size Element size (always 1 for libcurl).
/// @param nmemb Element count.
/// @param user The `std::string*` passed as `CURLOPT_WRITEDATA`.
/// @return The number of bytes consumed; anything else aborts the transfer.
auto write_body(char* data, std::size_t size, std::size_t nmemb, void* user) -> std::size_t {
  auto* const out   = static_cast<std::string*>(user);
  auto const  bytes = size * nmemb;
  out->append(data, bytes);
  return bytes;
}

/// @brief Run libcurl's global initialization exactly once per process.
///
/// `curl_easy_init` will do this implicitly, but implicitly it is NOT
/// thread-safe (libcurl's own documentation says so). Doing it under a
/// `once_flag` makes the first-use race impossible even though nothing in
/// this tree currently constructs two transports concurrently.
auto ensure_global_init() -> void {
  static std::once_flag flag;
  std::call_once(flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

/// @brief Lowercase an ASCII string.
auto ascii_lower(std::string_view in) -> std::string {
  std::string out(in);
  for (auto& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

/// @brief Whether `host` is a plain DNS name or dotted IPv4 literal.
auto plain_host(std::string_view host) -> bool {
  if (host.empty() || host.front() == '.' || host.back() == '.' || host.front() == '-') {
    return false;
  }
  return std::ranges::all_of(host, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
  });
}

/// @brief Whether `port` is 1..65535 in digits only.
auto valid_port(std::string_view port) -> bool {
  if (port.empty() || port.size() > 5 || !std::ranges::all_of(port, [](char c) { return c >= '0' && c <= '9'; })) {
    return false;
  }
  auto const value = std::stoul(std::string(port));
  return value >= 1 && value <= 65535;
}

/// @brief A scope guard owning one easy handle.
struct easy_handle {
  CURL* handle                               = curl_easy_init();
  easy_handle()                              = default;
  easy_handle(const easy_handle&)            = delete;
  easy_handle& operator=(const easy_handle&) = delete;
  ~easy_handle() {
    if (handle != nullptr) {
      curl_easy_cleanup(handle);
    }
  }
};

} // namespace

transport::~transport() = default;

auto method_to_text(method m) -> std::string_view {
  switch (m) {
  case method::get:
    return "GET";
  case method::post:
    return "POST";
  case method::put:
    return "PUT";
  case method::patch:
    return "PATCH";
  }
  return "GET";
}

auto url_encode_query(std::string_view s) -> std::string {
  constexpr std::string_view k_hex = "0123456789ABCDEF";
  std::string                out;
  out.reserve(s.size());
  for (char const raw : s) {
    auto const c = static_cast<unsigned char>(raw);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      out.push_back(static_cast<char>(c));
      continue;
    }
    if (c == ' ') {
      out += "%20";
      continue;
    }
    out.push_back('%');
    out.push_back(k_hex[c >> 4U]);
    out.push_back(k_hex[c & 0x0FU]);
  }
  return out;
}

curl_transport::curl_transport(std::chrono::seconds timeout) : _timeout(timeout) {
  ensure_global_init();
}

auto curl_transport::timeout() const noexcept -> std::chrono::seconds {
  return _timeout;
}

auto curl_transport::send(const request& req) -> std::expected<response, transport_error> {
  CURL* const handle = curl_easy_init();
  if (handle == nullptr) {
    return std::unexpected(transport_error::send_failed);
  }
  // Every early return below has to release both the handle and the header
  // list, and there are several; a scope guard is cheaper to get right than
  // repeating two cleanups per branch.
  curl_slist* header_list = nullptr;
  struct cleanup {
    CURL*        handle_;
    curl_slist** list_;
    ~cleanup() {
      if (*list_ != nullptr) {
        curl_slist_free_all(*list_);
      }
      curl_easy_cleanup(handle_);
    }
  } const guard{.handle_ = handle, .list_ = &header_list};

  std::string body_out;
  auto const  url = std::string(req.url);

  curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &write_body);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body_out);
  // Both halves of the 30-second contract — see http.cppm's header for why
  // neither one alone is a 30-second bound.
  curl_easy_setopt(handle, CURLOPT_TIMEOUT, static_cast<long>(_timeout.count()));
  curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, static_cast<long>(_timeout.count()));
  // No redirect following: an Authorization header must not be replayed to a
  // host the response chose. See http.cppm's header.
  curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
  // libcurl installs signal handlers for its own DNS timeout otherwise, which
  // is unsafe in a multi-threaded process (and this transport is constructed
  // from test threads).
  curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle, CURLOPT_USERAGENT, "planar/1.0");

#ifdef PLANAR_PORTABLE_LINUX_TLS
  // CPM passes CURL_CA_BUNDLE=none as a scoped normal variable. curl 8.7.1
  // removes only its cache entry, leaving a literal "none" in curl_config.h.
  // Clear that filename so the configured host CA directory can be used.
  curl_easy_setopt(handle, CURLOPT_CAINFO, static_cast<char const*>(nullptr));
  // curl's OpenSSL fallback is skipped when a CA directory is configured.
  // Red Hat hosts keep their trust bundle here instead of hashed CA files.
  constexpr char  k_redhat_ca_bundle[] = "/etc/pki/tls/certs/ca-bundle.crt";
  std::error_code ca_error;
  if (std::filesystem::is_regular_file(k_redhat_ca_bundle, ca_error)) {
    curl_easy_setopt(handle, CURLOPT_CAINFO, k_redhat_ca_bundle);
  }
#endif

  switch (req.verb) {
  case method::get:
    curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
    break;
  case method::post:
    curl_easy_setopt(handle, CURLOPT_POST, 1L);
    break;
  case method::put:
  case method::patch:
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, std::string(method_to_text(req.verb)).c_str());
    break;
  }
  // CURLOPT_CUSTOMREQUEST above only renames the verb; the body still has to
  // be supplied, and POSTFIELDS is the right vehicle for all three
  // body-carrying verbs here.
  std::string const payload = req.body.value_or(std::string{});
  if (req.body.has_value()) {
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, payload.c_str());
  } else if (req.verb == method::post) {
    // A POST with no body still needs an explicit zero length, or libcurl
    // waits to read one from the (absent) read callback.
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, 0L);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, "");
  }

  for (auto const& field : req.headers) {
    auto const line = std::format("{}: {}", field.name, field.value);
    header_list     = curl_slist_append(header_list, line.c_str());
    if (header_list == nullptr) {
      return std::unexpected(transport_error::send_failed);
    }
  }
  if (header_list != nullptr) {
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, header_list);
  }

  auto const result = curl_easy_perform(handle);
  if (result == CURLE_PEER_FAILED_VERIFICATION) {
    return std::unexpected(transport_error::certificate_verification_failed);
  }
  if (result != CURLE_OK) {
    return std::unexpected(transport_error::send_failed);
  }
  long status = 0;
  if (curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK) {
    return std::unexpected(transport_error::send_failed);
  }
  return response{.status = static_cast<std::uint16_t>(status), .body = std::move(body_out)};
}

auto check_download_url(std::string_view url, std::optional<url_scheme> previous) -> std::expected<url_scheme, std::string> {
  auto const refuse = [&](std::string_view why) -> std::expected<url_scheme, std::string> {
    return std::unexpected(std::format("{}: {}", why, url));
  };
  if (std::ranges::any_of(url, [](char c) {
        auto const u = static_cast<unsigned char>(c);
        return u <= 0x20 || u == 0x7F || c == '\\';
      })) {
    return refuse("url contains whitespace, control or backslash characters");
  }
  auto const marker = url.find("://");
  if (marker == std::string_view::npos) {
    return refuse("url has no scheme");
  }
  auto const scheme_text = ascii_lower(url.substr(0, marker));
  auto const rest        = url.substr(marker + 3);
  auto const authority   = rest.substr(0, rest.find_first_of("/?#"));
  if (authority.find('@') != std::string_view::npos) {
    return refuse("url carries userinfo");
  }
  std::string_view host = authority;
  std::string_view port;
  bool             has_port = false;
  if (auto const colon = authority.find(':'); colon != std::string_view::npos) {
    host     = authority.substr(0, colon);
    port     = authority.substr(colon + 1);
    has_port = true;
  }
  if (has_port && !valid_port(port)) {
    return refuse("url has a malformed port");
  }

  url_scheme scheme{};
  if (scheme_text == "https") {
    scheme = url_scheme::https;
    if (!plain_host(host)) {
      return refuse("https url has a malformed host");
    }
  } else if (scheme_text == "http") {
    scheme = url_scheme::http;
    if (host != "127.0.0.1" && host != "localhost") {
      return refuse("http is allowed only for the exact hosts 127.0.0.1 and localhost");
    }
  } else if (scheme_text == "file") {
    scheme = url_scheme::file;
    if (has_port || (!host.empty() && host != "localhost")) {
      return refuse("file url names a remote authority");
    }
  } else {
    return refuse("url scheme is not https, http or file");
  }

  if (previous.has_value()) {
    if (scheme == url_scheme::file) {
      return refuse("redirect to a file url");
    }
    if (*previous == url_scheme::https && scheme != url_scheme::https) {
      return refuse("redirect downgrades https");
    }
  }
  return scheme;
}

auto download(std::string_view url, const download_policy& policy) -> std::expected<download_result, download_error> {
  ensure_global_init();
  std::string               current(url);
  std::optional<url_scheme> previous;
  std::uint32_t             hops = 0;

  while (true) {
    auto const checked = check_download_url(current, previous);
    if (!checked) {
      return std::unexpected(
          download_error{.kind = previous.has_value() ? download_error_kind::redirect_refused : download_error_kind::invalid_url,
                         .url  = current,
                         .message = checked.error()});
    }
    easy_handle easy;
    if (easy.handle == nullptr) {
      return std::unexpected(download_error{.kind    = download_error_kind::transport_failed,
                                            .url     = current,
                                            .message = std::format("could not start a transfer for {}", current)});
    }
    auto* const handle = easy.handle;
    std::string body_out;
    curl_easy_setopt(handle, CURLOPT_URL, current.c_str());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &write_body);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body_out);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, static_cast<long>(policy.total_timeout.count()));
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, static_cast<long>(policy.connect_timeout.count()));
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, static_cast<long>(policy.low_speed_limit));
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, static_cast<long>(policy.low_speed_time.count()));
    // Redirects are followed by this loop, never by curl: curl cannot apply
    // check_download_url to each hop. No CURLOPT_HTTPHEADER is ever set, so no
    // Authorization header exists to leak.
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http,https,file");
    curl_easy_setopt(handle, CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED));
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "planar/1.0");
    curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
#ifdef PLANAR_PORTABLE_LINUX_TLS
    curl_easy_setopt(handle, CURLOPT_CAINFO, static_cast<char const*>(nullptr));
    constexpr char  k_redhat_ca_bundle[] = "/etc/pki/tls/certs/ca-bundle.crt";
    std::error_code ca_error;
    if (std::filesystem::is_regular_file(k_redhat_ca_bundle, ca_error)) {
      curl_easy_setopt(handle, CURLOPT_CAINFO, k_redhat_ca_bundle);
    }
#endif

    auto const result = curl_easy_perform(handle);
    if (result == CURLE_OPERATION_TIMEDOUT) {
      return std::unexpected(download_error{
          .kind = download_error_kind::timeout, .url = current, .message = std::format("download of {} timed out", current)});
    }
    if (result == CURLE_PEER_FAILED_VERIFICATION) {
      return std::unexpected(download_error{.kind    = download_error_kind::certificate_verification_failed,
                                            .url     = current,
                                            .message = std::format("certificate verification failed for {}", current)});
    }
    if (result != CURLE_OK) {
      return std::unexpected(
          download_error{.kind    = download_error_kind::transport_failed,
                         .url     = current,
                         .message = std::format("download of {} failed: {}", current, curl_easy_strerror(result))});
    }
    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    if (*checked == url_scheme::file) {
      status = 200;
    }
    char* location = nullptr;
    if (status >= 300 && status < 400) {
      curl_easy_getinfo(handle, CURLINFO_REDIRECT_URL, &location);
    }
    if (location == nullptr) {
      return download_result{
          .status = static_cast<std::uint16_t>(status), .body = std::move(body_out), .final_url = current, .redirects = hops};
    }
    if (hops >= policy.max_redirects) {
      return std::unexpected(
          download_error{.kind    = download_error_kind::too_many_redirects,
                         .url     = current,
                         .message = std::format("more than {} redirects fetching {}", policy.max_redirects, std::string(url))});
    }
    ++hops;
    previous = *checked;
    current  = location;
  }
}

} // namespace planar::http
