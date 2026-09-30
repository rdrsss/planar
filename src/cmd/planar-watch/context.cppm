/// @file context.cppm
/// @brief Binary-facing name for the shared, database-injected invocation context.
module;
export module planar.cmd.planar_watch.context;
import std;
import planar.cmd.internal.context;
import planar.cmd.internal.environment;
export import planar.cmd.planar_watch.database;
import planar.cmd.planar_watch.exit;

namespace planar::cmd::watch {
/// @brief Environment lookup used by this command binary.
export using env_lookup = internal::env_lookup;
/// @brief Invocation context with an injected database object.
export using context = internal::context<database>;
/// @brief Look up variables from the process environment.
/// @return Environment lookup callable.
export auto process_env() -> env_lookup;
/// @brief Create an environment lookup from supplied values.
/// @param vars Environment values.
/// @return Environment lookup callable.
export auto map_env(std::map<std::string, std::string, std::less<>> vars) -> env_lookup;
/// @brief Resolve the command database path.
/// @param env Environment lookup.
/// @return Database path or a command error.
export auto resolve_db_path(const env_lookup& env) -> std::expected<std::filesystem::path, domain_error>;
/// @brief Whether an invocation needs the main database's path resolved
/// before dispatch.
///
/// Every verb does, except the `queue` domain (plan 1080, task hq-watch-queue).
/// Its store is the agent database (`agentstore.cppm`), located from
/// `PLANAR_AGENT_DB` / `HOME` on its own, and the queue view must keep working
/// when the main database cannot be located (no `PLANAR_DB`, no `HOME`). The
/// check is on the first argument after the program name, the verb position:
/// anything else, an empty vector and a leading flag included, keeps the
/// resolution and its exit code.
/// @param args The full argument vector, program name first.
/// @return `false` only when the first argument is `queue`.
export auto uses_main_database(std::span<const std::string> args) -> bool;
/// @brief Resolve this binary's operator working directory.
/// @param env Environment lookup.
/// @return Working directory path.
export auto operator_cwd(const env_lookup& env) -> std::filesystem::path;
} // namespace planar::cmd::watch
