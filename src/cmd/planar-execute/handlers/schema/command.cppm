/// @file command.cppm
/// @brief Dispatch the planar-execute schema command.
module;
export module planar.cmd.planar_execute.handlers.schema.command;
import std;
import planar.cmd.planar_execute.catalog;
namespace planar::cmd::execute::handlers::schema {
/// @brief Execute this command.
/// @param out Input out.
/// @return Process exit code.
export auto execute(std::ostream& out) -> int {
  out << catalog_json() << '\n';
  return 0;
}
} // namespace planar::cmd::execute::handlers::schema
