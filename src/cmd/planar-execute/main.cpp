/// @file main.cpp
/// @brief The `planar-execute` binary's entry point (plan 996, task 6107).
///
/// Port target: `main` in zig/src/cmd/planar-execute/main.zig.
///
/// Thin, and thinner than the other three binaries' entry points because
/// there is genuinely less to snapshot: this binary has NO `context` type,
/// NO database path resolution and NO database. It reaches Planar state only
/// by shelling `planar`/`planar-agent`, so there is nothing here to thread
/// through. The ONE environment lookup is `$PLANAR_EXECUTE_ENGINE`, the
/// engine selector's second tier (plan 1033, task 6485; see
/// `planar.cmd.planar_execute.selector`).
///
/// The exit-code mapping is the one decision this file makes, and all three
/// codes are oracle-captured (see `planar.cmd.planar_execute.cli`'s header
/// for the full table): 0 for `--help`, 2 for every usage failure including
/// a bare invocation, 1 for a workflow file that cannot be read.
///
/// EVERY diagnostic — usage included, on the success path included — goes
/// to STDERR. stdout is the clean JSON result channel and stays empty
/// unless a workflow produced a payload. That is the opposite of the other
/// three binaries, where `--help` writes to stdout, and it was captured
/// rather than assumed.
// Not a module unit: `main` must have external linkage in the global module.
import std;
import planar.cmd.planar_execute.cli;
import planar.cmd.planar_execute.catalog;
import planar.cmd.planar_execute.engine;
import planar.cmd.planar_execute.selector;
import planar.cmd.planar_execute.profile;
import planar.cmd.planar_execute.host;
import planar.cmd.planar_execute.compat;
import planar.cmd.planar_execute.runflow;
import planar.engine_execute;

// After the imports: declares std:: types, includes no standard header.
#include "client_bridge.hpp"

namespace {

/// @brief Write the usage text to stderr, verbatim (it is a COMPLETE
/// payload — trailing newline included — so nothing is appended).
auto print_usage() -> void {
  std::cerr << planar::cmd::execute::usage_text();
}

/// @brief `$PLANAR_EXECUTE_ENGINE`, or unset.
/// @return The raw value.
auto engine_env() -> std::optional<std::string_view> {
  char const* raw = std::getenv("PLANAR_EXECUTE_ENGINE"); // NOLINT(concurrency-mt-unsafe) — single-threaded startup.
  if (raw == nullptr) {
    return std::nullopt;
  }
  return std::string_view{raw};
}

/// @brief Resolve the engine for this process, reading the config plane
/// through the sibling `planar` only when flag and env are both absent.
/// @param flag The `--engine` value, empty when not given.
/// @return The choice, or the one-line reason it could not be made.
auto resolve(std::string_view flag) -> std::expected<planar::cmd::execute::engine_choice, std::string> {
  namespace ex          = planar::cmd::execute;
  auto const flag_value = flag.empty() ? std::nullopt : std::optional<std::string_view>{flag};
  return ex::resolve_engine(flag_value, engine_env(), ex::sibling_config_reader(ex::executable_dir()));
}

/// @brief Environment lookup shared by the profile resolver.
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

/// @brief Run one `submit`: ensure the profile's daemon, check identity, start and follow.
/// @param asked The parsed arguments.
/// @return The process exit code.
auto submit_run(const planar::cmd::execute::submit_args& asked) -> int {
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

  auto const layout   = ex::layout_for(*resolved);
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

} // namespace

/// @brief Process entry point.
/// @param argc Argument count.
/// @param argv Argument vector.
/// @return The process exit code.
auto main(int argc, char** argv) -> int {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }

  auto const shape = planar::cmd::execute::classify(args);

  int code = 0;
  switch (shape) {
  case planar::cmd::execute::verb::none:
    print_usage();
    code = 2;
    break;

  case planar::cmd::execute::verb::help:
    print_usage();
    code = 0;
    break;

  case planar::cmd::execute::verb::unknown:
    // Message FIRST, then usage — the oracle's order.
    std::cerr << "planar-execute: unknown verb: " << args[1] << '\n';
    print_usage();
    code = 2;
    break;

  case planar::cmd::execute::verb::schema:
    // The one verb whose payload is stdout: the catalog is a machine
    // channel read by `cli_usage_lint`, exactly like the other binaries'
    // `schema`. Nothing goes to stderr.
    std::cout << planar::cmd::execute::catalog_json() << '\n';
    code = 0;
    break;

  case planar::cmd::execute::verb::profile: {
    // Like `schema`, a machine channel: the payload is stdout.
    namespace ex     = planar::cmd::execute;
    auto const asked = ex::parse_profile_args(std::span{args}.subspan(2));
    if (!asked.has_value()) {
      print_usage();
      code = 2;
      break;
    }
    // ONE read of the config plane serves both the engine and the profile.
    auto const bin_dir = ex::executable_dir();
    auto const entries = planar::engine::execute::read_planar_config_all(bin_dir);
    if (!entries.has_value()) {
      std::cerr << "planar-execute: cannot read the config plane: " << entries.error() << '\n';
      code = 1;
      break;
    }
    auto const choice = ex::resolve_engine(std::nullopt, engine_env(), ex::entries_config_reader(*entries));
    if (!choice.has_value()) {
      std::cerr << "planar-execute: " << choice.error() << '\n';
      code = 1;
      break;
    }
    auto const resolved = ex::resolve_profile(
        *entries, asked->name,
        [](std::string_view name) -> std::optional<std::string> {
          char const* raw = std::getenv(std::string{name}.c_str()); // NOLINT(concurrency-mt-unsafe) — single-threaded startup.
          return raw == nullptr || *raw == '\0' ? std::nullopt : std::optional<std::string>{raw};
        },
        [&bin_dir] {
          auto const path = planar::engine::execute::read_planar_config_path(bin_dir);
          return path.has_value() ? *path : std::string{"the planar config file"};
        });
    if (!resolved.has_value()) {
      std::cerr << "planar-execute: " << resolved.error() << '\n';
      code = 1;
      break;
    }
    std::cout << ex::render_profile(*choice, *resolved, asked->json);
    code = 0;
    break;
  }

  case planar::cmd::execute::verb::submit: {
    auto const asked = planar::cmd::execute::parse_submit_args(std::span{args}.subspan(2));
    if (!asked.has_value()) {
      print_usage();
      code = 2;
      break;
    }
    code = submit_run(*asked);
    break;
  }

  case planar::cmd::execute::verb::run: {
    auto const parsed = planar::cmd::execute::parse_run_args(std::span{args}.subspan(2));
    if (!parsed.has_value()) {
      print_usage();
      code = 2;
      break;
    }
    auto const choice = resolve(parsed->engine);
    if (!choice.has_value()) {
      std::cerr << "planar-execute: " << choice.error() << '\n';
      code = 1;
      break;
    }
    if (choice->engine == planar::cmd::execute::engine_kind::centurion) {
      // Parses, resolves, and is refused by NAME before anything runs
      // (tech-spec D5; test-spec "centurion engine refused at dispatch").
      // Retired by the end-to-end example scenario when the host lands in
      // plan 1033 M2, not carried as a permanent refusal.
      std::cerr << "planar-execute: engine 'centurion' is not available yet (" << choice->source
                << "); the Centurion host lands in plan 1033 M2\n";
      code = 1;
      break;
    }
    // Every engine failure is exit 1. The oracle maps every `EngineError`
    // except `BadUsage` to 1, and `BadUsage` cannot come out of this path —
    // it is produced by `parse_run_args` above, which has already succeeded.
    switch (planar::cmd::execute::run_workflow(*parsed, std::cout, std::cerr)) {
    case planar::cmd::execute::run_outcome::ok:
      code = 0;
      break;
    case planar::cmd::execute::run_outcome::load_failed:
    case planar::cmd::execute::run_outcome::engine_failed:
      code = 1;
      break;
    }
    break;
  }
  }

  std::cout.flush();
  std::cerr.flush();
  return code;
}
