/// @file host.t.cpp
/// @brief The profile's daemon lifecycle: what is written, when a daemon is
///        started, and when one is not (plan 1033 M2, tasks 6502/6710).
///
/// `ensure_host` takes its probe, spawn, clock and sleep as hooks, so every
/// case here drives the decision sequence with no daemon, no gRPC and no real
/// waiting. What each case asserts is the DECISION — joined versus spawned
/// versus refused — plus the one side effect that decision is allowed to have.

import std;
import planar.cmd.planar_execute.host;
import planar.cmd.planar_execute.profile;

#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace {

using planar::cmd::execute::daemon_config_json;
using planar::cmd::execute::ensure_host;
using planar::cmd::execute::host_endpoint;
using planar::cmd::execute::host_failure;
using planar::cmd::execute::host_hooks;
using planar::cmd::execute::host_layout;
using planar::cmd::execute::host_origin;
using planar::cmd::execute::layout_for;
using planar::cmd::execute::loopback_port;
using planar::cmd::execute::profile;
using planar::cmd::execute::read_endpoint_record;
using planar::cmd::execute::write_daemon_config;

/// @brief A temp state directory, removed with the case.
class scratch_state {
  std::filesystem::path _root;

public:
  explicit scratch_state(std::string_view tag)
      : _root(std::filesystem::temp_directory_path() /
              std::format("planar-host-{}-{}", tag, std::chrono::steady_clock::now().time_since_epoch().count())) {
    std::filesystem::create_directories(_root);
  }
  ~scratch_state() {
    std::error_code ignored;
    std::filesystem::remove_all(_root, ignored);
  }
  scratch_state(const scratch_state&)                    = delete;
  auto operator=(const scratch_state&) -> scratch_state& = delete;

  [[nodiscard]] auto path() const -> std::filesystem::path {
    return _root;
  }

  /// @brief A profile rooted here, with a bundle directory unless `with_bundle` is false.
  [[nodiscard]] auto make_profile(bool with_bundle = true) const -> profile {
    profile value;
    value.name               = "default";
    value.configured         = true;
    value.state_dir          = _root.string();
    value.planar_db          = (_root / "planar.db").string();
    value.idle_grace_seconds = 300;
    if (with_bundle) {
      value.bundle = (_root / "bundles").string();
    }
    return value;
  }
};

/// @brief Scripted hooks: a probe answer that can change, a spawn that records.
struct scripted_hooks {
  bool                                  serving                 = false; ///< What the probe answers.
  bool                                  serve_on_spawn          = false; ///< Whether a spawn makes the probe answer yes.
  int                                   spawns                  = 0;     ///< How many times spawn was called.
  bool                                  socket_present_at_spawn = false; ///< Whether a socket file existed when spawn ran.
  std::string                           spawn_error;                     ///< Non-empty makes spawn fail.
  bool                                  owner_alive = false;             ///< What the pid-liveness evidence answers.
  std::vector<std::string>              spawn_arguments;                 ///< Extra argv the spawn was asked for.
  std::vector<std::string>              spawn_environment;               ///< Extra environment the spawn was asked for.
  std::chrono::steady_clock::time_point clock{};                         ///< Fake now, advanced by sleep.

  [[nodiscard]] auto hooks() -> host_hooks {
    return host_hooks{
        .probe_ = [this](const std::filesystem::path&) { return serving; },
        .alive_ = [this](std::int64_t) { return owner_alive; },
        .spawn_ = [this](const host_layout&                      layout, const std::filesystem::path&,
                         const planar::cmd::execute::spawn_plan& plan) -> std::expected<void, std::string> {
          ++spawns;
          spawn_arguments         = plan.arguments_;
          spawn_environment       = plan.environment_;
          socket_present_at_spawn = std::filesystem::exists(layout.socket_);
          if (!spawn_error.empty()) {
            return std::unexpected(spawn_error);
          }
          if (serve_on_spawn) {
            serving = true;
          }
          return {};
        },
        .sleep_ = [this](std::chrono::milliseconds span) { clock += span; },
        .now_   = [this] { return clock; },
    };
  }
};

} // namespace

TEST_CASE("the daemon configuration names the profile's own state, bundle and grace", "[execute][host]") {
  const scratch_state scratch("config");
  const auto          resolved = scratch.make_profile();
  const auto          json     = daemon_config_json(resolved);

  INFO(json);
  CHECK(json.contains(std::format("\"database\": \"{}\"", (scratch.path() / "centurion.db").string())));
  CHECK(json.contains(std::format("\"bundles_dir\": \"{}\"", (scratch.path() / "bundles").string())));
  CHECK(json.contains("\"idle_grace\": \"300s\""));

  SECTION("a profile with no bundle writes no bundles_dir key at all") {
    // Centurion's configuration is closed; an empty value would be a different
    // claim from an absent key, and only the absent one means "install none".
    const auto bare = daemon_config_json(scratch.make_profile(false));
    CHECK_FALSE(bare.contains("bundles_dir"));
  }
}

TEST_CASE("each profile gets its own loopback port", "[execute][host]") {
  // Centurion cannot start the Unix listener without the TCP one, so two
  // profiles on the default port would collide and the second daemon would
  // fail for a reason unrelated to the work.
  const scratch_state first("port-a");
  const scratch_state second("port-b");
  const auto          left  = loopback_port(first.make_profile());
  const auto          right = loopback_port(second.make_profile());

  CHECK(left >= 41000);
  CHECK(left <= 41999);
  CHECK(left != right);
  CHECK(left == loopback_port(first.make_profile())); // stable across calls
}

TEST_CASE("writing the configuration creates the profile's state tree", "[execute][host]") {
  const scratch_state scratch("write");
  const auto          resolved = scratch.make_profile();

  auto written = write_daemon_config(resolved);
  REQUIRE(written.has_value());
  CHECK(std::filesystem::is_directory(written->runtime_));
  REQUIRE(std::filesystem::is_regular_file(written->config_));

  std::ifstream     in(written->config_, std::ios::binary);
  const std::string on_disk{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  CHECK(on_disk == daemon_config_json(resolved));

  // The configuration sits beside the daemon's credential tree.
  const auto mode = std::filesystem::status(written->centurion_).permissions();
  CHECK((mode & std::filesystem::perms::group_all) == std::filesystem::perms::none);
  CHECK((mode & std::filesystem::perms::others_all) == std::filesystem::perms::none);
}

TEST_CASE("a daemon already serving the profile is joined, not restarted", "[execute][host]") {
  const scratch_state scratch("join");
  scripted_hooks      script;
  script.serving = true;

  auto ensured = ensure_host(scratch.make_profile(), "/nonexistent/centuriond", script.hooks());
  REQUIRE(ensured.has_value());
  CHECK(ensured->origin_ == host_origin::joined);
  CHECK(ensured->target_.starts_with("unix:"));
  // The whole point: joining starts nothing.
  CHECK(script.spawns == 0);
}

TEST_CASE("a foreign startup lock never stops this process joining a serving daemon", "[execute][host]") {
  // Another process holding the lock means it is STARTING one; it must not
  // stop a caller from using the daemon that is already up.
  //
  // This case deliberately does NOT claim to test the pre-lock fast path.
  // Removing that probe leaves this passing, because the loser's wait loop
  // probes before it sleeps and joins on the same tick — the two differ in
  // syscalls, not in observable behaviour, so asserting otherwise would be a
  // test that cannot fail for the reason its name gives.
  const scratch_state scratch("join-unlocked");
  const auto          resolved = scratch.make_profile();
  const auto          layout   = layout_for(resolved);
  std::filesystem::create_directories(layout.runtime_);

  const int holder = ::open(layout.lock_.c_str(), O_RDWR | O_CREAT, 0600);
  REQUIRE(holder >= 0);
  REQUIRE(::flock(holder, LOCK_EX | LOCK_NB) == 0);

  scripted_hooks script;
  script.serving     = true;
  const auto started = script.clock;
  auto       ensured = ensure_host(resolved, "/nonexistent/centuriond", script.hooks());

  REQUIRE(ensured.has_value());
  CHECK(ensured->origin_ == host_origin::joined);
  CHECK(script.spawns == 0);
  CHECK(script.clock == started); // joined on the first probe, with no wait

  ::flock(holder, LOCK_UN);
  ::close(holder);
}

TEST_CASE("no daemon serving means exactly one is started and waited for", "[execute][host]") {
  const scratch_state scratch("spawn");
  scripted_hooks      script;
  script.serve_on_spawn = true;

  auto ensured = ensure_host(scratch.make_profile(), "/nonexistent/centuriond", script.hooks());
  REQUIRE(ensured.has_value());
  CHECK(ensured->origin_ == host_origin::spawned);
  CHECK(script.spawns == 1);
}

TEST_CASE("a socket nothing accepts on is removed before a daemon is started", "[execute][host]") {
  const scratch_state scratch("stale");
  const auto          resolved = scratch.make_profile();
  const auto          layout   = layout_for(resolved);
  std::filesystem::create_directories(layout.runtime_);
  std::ofstream(layout.socket_) << "crashed owner's leftover";
  REQUIRE(std::filesystem::exists(layout.socket_));

  scripted_hooks script;
  script.serve_on_spawn = true;
  auto ensured          = ensure_host(resolved, "/nonexistent/centuriond", script.hooks());

  REQUIRE(ensured.has_value());
  CHECK(script.spawns == 1);
  // The evidence: by the time the daemon was started, the stale file was gone.
  CHECK_FALSE(script.socket_present_at_spawn);
}

namespace {

/// @brief Write an endpoint record of the shape Centurion publishes.
auto publish_record(const host_layout& layout, std::int64_t pid) -> void {
  std::filesystem::create_directories(layout.runtime_);
  std::ofstream(layout.runtime_ / "host-deadbeef.json")
      << std::format(R"({{"schema":1,"instance_id":"abc","protocol_version":"centurion.v1",)"
                     R"("socket_target":"unix:{}","pid":{},"attested":true,)"
                     R"("process":{{"pid":{},"start_token":"darwin.tbsd:1.2"}},"ready_at_ms":1}})",
                     layout.socket_.string(), pid, pid);
}

} // namespace

TEST_CASE("the owner's published record is read back", "[execute][host]") {
  const scratch_state scratch("record");
  const auto          layout = layout_for(scratch.make_profile());
  publish_record(layout, 4242);

  const auto record = read_endpoint_record(layout);
  REQUIRE(record.has_value());
  CHECK(record->pid_ == 4242);
  CHECK(record->protocol_version_ == "centurion.v1");
  CHECK(record->start_token_ == "darwin.tbsd:1.2");

  SECTION("no record at all reads as absent rather than as a failure") {
    const scratch_state bare("record-absent");
    CHECK_FALSE(read_endpoint_record(layout_for(bare.make_profile())).has_value());
  }
}

TEST_CASE("a live owner that is not accepting is refused, not evicted", "[execute][host]") {
  // Deleting the socket of a daemon that is merely slow to answer would strand
  // a running process. The published record's pid is the only evidence there
  // is, so a live one means refuse.
  const scratch_state scratch("occupied");
  const auto          resolved = scratch.make_profile();
  const auto          layout   = layout_for(resolved);
  std::filesystem::create_directories(layout.runtime_);
  std::ofstream(layout.socket_) << "";
  publish_record(layout, 4242);

  scripted_hooks script;
  script.owner_alive = true;

  auto ensured = ensure_host(resolved, "/nonexistent/centuriond", script.hooks(), std::chrono::milliseconds{200});
  REQUIRE_FALSE(ensured.has_value());
  CHECK(ensured.error().kind_ == host_failure::occupied);
  CHECK(ensured.error().message_.contains("4242"));
  CHECK(script.spawns == 0);
  // The live owner's socket is still there.
  CHECK(std::filesystem::exists(layout.socket_));
}

TEST_CASE("a crashed owner's socket is removed and replaced", "[execute][host]") {
  const scratch_state scratch("crashed");
  const auto          resolved = scratch.make_profile();
  const auto          layout   = layout_for(resolved);
  std::filesystem::create_directories(layout.runtime_);
  std::ofstream(layout.socket_) << "";
  publish_record(layout, 4242);

  scripted_hooks script;
  script.owner_alive    = false; // the pid is gone
  script.serve_on_spawn = true;

  auto ensured = ensure_host(resolved, "/nonexistent/centuriond", script.hooks());
  REQUIRE(ensured.has_value());
  CHECK(ensured->origin_ == host_origin::spawned);
  CHECK(script.spawns == 1);
  CHECK_FALSE(script.socket_present_at_spawn);
}

TEST_CASE("provider configuration reaches the daemon, and a credential never reaches argv", "[execute][host][providers]") {
  using planar::cmd::execute::provider_spawn_plan;

  const scratch_state scratch("providers");

  SECTION("the declared vendor's keys become daemon arguments and environment") {
    auto resolved                                          = scratch.make_profile();
    resolved.providers["cliproxyapi"]["base_url"]          = "http://127.0.0.1:8317";
    resolved.providers["cliproxyapi"]["api_key"]           = "secret-token";
    resolved.providers["cliproxyapi"]["trusted_hostnames"] = "api.example.com";

    auto plan = provider_spawn_plan(resolved);
    REQUIRE(plan.has_value());

    std::string argv;
    for (const auto& item : plan->arguments_) {
      argv += " " + item;
    }
    CHECK(argv.contains("--model-base-url http://127.0.0.1:8317"));
    CHECK(argv.contains("--trusted-http-hostname api.example.com"));
    // The whole point: the credential is NOT an argument. Arguments are
    // visible in `ps` to every user on the machine.
    CHECK_FALSE(argv.contains("secret-token"));
    REQUIRE(plan->environment_.size() == 1);
    CHECK(plan->environment_.front() == "CENTURION_CLIPROXYAPI_API_KEY=secret-token");
  }

  SECTION("a vendor this daemon does not declare is refused by name") {
    auto resolved                              = scratch.make_profile();
    resolved.providers["anthropic"]["api_key"] = "x";
    auto plan                                  = provider_spawn_plan(resolved);
    REQUIRE_FALSE(plan.has_value());
    CHECK(plan.error().contains("anthropic"));
    CHECK(plan.error().contains("cliproxyapi"));
  }

  SECTION("an unknown key is refused rather than silently dropped") {
    // Passing it through would leave the operator believing a setting took
    // effect that the daemon never saw.
    auto resolved                                  = scratch.make_profile();
    resolved.providers["cliproxyapi"]["timeout_s"] = "30";
    auto plan                                      = provider_spawn_plan(resolved);
    REQUIRE_FALSE(plan.has_value());
    CHECK(plan.error().contains("timeout_s"));
  }

  SECTION("no provider configuration is no arguments") {
    auto plan = provider_spawn_plan(scratch.make_profile());
    REQUIRE(plan.has_value());
    CHECK(plan->arguments_.empty());
    CHECK(plan->environment_.empty());
  }
}

TEST_CASE("a spawned daemon is given the profile's provider settings", "[execute][host][providers]") {
  // The wiring, not just the translation: ensure_host must hand the plan to
  // the spawn, or the settings are computed and dropped.
  const scratch_state scratch("providers-spawn");
  auto                resolved                  = scratch.make_profile();
  resolved.providers["cliproxyapi"]["base_url"] = "http://127.0.0.1:8317";
  resolved.providers["cliproxyapi"]["api_key"]  = "secret-token";

  scripted_hooks script;
  script.serve_on_spawn = true;
  auto ensured          = ensure_host(resolved, "/nonexistent/centuriond", script.hooks());
  REQUIRE(ensured.has_value());
  REQUIRE(script.spawns == 1);

  std::string argv;
  for (const auto& item : script.spawn_arguments) {
    argv += " " + item;
  }
  CHECK(argv.contains("--model-base-url"));
  CHECK_FALSE(argv.contains("secret-token"));
  REQUIRE(script.spawn_environment.size() == 1);
  CHECK(script.spawn_environment.front().starts_with("CENTURION_CLIPROXYAPI_API_KEY="));
}

TEST_CASE("a profile whose provider configuration cannot be honoured never starts a daemon", "[execute][host][providers]") {
  const scratch_state scratch("providers-refused");
  auto                resolved               = scratch.make_profile();
  resolved.providers["anthropic"]["api_key"] = "x";

  scripted_hooks script;
  script.serve_on_spawn = true;
  auto ensured          = ensure_host(resolved, "/nonexistent/centuriond", script.hooks());
  REQUIRE_FALSE(ensured.has_value());
  CHECK(ensured.error().kind_ == planar::cmd::execute::host_failure::configuration);
  // Refused BEFORE anything was started: a daemon running with settings the
  // operator did not get is worse than no daemon.
  CHECK(script.spawns == 0);
}

TEST_CASE("draining is a Planar-side gate, not a daemon state", "[execute][host][drain]") {
  using planar::cmd::execute::draining_marker;
  using planar::cmd::execute::is_draining;
  using planar::cmd::execute::set_draining;

  const scratch_state scratch("drain");
  const auto          layout = layout_for(scratch.make_profile());

  CHECK_FALSE(is_draining(layout));
  REQUIRE(set_draining(layout, true).has_value());
  CHECK(is_draining(layout));
  CHECK(std::filesystem::exists(draining_marker(layout)));

  SECTION("ending a drain is idempotent") {
    REQUIRE(set_draining(layout, false).has_value());
    CHECK_FALSE(is_draining(layout));
    // Clearing an already-cleared drain is a request that has been honoured.
    REQUIRE(set_draining(layout, false).has_value());
    CHECK_FALSE(is_draining(layout));
  }

  SECTION("starting a drain twice is idempotent") {
    REQUIRE(set_draining(layout, true).has_value());
    CHECK(is_draining(layout));
  }
}

TEST_CASE("stopping a host signals it and waits for it to go", "[execute][host][stop]") {
  using planar::cmd::execute::stop_host;
  using planar::cmd::execute::stop_result;

  const scratch_state scratch("stop");
  const auto          resolved = scratch.make_profile();
  const auto          layout   = layout_for(resolved);

  SECTION("nothing serving is an outcome, not an error") {
    // "Stop what is already stopped" is a request that has been honoured.
    scripted_hooks script;
    auto           stopped = stop_host(layout, script.hooks());
    REQUIRE(stopped.has_value());
    CHECK(*stopped == stop_result::not_running);
  }

  publish_record(layout, 4242);

  SECTION("a crashed owner's leftover record is not something to signal") {
    // The realistic stale case: the daemon died and its endpoint record
    // outlived it. Signalling that pid would be signalling whatever now holds
    // the number, so the record alone is not evidence of a running daemon.
    scripted_hooks script;
    script.owner_alive = false;
    auto hooks         = script.hooks();
    int  signals       = 0;
    hooks.terminate_   = [&](std::int64_t) {
      ++signals;
      return true;
    };
    auto stopped = stop_host(layout, hooks);
    REQUIRE(stopped.has_value());
    CHECK(*stopped == stop_result::not_running);
    CHECK(signals == 0);
  }

  SECTION("a live owner is signalled and reported stopped once it goes") {
    scripted_hooks script;
    script.owner_alive = true;
    auto hooks         = script.hooks();
    int  signals       = 0;
    hooks.terminate_   = [&](std::int64_t pid) {
      ++signals;
      CHECK(pid == 4242);
      script.owner_alive = false; // it honoured the signal
      return true;
    };
    auto stopped = stop_host(layout, hooks);
    REQUIRE(stopped.has_value());
    CHECK(*stopped == stop_result::stopped);
    CHECK(signals == 1);
  }

  SECTION("a daemon still draining when the budget expires is reported, not killed") {
    // Centurion's shutdown is bounded by its own rules; a drain that is still
    // draining has not failed, and Planar never escalates to a forced kill --
    // that would abandon the work the drain exists to preserve.
    scripted_hooks script;
    script.owner_alive = true;
    auto hooks         = script.hooks();
    int  signals       = 0;
    hooks.terminate_   = [&](std::int64_t) {
      ++signals;
      return true; // signalled, but it keeps running
    };
    auto stopped = stop_host(layout, hooks, std::chrono::milliseconds{200});
    REQUIRE(stopped.has_value());
    CHECK(*stopped == stop_result::timed_out);
    CHECK(signals == 1); // signalled ONCE; no escalation
  }
}

TEST_CASE("a daemon that never accepts is a readiness failure naming its log", "[execute][host]") {
  const scratch_state scratch("readiness");
  scripted_hooks      script; // spawn succeeds, but the probe never turns true

  auto ensured = ensure_host(scratch.make_profile(), "/nonexistent/centuriond", script.hooks(), std::chrono::milliseconds{200});
  REQUIRE_FALSE(ensured.has_value());
  CHECK(ensured.error().kind_ == host_failure::readiness);
  CHECK(ensured.error().message_.contains("centuriond.log"));
  CHECK(script.spawns == 1);
}

TEST_CASE("a spawn that fails is reported as a spawn failure, not a timeout", "[execute][host]") {
  const scratch_state scratch("spawn-fail");
  scripted_hooks      script;
  script.spawn_error = "centuriond is not installed";

  auto ensured = ensure_host(scratch.make_profile(), "/nonexistent/centuriond", script.hooks());
  REQUIRE_FALSE(ensured.has_value());
  CHECK(ensured.error().kind_ == host_failure::spawn);
  CHECK(ensured.error().message_ == "centuriond is not installed");
}

TEST_CASE("losing the startup lock waits for the winner instead of starting a second daemon", "[execute][host]") {
  const scratch_state scratch("lock");
  const auto          resolved = scratch.make_profile();
  const auto          layout   = layout_for(resolved);
  std::filesystem::create_directories(layout.runtime_);

  // Hold the profile's lock the way another planar-execute would.
  const int holder = ::open(layout.lock_.c_str(), O_RDWR | O_CREAT, 0600);
  REQUIRE(holder >= 0);
  REQUIRE(::flock(holder, LOCK_EX | LOCK_NB) == 0);

  SECTION("the winner's daemon is joined once it accepts") {
    scripted_hooks script;
    // The probe answers no until the loser's first wait, then yes: the winner
    // came up while this process was waiting.
    int  polls   = 0;
    auto hooks   = script.hooks();
    hooks.probe_ = [&polls](const std::filesystem::path&) { return ++polls > 2; };

    auto ensured = ensure_host(resolved, "/nonexistent/centuriond", hooks, std::chrono::milliseconds{500});
    REQUIRE(ensured.has_value());
    CHECK(ensured->origin_ == host_origin::joined);
    CHECK(script.spawns == 0);
  }

  SECTION("a winner that never accepts is a lock failure, and still nothing is started") {
    scripted_hooks script;
    auto           ensured = ensure_host(resolved, "/nonexistent/centuriond", script.hooks(), std::chrono::milliseconds{200});
    REQUIRE_FALSE(ensured.has_value());
    CHECK(ensured.error().kind_ == host_failure::lock);
    CHECK(script.spawns == 0);
  }

  ::flock(holder, LOCK_UN);
  ::close(holder);
}
