/// @file completion.cpp
/// @brief Implementation of `planar.cmd.planar_watch.handlers.completion`.

module planar.cmd.planar_watch.handlers.completion;

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.completion;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.exit;
import planar.cmd.planar_watch.handler;

namespace planar::cmd::watch::handlers {

auto completion(context& ctx, const cliapp::parsed_args& args, const CLI::App& root) -> handler_result {
  // The parser enforces `required = true` on <shell>, so an absent value
  // here is unreachable from argv — it is a parse error (exit 1) long
  // before dispatch. Treated as invalid input rather than asserted, since
  // a direct in-process caller (handlers.t.cpp) can construct one.
  auto const shell_name = cliapp::positional_string(args, "shell");
  if (!shell_name.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "unsupported shell ''; supported: bash, zsh, fish"));
  }

  std::optional<cliapp::shell> target;
  if (*shell_name == "bash") {
    target = cliapp::shell::bash;
  } else if (*shell_name == "zsh") {
    target = cliapp::shell::zsh;
  } else if (*shell_name == "fish") {
    target = cliapp::shell::fish;
  }
  if (!target.has_value()) {
    // Oracle-captured body, verbatim: the Zig handler's
    // `exit.die(ctx, error.InvalidInput, "unsupported shell '{s}'; supported: bash, zsh, fish", ...)`
    // renders as `error: <body>` on stderr with exit 2.
    return std::unexpected(error_from_body(domain_error_kind::invalid_input,
                                           std::format("unsupported shell '{}'; supported: bash, zsh, fish", *shell_name)));
  }

  // `generate_script` returns the whole script; the Zig handler writes it
  // with `writeAll` and appends nothing, so neither does this.
  ctx.out() << cliapp::generate_script(root, *target);
  return {};
}

} // namespace planar::cmd::watch::handlers
