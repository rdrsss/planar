// @file fixture_server.hpp
// @brief An in-process HTTP/1.1 fixture server bound to the loopback
// interface, for tests that must exercise a real HTTP round trip without
// touching the network (plan 996, task 6041).
//
// ## What this replaces, and why it is not a transport stub
//
// The Zig tree tests the external plane at two levels, and the distinction is
// load-bearing:
//
//   * the ADAPTER unit tests (`zig/src/engine/extsync/{jira,github}.zig`)
//     substitute a `FakeTransport` that never opens a socket — enough to pin
//     URL shapes, header sets and body encodings;
//   * the SYNC integration test (`zig/integration_tests/ext_sync_test.zig`)
//     stands up a real `std.http.Server` on `127.0.0.1:0` in a background
//     thread and drives the actual binary against it — which is what proves
//     the HTTP CLIENT works at all, not just that the adapter formats a
//     request correctly.
//
// A transport stub cannot make the second statement: it bypasses every line
// of `curl_transport`. This header is the C++ equivalent of the second one.
// Both levels exist here for the same reason they exist there.
//
// ## No test that uses this can reach the network, structurally
//
// The listening socket is bound to `INADDR_LOOPBACK` (127.0.0.1) with port 0,
// so the kernel assigns an ephemeral port on the loopback interface only —
// the socket is not reachable from off-host and, more to the point, `port()`
// / `base_url()` are the ONLY way a caller learns where to send. A test using
// this server has no hostname to resolve and no route off the machine. That
// is the structural half of the guarantee; the other half is that no
// `*.t.cpp` in this tree names a real remote host, which its own comments
// state and which is checkable by grep.
//
// ## Why a HEADER and not a module or a library
//
// Same argument `parity_harness.hpp` and `catalog_parity.hpp` next door make
// in full: this is test-only scaffolding used by `*.t.cpp` in more than one
// module, D18 forbids the sideways target edge that sharing it through a
// library would create in the engine layer, and putting test scaffolding in
// the shipped layer-1 libraries would be wrong regardless of legality. A
// plain header creates NO target edge — each test target compiles its own
// copy. Nothing that ships includes it.
#pragma once

// Included from module-importing test translation units, so it must not
// `#include` any standard library header — `import std;` at the top of the
// includer provides everything used below. The POSIX socket headers are C
// headers and have no such constraint (same posture parity_harness.hpp takes
// with <sys/wait.h>).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace planar::http::fixture {

/// @brief One request the fixture server received, as the handler sees it.
struct captured_request {
  std::string                                      verb;    ///< The HTTP method, uppercase.
  std::string                                      target;  ///< The request target (path plus query).
  std::string                                      body;    ///< The request body, empty when absent.
  std::vector<std::pair<std::string, std::string>> headers; ///< Headers in arrival order, names lowercased.

  /// @brief Look up one header by (lowercase) name.
  /// @param name The lowercase field name.
  /// @return The value, or unset when the header was not sent.
  [[nodiscard]] auto header_value(std::string_view name) const -> std::optional<std::string> {
    for (auto const& [key, value] : headers) {
      if (key == name) {
        return value;
      }
    }
    return std::nullopt;
  }
};

/// @brief What the handler wants sent back.
struct canned_response {
  int         status = 200;                    ///< The HTTP status code.
  std::string body;                            ///< The response body.
  std::string content_type = "application/json"; ///< The `Content-Type` to send.
};

/// @brief An HTTP/1.1 server on an ephemeral loopback port, serving one
/// caller-supplied handler until destroyed.
///
/// Single-threaded on the serving side: one accept loop, one connection at a
/// time, `Connection: close` on every response. That is sufficient for every
/// test here (an adapter issues one request at a time) and it removes the
/// concurrency the Zig original's loop also does not have.
class server {
private:
  using handler_fn = std::function<canned_response(const captured_request&)>;

  handler_fn        _handler;
  int               _listen_fd = -1;
  std::uint16_t     _port      = 0;
  std::atomic<bool> _stop{false};
  std::atomic<std::size_t> _requests{0};
  std::thread              _thread;

  /// @brief Read from `fd` until `needle` appears or the peer closes.
  /// @param fd The connected socket.
  /// @param buffer The accumulating read buffer; appended to.
  /// @param needle The delimiter to wait for.
  /// @return The index of `needle`, or `std::string::npos` if the peer closed first.
  static auto read_until(int fd, std::string& buffer, std::string_view needle) -> std::size_t {
    while (true) {
      auto const at = buffer.find(needle);
      if (at != std::string::npos) {
        return at;
      }
      std::array<char, 4096> chunk{};
      auto const             got = ::read(fd, chunk.data(), chunk.size());
      if (got <= 0) {
        return std::string::npos;
      }
      buffer.append(chunk.data(), static_cast<std::size_t>(got));
    }
  }

  /// @brief Serve one accepted connection, then close it.
  /// @param fd The connected socket; closed before returning.
  auto serve_one(int fd) -> void {
    std::string raw;
    auto const  head_end = read_until(fd, raw, "\r\n\r\n");
    if (head_end == std::string::npos) {
      ::close(fd);
      return;
    }
    std::string_view const head{raw.data(), head_end};

    captured_request req;
    std::size_t      line_start = 0;
    bool             first      = true;
    std::size_t      content_length = 0;
    while (line_start < head.size()) {
      auto       line_end = head.find("\r\n", line_start);
      if (line_end == std::string_view::npos) {
        line_end = head.size();
      }
      std::string_view const line = head.substr(line_start, line_end - line_start);
      line_start                  = line_end + 2;
      if (first) {
        first             = false;
        auto const first_sp  = line.find(' ');
        auto const second_sp = line.find(' ', first_sp == std::string_view::npos ? 0 : first_sp + 1);
        if (first_sp != std::string_view::npos) {
          req.verb = std::string(line.substr(0, first_sp));
          req.target =
              std::string(line.substr(first_sp + 1, (second_sp == std::string_view::npos ? line.size() : second_sp) -
                                                        first_sp - 1));
        }
        continue;
      }
      auto const colon = line.find(':');
      if (colon == std::string_view::npos) {
        continue;
      }
      std::string name{line.substr(0, colon)};
      for (auto& c : name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      auto value = std::string(line.substr(colon + 1));
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.erase(value.begin());
      }
      if (name == "content-length") {
        content_length = static_cast<std::size_t>(std::strtoul(value.c_str(), nullptr, 10));
      }
      req.headers.emplace_back(std::move(name), std::move(value));
    }

    std::string body = raw.substr(head_end + 4);
    while (body.size() < content_length) {
      std::array<char, 4096> chunk{};
      auto const             got = ::read(fd, chunk.data(), chunk.size());
      if (got <= 0) {
        break;
      }
      body.append(chunk.data(), static_cast<std::size_t>(got));
    }
    req.body = std::move(body);

    _requests.fetch_add(1, std::memory_order_acq_rel);
    canned_response const reply = _handler(req);

    // `Connection: close` plus an explicit Content-Length on every response,
    // including the empty ones: libcurl otherwise has to wait for the peer to
    // close before it can know a bodyless 204 is complete, which turns every
    // such case into a timeout-length pause.
    auto const wire = std::format("HTTP/1.1 {} X\r\nContent-Type: {}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                                  reply.status, reply.content_type, reply.body.size(), reply.body);
    std::size_t sent = 0;
    while (sent < wire.size()) {
      auto const wrote = ::write(fd, wire.data() + sent, wire.size() - sent);
      if (wrote <= 0) {
        break;
      }
      sent += static_cast<std::size_t>(wrote);
    }
    ::close(fd);
  }

  /// @brief The accept loop. Polls with a short timeout rather than blocking
  /// in `accept`, so the destructor's stop flag is observed without needing
  /// to wake a blocked `accept` (closing the listening fd from another thread
  /// does not reliably do that on macOS).
  auto run() -> void {
    while (!_stop.load(std::memory_order_acquire)) {
      pollfd waiting{.fd = _listen_fd, .events = POLLIN, .revents = 0};
      auto const ready = ::poll(&waiting, 1, 25);
      if (ready <= 0) {
        continue;
      }
      int const fd = ::accept(_listen_fd, nullptr, nullptr);
      if (fd < 0) {
        continue;
      }
      serve_one(fd);
    }
  }

public:
  server(const server&)            = delete;
  server& operator=(const server&) = delete;
  server(server&&)                 = delete;
  server& operator=(server&&)      = delete;

  /// @brief Bind an ephemeral loopback port and start serving.
  /// @param handler Called once per request, on the server thread. It must be
  /// safe to call concurrently with the test thread's own work.
  explicit server(handler_fn handler) : _handler(std::move(handler)) {
    _listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (_listen_fd < 0) {
      throw std::runtime_error("fixture server: socket() failed");
    }
    int const on = 1;
    ::setsockopt(_listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    // Loopback only. See this header's "No test that uses this can reach the
    // network" section — this is the structural half of that guarantee.
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(0);
    if (::bind(_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      ::close(_listen_fd);
      throw std::runtime_error("fixture server: bind() failed");
    }
    if (::listen(_listen_fd, 8) != 0) {
      ::close(_listen_fd);
      throw std::runtime_error("fixture server: listen() failed");
    }
    sockaddr_in bound{};
    socklen_t   bound_len = sizeof(bound);
    if (::getsockname(_listen_fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) != 0) {
      ::close(_listen_fd);
      throw std::runtime_error("fixture server: getsockname() failed");
    }
    _port   = ntohs(bound.sin_port);
    _thread = std::thread([this] { run(); });
  }

  /// @brief Stop the accept loop, join the server thread and close the socket.
  ~server() {
    _stop.store(true, std::memory_order_release);
    if (_thread.joinable()) {
      _thread.join();
    }
    if (_listen_fd >= 0) {
      ::close(_listen_fd);
    }
  }

  /// @brief The kernel-assigned loopback port.
  /// @return The port.
  [[nodiscard]] auto port() const noexcept -> std::uint16_t { return _port; }

  /// @brief The origin to send to.
  /// @return `http://127.0.0.1:<port>`.
  [[nodiscard]] auto base_url() const -> std::string { return std::format("http://127.0.0.1:{}", _port); }

  /// @brief How many requests the server has served.
  /// @return The count.
  [[nodiscard]] auto request_count() const noexcept -> std::size_t { return _requests.load(std::memory_order_acquire); }
};

} // namespace planar::http::fixture
