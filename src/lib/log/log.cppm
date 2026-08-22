/// @file log.cppm
/// @brief `planar.log` — spdlog-backed text/JSON dual-mode logging setup
/// (task cpp-cli-output-logging, D11: "spdlog is the logging layer, keep
/// text/JSON dual mode").
///
/// Replaces the Zig side's `std.log.scoped(.<module>)` pattern
/// (CLAUDE.md § Zig Style: "Structured fields via `std.log.scoped(.
/// <module>)`. JSON output for production; text for dev."). `scoped(name)`
/// below is the direct analog: a named logger sharing the process default
/// logger's sinks, mode, and level, so callers get per-module logging
/// without each one re-deriving sink/formatter setup.
///
/// The raw `spdlog::logger`/`spdlog::sinks::sink` types are only ever
/// forward-declared here and passed/returned as (shared-pointer) opaque
/// handles — the full `<spdlog/spdlog.h>` include lives in log.cpp's
/// global module fragment, mirroring db.cppm's treatment of the raw
/// `sqlite3*` C API type.
module;

namespace spdlog {
class logger;
namespace sinks {
class sink;
}
} // namespace spdlog

export module planar.log;

import std;

namespace planar::log {

/// @brief Which formatter a logger writes with. `text` is the
/// human-readable dev-console form; `json` is the structured
/// machine-parseable form (D11's "production" mode).
export enum class mode : std::uint8_t {
  text,
  json,
};

/// @brief Log severity, mirroring spdlog::level::level_enum's ordering
/// without exposing the spdlog enum type itself in this interface.
export enum class level : std::uint8_t {
  trace,
  debug,
  info,
  warn,
  err,
  critical,
  off,
};

/// @brief Build a logger named `name` writing to `sink`, formatted per
/// `m`, at severity `lvl`. Exposed primarily so callers (and tests) can
/// supply their own sink — e.g. an in-memory ostream sink — and inspect
/// output deterministically without going through the process-global
/// default logger `init()` installs.
/// @param name The logger's name (appears in text-mode output as `[name]`
/// and in JSON-mode output as the `"logger"` field).
/// @param m Which formatter to attach to `sink`.
/// @param sink The spdlog sink to write formatted records to.
/// @param lvl The minimum severity this logger emits.
/// @return A logger ready to use.
export auto make_logger(std::string_view name, mode m, std::shared_ptr<spdlog::sinks::sink> sink, level lvl = level::info)
    -> std::shared_ptr<spdlog::logger>;

/// @brief Install a stdout-color-sink logger named `"planar"` as the
/// process default logger, in mode `m` at severity `lvl`. Call once, as
/// early as possible in a binary's `main`. Mirrors the Zig side reading
/// `[log]` config to choose text-vs-JSON at startup (M3+'s config module
/// is expected to call this with the resolved mode).
/// @param m Which formatter the default logger uses.
/// @param lvl The minimum severity the default logger (and every
/// subsequently created `scoped()` logger) emits.
export auto init(mode m, level lvl = level::info) -> void;

/// @brief Return a logger scoped to `name`, sharing the process default
/// logger's sinks, formatter mode, and level — the C++ analog of Zig's
/// `std.log.scoped(.<module>)`. Idempotent: calling it twice with the
/// same `name` returns the SAME underlying logger (spdlog's registry),
/// not a fresh one with independent state. If `init()` has not been
/// called yet, falls back to spdlog's own bootstrap default logger (a
/// plain unformatted stdout sink) so a `scoped()` call before `init()`
/// never crashes — it simply logs unstructured until `init()` runs.
/// @param name The scope name (typically a module name, e.g. `"db"`).
/// @return The (possibly newly created) scoped logger.
export auto scoped(std::string_view name) -> std::shared_ptr<spdlog::logger>;

} // namespace planar::log
