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

} // namespace planar::http
