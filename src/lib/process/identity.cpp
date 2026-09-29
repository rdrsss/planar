/// @file identity.cpp
/// @brief Implementation of `planar.process.identity`. The platform tables
/// live on the interface declarations; this unit is where the C calls are
/// made and where every errno is turned into a value or an `error` before
/// it can escape.

module;

#include <cerrno>
#include <csignal>
#include <ctime>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#endif

module planar.process.identity;

import std;

namespace planar::process::identity {

namespace {

/// @brief Turn a `kill(..., 0)` return into a verdict, reading errno only
/// when the call failed.
auto exists_from_kill(int rc) -> std::expected<bool, error> {
  return exists_from_errno(rc == 0 ? 0 : errno);
}

#if defined(__linux__)

/// @brief Read a whole small file, or nothing when it cannot be opened.
auto read_small_file(const std::filesystem::path& path) -> std::optional<std::string> {
  std::ifstream in{path};
  if (!in) {
    return std::nullopt;
  }
  std::string body{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
  while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' ')) {
    body.pop_back();
  }
  return body;
}

/// @brief The target of a symbolic link, or nothing when it cannot be read.
auto read_link_target(const char* path) -> std::optional<std::string> {
  std::array<char, 256> buffer{};
  auto const            got = ::readlink(path, buffer.data(), buffer.size() - 1);
  if (got <= 0) {
    return std::nullopt;
  }
  return std::string{buffer.data(), static_cast<std::size_t>(got)};
}

/// @brief Field 22 (`starttime`) of a `/proc/<pid>/stat` line.
///
/// Field 2 is the command name in parentheses and may itself contain spaces
/// and parentheses, so the parse starts after the LAST `)`. The remaining
/// fields are space-separated from field 3 (`state`), so `starttime` is
/// the 20th token after it, counting from zero.
auto parse_starttime(std::string_view stat) -> std::optional<start_time> {
  auto const close = stat.rfind(')');
  if (close == std::string_view::npos) {
    return std::nullopt;
  }
  auto const rest = stat.substr(close + 1);

  constexpr std::size_t k_starttime_index = 22 - 3;
  std::size_t           index             = 0;
  for (auto const part : std::views::split(rest, ' ')) {
    std::string_view const token{part.begin(), part.end()};
    if (token.empty()) {
      continue;
    }
    if (index == k_starttime_index) {
      start_time value     = 0;
      auto const [end, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
      if (ec != std::errc{} || end != token.data() + token.size()) {
        return std::nullopt;
      }
      return value;
    }
    ++index;
  }
  return std::nullopt;
}

#endif

} // namespace

auto exists_from_errno(int err) -> std::expected<bool, error> {
  switch (err) {
  case 0:
  case EPERM:
    return true;
  case ESRCH:
    return false;
  default:
    return std::unexpected{error::query_failed};
  }
}

auto process_start_time(std::int64_t pid) -> std::expected<std::optional<start_time>, error> {
  if (pid <= 0) {
    return std::optional<start_time>{};
  }
#if defined(__APPLE__)
  proc_bsdinfo info{};
  auto const   got = ::proc_pidinfo(static_cast<int>(pid), PROC_PIDTBSDINFO, 0, &info, sizeof info);
  if (got == static_cast<int>(sizeof info)) {
    auto const seconds      = static_cast<start_time>(info.pbi_start_tvsec);
    auto const microseconds = static_cast<start_time>(info.pbi_start_tvusec);
    return std::optional<start_time>{seconds * 1'000'000 + microseconds};
  }
  if (errno == ESRCH) {
    return std::optional<start_time>{};
  }
  return std::unexpected{error::query_failed};
#elif defined(__linux__)
  auto const      path = std::filesystem::path{"/proc"} / std::to_string(pid) / "stat";
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    // A pid with no /proc entry is an absent process; a failed existence
    // check is a query failure.
    if (ec) {
      return std::unexpected{error::query_failed};
    }
    return std::optional<start_time>{};
  }
  auto const body = read_small_file(path);
  if (!body.has_value()) {
    // The entry vanished between the existence check and the read.
    return std::optional<start_time>{};
  }
  auto const parsed = parse_starttime(*body);
  if (!parsed.has_value()) {
    return std::unexpected{error::query_failed};
  }
  return std::optional<start_time>{*parsed};
#else
  return std::unexpected{error::query_failed};
#endif
}

auto process_exists(std::int64_t pid) -> std::expected<bool, error> {
  if (pid <= 0) {
    return false;
  }
  return exists_from_kill(::kill(static_cast<::pid_t>(pid), 0));
}

auto group_has_members(std::int64_t pgid) -> std::expected<bool, error> {
  if (pgid <= 1) {
    return false;
  }
  return exists_from_kill(::kill(static_cast<::pid_t>(-pgid), 0));
}

auto signal_group(std::int64_t pgid, int sig) -> std::expected<void, error> {
  if (pgid <= 1) {
    return std::unexpected{error::no_such_process};
  }
  if (::kill(static_cast<::pid_t>(-pgid), sig) == 0) {
    return {};
  }
  switch (errno) {
  case ESRCH:
    return std::unexpected{error::no_such_process};
  case EPERM:
    return std::unexpected{error::not_permitted};
  case EINVAL:
    return std::unexpected{error::invalid_signal};
  default:
    return std::unexpected{error::query_failed};
  }
}

auto native_identity_source() -> identity_source {
  return []() -> std::optional<std::string> {
#if defined(__APPLE__)
    std::array<char, 128> buffer{};
    std::size_t           size = buffer.size();
    if (::sysctlbyname("kern.bootsessionuuid", buffer.data(), &size, nullptr, 0) != 0 || size == 0) {
      return std::nullopt;
    }
    std::string value{buffer.data(), size};
    while (!value.empty() && (value.back() == '\0' || value.back() == '\n')) {
      value.pop_back();
    }
    if (value.empty()) {
      return std::nullopt;
    }
    return value;
#elif defined(__linux__)
    auto const boot_id = read_small_file("/proc/sys/kernel/random/boot_id");
    auto const pid_ns  = read_link_target("/proc/self/ns/pid");
    if (!boot_id.has_value() || boot_id->empty() || !pid_ns.has_value() || pid_ns->empty()) {
      return std::nullopt;
    }
    return std::format("{}:{}", *boot_id, *pid_ns);
#else
    return std::nullopt;
#endif
  };
}

auto host_identity(const identity_source& source) -> std::string {
  if (!source) {
    return std::string{k_unknown_host_identity};
  }
  auto const value = source();
  if (!value.has_value() || value->empty()) {
    return std::string{k_unknown_host_identity};
  }
  return *value;
}

auto system_clock::monotonic_ms() -> std::expected<std::int64_t, error> {
#if defined(__APPLE__)
  constexpr clockid_t k_clock = CLOCK_UPTIME_RAW;
#else
  constexpr clockid_t k_clock = CLOCK_MONOTONIC;
#endif
  ::timespec now{};
  if (::clock_gettime(k_clock, &now) != 0) {
    return std::unexpected{error::clock_failed};
  }
  return static_cast<std::int64_t>(now.tv_sec) * 1'000 + static_cast<std::int64_t>(now.tv_nsec) / 1'000'000;
}

auto system_clock::wall_ms() -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace planar::process::identity
