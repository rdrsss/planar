/// @file docs.cppm
/// @brief `planar.cmd.planar_watch.docs` — the `planar-watch` binary's per-command examples and
/// exit codes, supplied as data beside its summary table.
///
/// `CLI::App` has no slot for either, so they live here and reach two
/// consumers: `planar.cliapp.schema::schema_json` emits them as each leaf's
/// `docs.examples` and `docs.exitCodes`, and `install_docs_footers` renders
/// them as the `Examples:` and `Exit codes:` sections of that leaf's
/// `--help`. One table feeds both, so the two cannot drift. Only leaf verbs
/// have rows. Each leaf lists the exit codes its handler can return: the
/// binary-wide meaning of a code is in `k_meanings`, and a verb whose code
/// means something narrower overrides it in `k_overrides`.
module;

export module planar.cmd.planar_watch.docs;

import std;
import planar.cliapp.schema;

namespace planar::cmd::watch {

/// @brief The examples and exit codes of every `planar-watch` leaf verb.
///
/// The returned table borrows static storage and is valid for the life of
/// the process.
/// @return The table.
export auto surface_docs() -> const cliapp::command_docs& {
  static constexpr std::pair<std::string_view, std::string_view> k_examples[] = {
      {"planar-watch feed", R"(planar-watch feed --tail 20)"},
      {"planar-watch feed", R"(planar-watch feed --follow --vendor claude)"},
      {"planar-watch ps", R"(planar-watch ps)"},
      {"planar-watch ps", R"(planar-watch ps --stale --sort-by lease)"},
      {"planar-watch claims", R"(planar-watch claims --status active)"},
      {"planar-watch claims", R"(planar-watch claims --plan 7 --json)"},
      {"planar-watch actions", R"(planar-watch actions --task 42 --limit 20)"},
      {"planar-watch plans", R"(planar-watch plans --in-flight-only)"},
      {"planar-watch log", R"(planar-watch log --task 42)"},
      {"planar-watch log", R"(planar-watch log --plan 7 --limit 50)"},
      {"planar-watch tree", R"(planar-watch tree --root-session 3)"},
      {"planar-watch run list", R"(planar-watch run list --plan 7 --status running)"},
      {"planar-watch run show", R"(planar-watch run show 3)"},
      {"planar-watch sync-events", R"(planar-watch sync-events --plan 7 --outcome conflict)"},
      {"planar-watch queue history", R"(planar-watch queue history --since 24h)"},
      {"planar-watch diagnose", R"(planar-watch diagnose --plan 7)"},
      {"planar-watch diagnose", R"(planar-watch diagnose --days 3 --check claim-lease-lapsed --json)"},
  };
  static constexpr std::pair<std::string_view, std::string_view> k_exit_codes[] = {
      {"planar-watch feed", "0 1 2 7"},          {"planar-watch ps", "0 1 2 7"},
      {"planar-watch claims", "0 1 2 7"},        {"planar-watch actions", "0 1 2 7"},
      {"planar-watch plans", "0 1 2 7"},         {"planar-watch log", "0 1 2 7"},
      {"planar-watch tree", "0 1 2 7"},          {"planar-watch run list", "0 1 2 7"},
      {"planar-watch run show", "0 1 2 7"},      {"planar-watch sync-events", "0 1 2 7"},
      {"planar-watch queue history", "0 1 2 7"}, {"planar-watch diagnose", "0 1 2 7"},
      {"planar-watch version", "0 1"},           {"planar-watch completion", "0 1 2"},
      {"planar-watch schema", "0 1 2"},
  };
  static constexpr cliapp::exit_code_doc k_meanings[] = {
      {0, R"(Success.)"},
      {1, R"(Failure: entity not found, an unmapped error, or a usage error such as an unknown flag.)"},
      {2, R"(Bad input: an invalid value or entity ref.)"},
      {7, R"(The database schema is behind or ahead of this binary.)"},
  };
  static constexpr cliapp::exit_meaning_override k_overrides[] = {
      {"planar-watch diagnose", 0, R"(The run completed, whatever it found. Findings never change the exit status.)"},
      {"planar-watch diagnose", 1,
       R"(The run did not complete: the database stayed locked past 250 ms, a query failed or the planning tables are missing. `diagnose: unavailable (<reason>)` is printed.)"},
      {"planar-watch diagnose", 2, R"(Bad input: an unknown --check or plan, --days below 1, or an unknown flag.)"},
  };
  static constexpr cliapp::command_docs k_docs{k_examples, k_exit_codes, k_meanings, k_overrides};
  return k_docs;
}

} // namespace planar::cmd::watch
