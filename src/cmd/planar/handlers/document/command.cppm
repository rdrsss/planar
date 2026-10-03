/// @file src/cmd/planar/handlers/document/command.cppm
/// @brief Read-only, revision-bound canonical document projection commands.
module;

export module planar.cmd.planar.handlers.document;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Emit an authoritative `block-document-v1` projection.
/// @param ctx The invocation context.
/// @param args Parsed command arguments.
/// @return Success; `invalid_input` (exit 2) for an unsupported `--kind`;
/// `not_found` (exit 1) for an absent source row; `generic_failure` (exit 1)
/// when the database cannot be opened read-only or a query fails;
/// `schema_version_ahead` (exit 7) / `schema_version_behind` (exit 1) on a
/// schema mismatch.
export auto document_project(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Validate a caller's complete adjacent range against one revision.
/// @param ctx The invocation context.
/// @param args Parsed command arguments.
/// @return Success; every `document_project` refusal; or `invalid_input`
/// (exit 2) with `document range rejected: <reason>` when the evidence does
/// not match the freshly projected snapshot.
export auto document_validate_range(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the read-only document command family.
/// @param root The root command tree.
export auto declare_document(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
