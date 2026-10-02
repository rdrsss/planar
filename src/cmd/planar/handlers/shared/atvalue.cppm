/// @file atvalue.cppm
/// @brief `planar.cmd.planar.handlers.atvalue` -- the `@<file>` grammar for
/// free-text flags and positionals (tasks 6848, 7117).
///
/// docs/cli-reference.md documents several free-text inputs as "May be
/// `@<file>`": a value whose FIRST byte is `@` names a file whose bytes
/// replace the value, read raw. This module is the one handler-layer wrapper
/// over the engine's `read_body` for it, so every verb refuses an unreadable
/// file the same way: `error: read <label>: FileNotFound` at exit 2, before
/// anything is written. A literal `@path` token must never reach a stored
/// column silently.
///
/// Every fallible boundary surfaces `std::expected<T, domain_error>`; no
/// exceptions cross the module boundary.
module;

export module planar.cmd.planar.handlers.atvalue;

import std;
import planar.cmd.planar.exit;

namespace planar::cmd::handlers {

/// @brief Resolve the `@<file>` grammar on an optional free-text value.
///
/// The refusal is the shape `artifact update --body` pinned (oracle
/// captured): `error: read --body: FileNotFound`, exit 2, and it does NOT
/// name the path.
/// @param raw The raw value, when the flag or positional was given at all.
/// @param label What the refusal calls the input, e.g. `--body` or `<body>`.
/// @return The resolved value (unset when `raw` was unset), or the refusal.
export auto resolve_at_value(std::optional<std::string> raw, std::string_view label)
    -> std::expected<std::optional<std::string>, domain_error>;

} // namespace planar::cmd::handlers
