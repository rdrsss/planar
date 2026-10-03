/// @file client.cpp
/// @brief Execute the Centurion client verbs for planar-execute.
module;
#include <cstdint>
#include <functional>
#include <string>

// The bridge declarations must live in the global module after their standard types.
#include "client_bridge.hpp"

module planar.cmd.planar_execute.handlers.shared.client;
import std;
import planar.cmd.planar_execute.cli;
import planar.cmd.planar_execute.engine;
import planar.cmd.planar_execute.selector;
import planar.cmd.planar_execute.profile;
import planar.cmd.planar_execute.host;
import planar.cmd.planar_execute.compat;
import planar.cmd.planar_execute.runflow;
import planar.engine_execute;

namespace planar::cmd::execute::handlers::client {
/// @brief Environment lookup shared by the profile resolver.
/// @param name Environment variable name.
/// @return Its nonempty value, when set.
auto env_value(std::string_view name) -> std::optional<std::string> {
  char const* raw = std::getenv(std::string{name}.c_str()); // NOLINT(concurrency-mt-unsafe) — single-threaded startup.
  return raw == nullptr || *raw == '\0' ? std::nullopt : std::optional<std::string>{raw};
}

/// @brief Resolve one profile from the config plane, for the verbs that need one.
/// @param name The profile name.
/// @return The resolved profile, or the one-line reason.
auto resolve_named_profile(std::string_view name) -> std::expected<planar::cmd::execute::profile, std::string> {
  namespace ex       = planar::cmd::execute;
  auto const bin_dir = ex::executable_dir();
  auto const entries = planar::engine::execute::read_planar_config_all(bin_dir);
  if (!entries.has_value()) {
    return std::unexpected(std::format("cannot read the config plane: {}", entries.error()));
  }
  return ex::resolve_profile(*entries, name, env_value, [&bin_dir] {
    auto const path = planar::engine::execute::read_planar_config_path(bin_dir);
    return path.has_value() ? *path : std::string{"the planar config file"};
  });
}

/// @brief Translate the bridge's classification into the flow's.
///
/// An explicit switch, NOT a cast between two enums that happen to be
/// declared in the same order: the two live in different translation units
/// (one a header, one a module, because gRPC headers cannot enter a module
/// purview) and a reordering of either would silently turn one outcome into
/// another — "refused" into "retryable" is a retry loop against a durable
/// refusal, and the reverse abandons work that was never attempted.
/// @param outcome Bridge call outcome.
/// @return The corresponding flow outcome.
auto translate(planar::cmd::execute::call_outcome outcome) -> planar::cmd::execute::daemon_outcome {
  namespace ex = planar::cmd::execute;
  switch (outcome) {
  case ex::call_outcome::ok:
    return ex::daemon_outcome::ok;
  case ex::call_outcome::retryable:
    return ex::daemon_outcome::retryable;
  case ex::call_outcome::refused:
    return ex::daemon_outcome::refused;
  case ex::call_outcome::uncertain:
    return ex::daemon_outcome::uncertain;
  }
  return ex::daemon_outcome::uncertain;
}

/// @brief Project a bridge answer onto the flow's shape.
/// @param answer Bridge call result.
/// @return The corresponding flow answer.
auto translate(const planar::cmd::execute::call_result& answer) -> planar::cmd::execute::daemon_answer {
  namespace ex = planar::cmd::execute;
  return ex::daemon_answer{.outcome_ = translate(answer.outcome_),
                           .run_     = ex::run_projection{.run_id_      = answer.run_.run_id_,
                                                          .status_      = answer.run_.status_,
                                                          .result_json_ = answer.run_.result_json_,
                                                          .error_json_  = answer.run_.error_json_,
                                                          .terminal_    = answer.run_.terminal_},
                           .message_ = answer.message_};
}

/// @brief Refuse an engine verb when this build cannot reach a Centurion engine.
///
/// Called FIRST by every verb in this file, before a profile is resolved or a
/// host marker written: a build without the engine must not report a daemon
/// that is merely not running (`status` would exit 0 saying "serving: no"),
/// nor leave a `drain` marker behind. The exit code is the one a durable
/// refusal maps to, because rebuilding the binary is the only thing that
/// changes the answer.
/// @return The exit code to stop with, or nullopt when the engine is linked.
auto refuse_without_engine() -> std::optional<int> {
  namespace ex          = planar::cmd::execute;
  const char* const why = ex::engine_unavailable_reason();
  if (why == nullptr) {
    return std::nullopt;
  }
  std::cerr << "planar-execute: " << why << '\n';
  return ex::exit_code_for(ex::flow_result::refused);
}

/// @brief Render one run as the operator or a script reads it.
/// @param run Run projection to render.
/// @param json Whether to use JSON output.
/// @return Rendered run text.
auto render_run(const planar::cmd::execute::run_view& run, bool json) -> std::string {
  if (json) {
    return std::format(R"({{"run_id":"{}","status":"{}","terminal":{},"sequence":{},"result":{},"error":{}}})"
                       "\n",
                       run.run_id_, run.status_, run.terminal_ ? "true" : "false", run.sequence_,
                       run.result_json_.empty() ? "null" : run.result_json_, run.error_json_.empty() ? "null" : run.error_json_);
  }
  std::string text = std::format("run:      {}\nstatus:   {}\nsequence: {}\n", run.run_id_, run.status_, run.sequence_);
  if (!run.result_json_.empty()) {
    text += std::format("result:   {}\n", run.result_json_);
  }
  if (!run.error_json_.empty()) {
    text += std::format("error:    {}\n", run.error_json_);
  }
  return text;
}

/// @brief Resolve a profile and reach its daemon WITHOUT starting one.
///
/// `status`, `cancel` and `host status` are inspection verbs: a daemon that is
/// not running is an answer, not a reason to start one. Only `submit` spawns.
struct reached_host {
  planar::cmd::execute::profile     profile_;   ///< Resolved profile.
  planar::cmd::execute::host_layout layout_;    ///< Host filesystem layout.
  bool                              serving_{}; ///< Whether the host is serving.
};

/// @brief Find the daemon serving a profile without starting it.
/// @param profile_name Profile to inspect.
/// @return Host state, or a diagnostic.
auto reach_host(std::string_view profile_name) -> std::expected<reached_host, std::string> {
  namespace ex        = planar::cmd::execute;
  auto const resolved = resolve_named_profile(profile_name);
  if (!resolved.has_value()) {
    return std::unexpected(resolved.error());
  }
  const auto layout = ex::layout_for(*resolved);
  return reached_host{.profile_ = *resolved, .layout_ = layout, .serving_ = ex::probe_socket(layout.socket_.c_str())};
}

auto status_run(const planar::cmd::execute::run_id_args& asked) -> int {
  if (const auto refused = refuse_without_engine(); refused.has_value()) {
    return *refused;
  }
  namespace ex = planar::cmd::execute;
  auto reached = reach_host(asked.profile);
  if (!reached.has_value()) {
    std::cerr << "planar-execute: " << reached.error() << '\n';
    return 1;
  }
  if (asked.run_id.empty()) {
    // No run named: report the profile itself. A down daemon is a legitimate
    // answer here, so this exits 0 either way.
    if (asked.json) {
      std::cout << std::format(R"({{"profile":"{}","state_dir":"{}","socket":"{}","serving":{}}})"
                               "\n",
                               reached->profile_.name, reached->profile_.state_dir, reached->layout_.socket_.string(),
                               reached->serving_ ? "true" : "false");
    } else {
      std::cout << std::format("profile:   {}\nstate_dir: {}\nsocket:    {}\nserving:   {}\n", reached->profile_.name,
                               reached->profile_.state_dir, reached->layout_.socket_.string(), reached->serving_ ? "yes" : "no");
    }
    return 0;
  }
  if (!reached->serving_) {
    std::cerr << std::format("planar-execute: no daemon is serving profile '{}'; nothing to read a run from\n", asked.profile);
    return 1;
  }
  auto answer = ex::fetch_run(reached->layout_.socket_.c_str(), asked.run_id.c_str());
  if (answer.outcome_ != ex::call_outcome::ok) {
    std::cerr << "planar-execute: " << answer.message_ << '\n';
    return 1;
  }
  std::cout << render_run(answer.run_, asked.json);
  return 0;
}

auto cancel_run_verb(const planar::cmd::execute::run_id_args& asked) -> int {
  if (const auto refused = refuse_without_engine(); refused.has_value()) {
    return *refused;
  }
  namespace ex = planar::cmd::execute;
  auto reached = reach_host(asked.profile);
  if (!reached.has_value()) {
    std::cerr << "planar-execute: " << reached.error() << '\n';
    return 1;
  }
  if (!reached->serving_) {
    // Nothing is running it, so there is nothing to stop. Said plainly rather
    // than as a transport error.
    std::cerr << std::format("planar-execute: no daemon is serving profile '{}'; the run is not executing\n", asked.profile);
    return 1;
  }
  // Read first: the control carries the caller's optimistic cursor over the
  // run's history, and quoting a stale one would refuse a cancel for a reason
  // that has nothing to do with the operator's intent.
  auto current = ex::fetch_run(reached->layout_.socket_.c_str(), asked.run_id.c_str());
  if (current.outcome_ != ex::call_outcome::ok) {
    std::cerr << "planar-execute: " << current.message_ << '\n';
    return 1;
  }
  auto cancelled = ex::cancel_run(reached->layout_.socket_.c_str(), asked.run_id.c_str(), current.run_.sequence_);
  if (cancelled.outcome_ != ex::call_outcome::ok) {
    std::cerr << "planar-execute: " << cancelled.message_ << '\n';
    return 1;
  }
  std::cout << render_run(cancelled.run_, asked.json);
  return 0;
}

auto host_status(const planar::cmd::execute::run_id_args& asked) -> int {
  if (const auto refused = refuse_without_engine(); refused.has_value()) {
    return *refused;
  }
  namespace ex = planar::cmd::execute;
  auto reached = reach_host(asked.profile);
  if (!reached.has_value()) {
    std::cerr << "planar-execute: " << reached.error() << '\n';
    return 1;
  }
  const auto record   = ex::read_endpoint_record(reached->layout_);
  const auto recorded = ex::recorded_tuple(reached->layout_);
  if (asked.json) {
    std::cout << std::format(
        R"({{"profile":"{}","serving":{},"pid":{},"socket_target":"{}","protocol_version":"{}","daemon_build":"{}"}})"
        "\n",
        reached->profile_.name, reached->serving_ ? "true" : "false", record ? record->pid_ : 0,
        record ? record->socket_target_ : std::string{}, record ? record->protocol_version_ : std::string{},
        recorded ? recorded->daemon_build_ : std::string{});
    return 0;
  }
  std::cout << std::format("profile:  {}\nserving:  {}\n", reached->profile_.name, reached->serving_ ? "yes" : "no");
  if (record) {
    std::cout << std::format("pid:      {}\nendpoint: {}\nprotocol: {}\n", record->pid_, record->socket_target_,
                             record->protocol_version_);
  }
  if (recorded) {
    std::cout << std::format("daemon:   {}\nbundle:   {}\n", recorded->daemon_build_,
                             recorded->bundle_digest_.empty() ? std::string{"<none>"} : recorded->bundle_digest_);
  }
  return 0;
}

auto follow_run_verb(const planar::cmd::execute::run_id_args& asked) -> int {
  if (const auto refused = refuse_without_engine(); refused.has_value()) {
    return *refused;
  }
  namespace ex = planar::cmd::execute;
  auto reached = reach_host(asked.profile);
  if (!reached.has_value()) {
    std::cerr << "planar-execute: " << reached.error() << '\n';
    return 1;
  }
  if (!reached->serving_) {
    std::cerr << std::format("planar-execute: no daemon is serving profile '{}'; nothing to follow\n", asked.profile);
    return 1;
  }

  const auto cursor_file = ex::cursor_path(reached->layout_.home_, asked.run_id);
  // An explicit --from wins: it is the operator saying "ignore what I saw
  // before". Otherwise resume after the last event this client ACCEPTED.
  const auto start = asked.from.value_or(ex::read_cursor(cursor_file));

  std::string cursor_error;
  auto        followed =
      ex::follow_run(reached->layout_.socket_.c_str(), asked.run_id.c_str(), start, [&](const ex::follow_event& event) {
        if (auto accepted = ex::accept_follow_event(std::cout, cursor_file, event.sequence_, event.event_type_,
                                                    event.current_status_, event.payload_json_);
            !accepted.has_value()) {
          cursor_error = accepted.error();
          return false;
        }
        return true;
      });
  if (!cursor_error.empty()) {
    std::cerr << "planar-execute: " << cursor_error << '\n';
    return 1;
  }
  if (followed.outcome_ != ex::call_outcome::ok) {
    std::cerr << "planar-execute: " << followed.message_ << '\n';
    return 1;
  }
  return 0;
}

auto host_lifecycle(std::string_view action, const planar::cmd::execute::run_id_args& asked) -> int {
  if (const auto refused = refuse_without_engine(); refused.has_value()) {
    return *refused;
  }
  namespace ex = planar::cmd::execute;
  auto reached = reach_host(asked.profile);
  if (!reached.has_value()) {
    std::cerr << "planar-execute: " << reached.error() << '\n';
    return 1;
  }

  if (action == "drain") {
    // Planar cannot tell Centurion to stop admitting — there is no such
    // operation — so what it stops is its own submitting. Work already
    // running is untouched, which is the point.
    if (auto marked = ex::set_draining(reached->layout_, true); !marked.has_value()) {
      std::cerr << "planar-execute: " << marked.error() << '\n';
      return 1;
    }
    std::cout << std::format("profile '{}' is draining: new submissions are refused, running work is untouched\n", asked.profile);
    return 0;
  }

  auto stopped = ex::stop_host(
      reached->layout_, ex::real_hooks([](const std::filesystem::path& socket) { return ex::probe_socket(socket.c_str()); }));
  if (!stopped.has_value()) {
    std::cerr << "planar-execute: " << stopped.error().message_ << '\n';
    return 1;
  }
  // A stopped host leaves no reason to keep refusing submissions: the next one
  // starts a fresh daemon.
  if (auto cleared = ex::set_draining(reached->layout_, false); !cleared.has_value()) {
    std::cerr << "planar-execute: " << cleared.error() << '\n';
    return 1;
  }
  switch (*stopped) {
  case ex::stop_result::not_running:
    std::cout << std::format("no daemon was serving profile '{}'\n", asked.profile);
    return 0;
  case ex::stop_result::stopped:
    std::cout << std::format("daemon for profile '{}' stopped and drained\n", asked.profile);
    return 0;
  case ex::stop_result::timed_out:
    std::cerr << std::format("planar-execute: daemon for profile '{}' was asked to stop and is still draining\n", asked.profile);
    return 1;
  }
  return 1;
}

auto submit_run(const planar::cmd::execute::submit_args& asked) -> int {
  if (const auto refused = refuse_without_engine(); refused.has_value()) {
    return *refused;
  }
  namespace ex = planar::cmd::execute;

  auto const resolved = resolve_named_profile(asked.profile);
  if (!resolved.has_value()) {
    std::cerr << "planar-execute: " << resolved.error() << '\n';
    return 1;
  }

  // The daemon ships BESIDE this binary (task 6709 installs both under the
  // same prefix), for the same reason `cli.planar(...)` resolves its sibling:
  // what runs is what was installed with this client, not whatever a PATH
  // happens to name first.
  auto const bin_dir = ex::executable_dir();
  auto const daemon  = std::filesystem::path{bin_dir} / "centuriond";

  auto const layout = ex::layout_for(*resolved);
  if (ex::is_draining(layout)) {
    // Retryable, not a failure of the work: the operator asked this profile to
    // stop taking new runs, and that is a state it will leave.
    std::cerr << std::format("planar-execute: profile '{}' is draining and is not accepting new runs\n", resolved->name);
    return ex::exit_code_for(ex::flow_result::retry_later);
  }
  auto const recorded = ex::recorded_tuple(layout);

  auto ensured = ex::ensure_host(
      *resolved, daemon, ex::real_hooks([](const std::filesystem::path& socket) { return ex::probe_socket(socket.c_str()); }));
  if (!ensured.has_value()) {
    std::cerr << "planar-execute: " << ensured.error().message_ << '\n';
    return 1;
  }

  const auto current = ex::compute_tuple(
      *resolved, ex::compatibility_inputs{.daemon_          = daemon,
                                          .sibling_bin_dir_ = bin_dir,
                                          .workbench_root_  = env_value("PLANAR_WORKBENCH_ROOT").value_or(std::string{})});
  if (ensured->origin_ == ex::host_origin::joined) {
    // Only a JOINED daemon can disagree with this client; one this process
    // just started was configured by it.
    if (const auto mismatches = ex::compare_tuples(recorded, current); !mismatches.empty()) {
      std::cerr << "planar-execute: " << ex::mismatch_text(mismatches, resolved->name) << '\n';
      return 1;
    }
  } else if (const auto written = ex::record_tuple(layout, current); !written.has_value()) {
    std::cerr << "planar-execute: " << written.error() << '\n';
    return 1;
  }

  const auto socket = ensured->socket_.string();
  const auto client = ex::run_client{
      .submit_ =
          [&socket](std::string_view bundle, std::string_view input, std::string_view request_id) {
            return translate(ex::submit_bundle_run(socket.c_str(), std::string{bundle}.c_str(), std::string{input}.c_str(),
                                                   std::string{request_id}.c_str()));
          },
      .fetch_ =
          [&socket](std::string_view run_id) { return translate(ex::fetch_run(socket.c_str(), std::string{run_id}.c_str())); },
  };

  const auto request_id = ex::make_request_id(std::chrono::system_clock::now(), std::random_device{}());
  const auto outcome    = ex::submit_and_follow(client, asked.bundle, asked.input, request_id);
  if (outcome.result_ == ex::flow_result::completed) {
    // stdout stays the clean payload channel, as it is for `run`.
    std::cout << ex::result_payload(outcome.run_) << '\n';
  } else {
    std::cerr << "planar-execute: " << outcome.message_ << '\n';
  }
  return ex::exit_code_for(outcome.result_);
}

} // namespace planar::cmd::execute::handlers::client
