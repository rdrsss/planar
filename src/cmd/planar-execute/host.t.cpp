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
  std::chrono::steady_clock::time_point clock{};                         ///< Fake now, advanced by sleep.

  [[nodiscard]] auto hooks() -> host_hooks {
    return host_hooks{
        .probe_ = [this](const std::filesystem::path&) { return serving; },
        .alive_ = [this](std::int64_t) { return owner_alive; },
        .spawn_ = [this](const host_layout& layout, const std::filesystem::path&) -> std::expected<void, std::string> {
          ++spawns;
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
