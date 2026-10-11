/// @file command.cppm
/// @brief Dispatch the planar-execute schema command.
module;
export module planar.cmd.planar_execute.handlers.schema.command;
import std;
import planar.cliapp.schema;
import planar.cmd.planar_execute.catalog;
namespace planar::cmd::execute::handlers::schema {
/// @brief Execute this command.
///
/// `planar-execute` keeps its manual parser, so the two selection flags are
/// read here: `--command <path>` / `--command=<path>` and `--compact`. Any
/// other token is ignored, as it always was. A `--command` with no value is
/// treated as absent.
/// @param args The tokens after `schema`.
/// @param out Standard output.
/// @param err Standard error, which receives the message for an unknown command.
/// @return Process exit code: 0, or 2 when `--command` names no command.
export auto execute(std::span<std::string const> args, std::ostream& out, std::ostream& err) -> int {
  planar::cliapp::schema_request request;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--compact") {
      request.compact = true;
    } else if (args[i] == "--command" && i + 1 < args.size()) {
      request.command = args[++i];
    } else if (args[i].starts_with("--command=")) {
      request.command = args[i].substr(std::string_view{"--command="}.size());
    }
  }
  auto const selected = planar::cliapp::select_schema(catalog_json(), request);
  if (!selected) {
    err << "planar-execute: " << selected.error() << '\n';
    return 2;
  }
  out << *selected << '\n';
  return 0;
}
} // namespace planar::cmd::execute::handlers::schema
