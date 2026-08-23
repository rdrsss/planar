/// @file completion.cpp
/// @brief Implementation of `planar.cmd.planar_watch.handlers.completion`.

module planar.cmd.planar_watch.handlers.completion;

import std;
import planar.cli;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.exit;
import planar.cmd.planar_watch.handler;

namespace planar::cmd::watch::handlers {

namespace {

/// @brief Read a string positional out of a parse result.
///
/// Inlined here rather than reached for from `src/cmd/planar/args.cppm`:
/// that module belongs to a different layer-3 target, and D18 forbids a
/// `cmd_* -> cmd_*` edge (cmake/architecture.cmake FATALs at configure
/// time). One accessor is not worth a shared layer-1 library that would
/// exist solely to launder that edge.
/// @param args The parsed result.
/// @param name The positional's declared name.
/// @return The value, or unset when absent or not a string.
auto positional_string(const cli::match_result& args, std::string_view name) -> std::optional<std::string> {
  auto const it = args.positionals.find(std::string{name});
  if (it == args.positionals.end()) {
    return std::nullopt;
  }
  if (auto const* value = std::get_if<std::string>(&it->second)) {
    return *value;
  }
  return std::nullopt;
}

} // namespace

auto completion(context& ctx, const cli::match_result& args, const cli::cmd& root) -> handler_result {
  // The parser enforces `required = true` on <shell>, so an absent value
  // here is unreachable from argv — it is a parse error (exit 1) long
  // before dispatch. Treated as invalid input rather than asserted, since
  // a direct in-process caller (handlers.t.cpp) can construct one.
  auto const shell_name = positional_string(args, "shell");
  if (!shell_name.has_value()) {
    return std::unexpected(
        error_from_body(cli::domain_error_kind::invalid_input, "unsupported shell ''; supported: bash, zsh, fish"));
  }

  std::optional<cli::shell> target;
  if (*shell_name == "bash") {
    target = cli::shell::bash;
  } else if (*shell_name == "zsh") {
    target = cli::shell::zsh;
  } else if (*shell_name == "fish") {
    target = cli::shell::fish;
  }
  if (!target.has_value()) {
    // Oracle-captured body, verbatim: the Zig handler's
    // `exit.die(ctx, error.InvalidInput, "unsupported shell '{s}'; supported: bash, zsh, fish", ...)`
    // renders as `error: <body>` on stderr with exit 2.
    return std::unexpected(error_from_body(cli::domain_error_kind::invalid_input,
                                           std::format("unsupported shell '{}'; supported: bash, zsh, fish", *shell_name)));
  }

  // `generate_script` returns the whole script; the Zig handler writes it
  // with `writeAll` and appends nothing, so neither does this.
  ctx.out() << cli::generate_script(root, *target);
  return {};
}

} // namespace planar::cmd::watch::handlers
