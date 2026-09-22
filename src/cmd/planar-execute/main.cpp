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
    auto const json = planar::cmd::execute::parse_profile_args(std::span{args}.subspan(2));
    if (!json.has_value()) {
      print_usage();
      code = 2;
      break;
    }
    auto const choice = resolve({});
    if (!choice.has_value()) {
      std::cerr << "planar-execute: " << choice.error() << '\n';
      code = 1;
      break;
    }
    std::cout << planar::cmd::execute::render_profile(*choice, *json);
    code = 0;
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
