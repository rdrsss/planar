/// @file host.cpp
/// @brief Implementation of the profile's daemon lifecycle (plan 1033 M2, tasks 6502/6710).
module;

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

module planar.cmd.planar_execute.host;

import std;
import planar.cmd.planar_execute.profile;

namespace planar::cmd::execute {
namespace {

/// @brief Build a classified failure.
auto fail(host_failure kind, std::string message) -> std::unexpected<host_error> {
  return std::unexpected(host_error{.kind_ = kind, .message_ = std::move(message)});
}

/// @brief Minimal JSON string escaping for the values Planar writes (paths).
auto quote(std::string_view text) -> std::string {
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('"');
  for (const char character : text) {
    switch (character) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      out.push_back(character);
      break;
    }
  }
  out.push_back('"');
  return out;
}

/// @brief An RAII `flock` on one file, released with the descriptor.
class startup_lock {
  int _descriptor = -1;

public:
  startup_lock()                                       = default;
  startup_lock(const startup_lock&)                    = delete;
  auto operator=(const startup_lock&) -> startup_lock& = delete;
  ~startup_lock() {
    if (_descriptor >= 0) {
      ::flock(_descriptor, LOCK_UN);
      ::close(_descriptor);
    }
  }

  /// @brief Try to take the lock without blocking. @param path Lock file. @return Whether it was taken.
  [[nodiscard]] auto acquire(const std::filesystem::path& path) -> bool {
    _descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (_descriptor < 0) {
      return false;
    }
    if (::flock(_descriptor, LOCK_EX | LOCK_NB) != 0) {
      ::close(_descriptor);
      _descriptor = -1;
      return false;
    }
    return true;
  }
};

} // namespace

auto layout_for(const profile& resolved) -> host_layout {
  const std::filesystem::path home{resolved.state_dir};
  const auto                  centurion = home / ".centurion";
  const auto                  runtime   = centurion / "runtime";
  return host_layout{.home_      = home,
                     .centurion_ = centurion,
                     .config_    = centurion / "config",
                     .runtime_   = runtime,
                     // Centurion's own fixed name; a client that guessed differently
                     // would look for a socket no daemon ever creates.
                     .socket_   = runtime / "centuriond-v1.sock",
                     .database_ = home / "centurion.db",
                     .lock_     = home / "host.lock",
                     .log_      = home / "centuriond.log"};
}

auto loopback_port(const profile& resolved) -> std::uint16_t {
  const auto digest = std::hash<std::string>{}(resolved.state_dir);
  return static_cast<std::uint16_t>(41000 + (digest % 1000));
}

auto daemon_config_json(const profile& resolved) -> std::string {
  const auto layout = layout_for(resolved);

  std::string json = "{\n";
  json += std::format("  \"database\": {},\n", quote(layout.database_.string()));
  if (resolved.bundle.has_value()) {
    // Centurion ADR-0055. Without this key a stock daemon installs no bundle,
    // and every start Planar makes would name one that is not there.
    json += std::format("  \"bundles_dir\": {},\n", quote(*resolved.bundle));
  }
  json += std::format("  \"idle_grace\": {},\n", quote(std::format("{}s", resolved.idle_grace_seconds)));
  json += std::format("  \"port\": {}\n", loopback_port(resolved));
  json += "}\n";
  return json;
}

auto write_daemon_config(const profile& resolved) -> std::expected<host_layout, host_error> {
  const auto      layout = layout_for(resolved);
  std::error_code failure;
  std::filesystem::create_directories(layout.runtime_, failure);
  if (failure) {
    return fail(host_failure::state_dir, std::format("cannot create {}: {}", layout.runtime_.string(), failure.message()));
  }
  // Owner-only: the configuration sits beside the daemon's credential tree.
  std::filesystem::permissions(layout.centurion_, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace,
                               failure);

  const auto    text = daemon_config_json(resolved);
  std::ofstream out(layout.config_, std::ios::binary | std::ios::trunc);
  if (!out) {
    return fail(host_failure::state_dir, std::format("cannot write {}", layout.config_.string()));
  }
  out << text;
  out.close();
  if (!out) {
    return fail(host_failure::state_dir, std::format("cannot write {}", layout.config_.string()));
  }
  return layout;
}

auto ensure_host(const profile& resolved, const std::filesystem::path& daemon, const host_hooks& hooks,
                 std::chrono::milliseconds budget) -> std::expected<host_endpoint, host_error> {
  auto written = write_daemon_config(resolved);
  if (!written) {
    return std::unexpected(written.error());
  }
  const auto layout = *written;
  const auto target = std::format("unix:{}", layout.socket_.string());
  const auto now    = hooks.now_ ? hooks.now_ : [] { return std::chrono::steady_clock::now(); };
  const auto rest   = hooks.sleep_ ? hooks.sleep_ : [](std::chrono::milliseconds span) { std::this_thread::sleep_for(span); };

  // 1. Fast path: someone is already serving, answered without touching the
  //    lock. This is an optimization, not a guarantee — the loser loop below
  //    reaches the same answer, one probe later.
  if (hooks.probe_(layout.socket_)) {
    return host_endpoint{.target_ = target, .socket_ = layout.socket_, .origin_ = host_origin::joined};
  }

  const auto   deadline = now() + budget;
  startup_lock lock;
  if (!lock.acquire(layout.lock_)) {
    // Another process is starting the same daemon. Wait for ITS host rather
    // than starting a second one: exclusivity is the point of the lock, and a
    // loser that spawned anyway would be the race the lock exists to prevent.
    while (now() < deadline) {
      if (hooks.probe_(layout.socket_)) {
        return host_endpoint{.target_ = target, .socket_ = layout.socket_, .origin_ = host_origin::joined};
      }
      rest(std::chrono::milliseconds{50});
    }
    return fail(host_failure::lock, std::format("another process holds {} and no daemon accepted on {} within {}ms",
                                                layout.lock_.string(), layout.socket_.string(), budget.count()));
  }

  // 3. Re-probe under the lock: the winner of a race with step 1 has started one.
  if (hooks.probe_(layout.socket_)) {
    return host_endpoint{.target_ = target, .socket_ = layout.socket_, .origin_ = host_origin::joined};
  }

  // 4. A socket nothing accepts on is a crashed owner's leftover. Removing it
  //    is safe ONLY under the lock, which is why it happens here and not in
  //    the probe above.
  std::error_code ignored;
  std::filesystem::remove(layout.socket_, ignored);

  if (auto started = hooks.spawn_(layout, daemon); !started) {
    return fail(host_failure::spawn, started.error());
  }

  while (now() < deadline) {
    if (hooks.probe_(layout.socket_)) {
      return host_endpoint{.target_ = target, .socket_ = layout.socket_, .origin_ = host_origin::spawned};
    }
    rest(std::chrono::milliseconds{50});
  }
  return fail(host_failure::readiness, std::format("started {} but it did not accept on {} within {}ms; see {}", daemon.string(),
                                                   layout.socket_.string(), budget.count(), layout.log_.string()));
}

auto real_hooks(std::function<bool(const std::filesystem::path&)> probe) -> host_hooks {
  return host_hooks{
      .probe_ = std::move(probe),
      .spawn_ = [](const host_layout& layout, const std::filesystem::path& daemon) -> std::expected<void, std::string> {
        if (::access(daemon.c_str(), X_OK) != 0) {
          return std::unexpected(std::format("{} is not an executable daemon", daemon.string()));
        }
        // HOME is the profile's state directory: Centurion resolves its whole
        // ~/.centurion layout from it, so this is what keeps two profiles'
        // daemons in separate trees. Every other variable is inherited.
        std::vector<std::string> env;
        for (char** entry = environ; *entry != nullptr; ++entry) {
          std::string_view text{*entry};
          if (!text.starts_with("HOME=")) {
            env.emplace_back(text);
          }
        }
        env.push_back(std::format("HOME={}", layout.home_.string()));

        std::vector<std::string> arguments{daemon.string()};
        std::vector<char*>       argv;
        argv.reserve(arguments.size() + 1);
        for (auto& argument : arguments) {
          argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        std::vector<char*> envp;
        envp.reserve(env.size() + 1);
        for (auto& entry : env) {
          envp.push_back(entry.data());
        }
        envp.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        if (::posix_spawn_file_actions_init(&actions) != 0) {
          return std::unexpected("cannot initialize spawn file actions");
        }
        const auto log = layout.log_.string();
        ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        ::posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);

        // Detached: its own session, so the daemon outlives this client and is
        // not signalled by the terminal that started it.
        posix_spawnattr_t attributes;
        ::posix_spawnattr_init(&attributes);
#ifdef POSIX_SPAWN_SETSID
        ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
#endif
        pid_t      child = -1;
        const auto code  = ::posix_spawn(&child, argv[0], &actions, &attributes, argv.data(), envp.data());
        ::posix_spawn_file_actions_destroy(&actions);
        ::posix_spawnattr_destroy(&attributes);
        if (code != 0) {
          return std::unexpected(std::format("cannot start {}: {}", daemon.string(), std::strerror(code)));
        }
        return {};
      },
      .sleep_ = [](std::chrono::milliseconds span) { std::this_thread::sleep_for(span); },
      .now_   = [] { return std::chrono::steady_clock::now(); },
  };
}

} // namespace planar::cmd::execute
