/// @file add.cppm
/// @brief CLI declaration for `artifact add`.
export module planar.cmd.planar.handlers.artifact.add;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::artifact_cli {
/// @brief Register the add CLI node.
/// @param artifact Input artifact.
/// @return Registered CLI node.
export auto attach_add(CLI::App& artifact) -> CLI::App* {
  CLI::App* add = artifact.add_subcommand("add", "Register a new artifact.");
  add_string(*add, "--body", "Artifact body text; @<file> reads it from a file");
  add_string_required(*add, "--kind", "Artifact kind, e.g. tech_spec, adr, design_note, research");
  add_string(*add, "--from-file", "Read the body from this file; also sets the source path");
  add_string(*add, "--source-path", "Path of the source file the artifact mirrors");
  add_string(*add, "--scope", "Scope slug to create in (default: cwd-derived write scope)");
  add_string_default(*add, "--status", "draft", "Initial status: draft, active, superseded, retired (default draft)");
  add_int(*add, "--plan", "Attach to this plan id via a derives-from link");
  add_bool_default_true(*add, "--editor", "Accepted for parity; not read by the handler");
  add_json(*add, "Emit machine-readable JSON instead of text");
  add_positional(*add, "title", "Artifact title");
  return add;
}
} // namespace planar::cmd::handlers::artifact_cli
