/// @file log.cpp
/// @brief Implementation of `planar.log` (see log.cppm).
///
/// The vendored spdlog and Glaze headers are confined to this translation
/// unit's global module fragment — no other file in the module purview
/// names a raw `spdlog::*` or `glz::*` type (log.cppm only forward-
/// declares the two opaque handle types it hands back to callers).
module;

#include <spdlog/sinks/sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <glaze/glaze.hpp>

module planar.log;

import std;

namespace planar::log {

namespace detail {

/// @brief Map this module's `level` to spdlog's own level enum.
/// @param lvl The `planar::log::level` to translate.
/// @return The equivalent `spdlog::level::level_enum` value.
auto to_spdlog_level(level lvl) -> spdlog::level::level_enum {
  switch (lvl) {
  case level::trace:
    return spdlog::level::trace;
  case level::debug:
    return spdlog::level::debug;
  case level::info:
    return spdlog::level::info;
  case level::warn:
    return spdlog::level::warn;
  case level::err:
    return spdlog::level::err;
  case level::critical:
    return spdlog::level::critical;
  case level::off:
    return spdlog::level::off;
  }
  return spdlog::level::info;
}

/// @brief One structured JSON log record. Deliberately NOT in an
/// anonymous namespace: Glaze's compile-time reflection needs external
/// linkage on a reflected type (see src/lib/core/vendor_probe.cpp's file
/// comment for the same constraint on its probe payload type) —
/// an anonymous-namespace type fails to compile under glz::write_json.
///
/// `ts_ms` is milliseconds-since-epoch rather than a formatted ISO8601
/// string: spdlog's `log_msg::time` is a `log_clock::time_point`
/// (`system_clock` under the default, non-coarse clock build), and a raw
/// epoch integer is trivially exact and trivially testable without
/// pulling in a second formatting dependency for calendar math this
/// module does not otherwise need. A JSON log consumer can format it
/// however it likes; nothing here depends on the string shape.
struct json_log_record {
  std::int64_t ts_ms = 0; ///< Milliseconds since the Unix epoch.
  std::string  level;     ///< Stable spdlog level name (e.g. "info", "warning").
  std::string  logger;    ///< The emitting logger's name.
  std::string  message;   ///< The formatted log payload.
};

/// @brief spdlog formatter that renders one line of `json_log_record` per
/// log call, matching D11's "JSON dual mode" requirement.
class json_formatter final : public spdlog::formatter {
public:
  /// @brief Render one JSON line for `msg` into `dest`.
  /// @param msg The spdlog record to format.
  /// @param dest The destination buffer to append the formatted line to.
  auto format(spdlog::details::log_msg const& msg, spdlog::memory_buf_t& dest) -> void override {
    json_log_record rec{
        .ts_ms   = std::chrono::duration_cast<std::chrono::milliseconds>(msg.time.time_since_epoch()).count(),
        .level   = std::string(spdlog::level::to_string_view(msg.level).data(), spdlog::level::to_string_view(msg.level).size()),
        .logger  = std::string(msg.logger_name.data(), msg.logger_name.size()),
        .message = std::string(msg.payload.data(), msg.payload.size()),
    };
    auto json = glz::write_json(rec).value_or("{\"error\":\"json log serialization failed\"}");
    dest.append(json.data(), json.data() + json.size());
    dest.push_back('\n');
  }

  /// @brief Return a fresh copy of this (stateless) formatter.
  /// @return A new `json_formatter` instance.
  [[nodiscard]] auto clone() const -> std::unique_ptr<spdlog::formatter> override {
    return std::make_unique<json_formatter>();
  }
};

} // namespace detail

auto make_logger(std::string_view name, mode m, std::shared_ptr<spdlog::sinks::sink> sink, level lvl)
    -> std::shared_ptr<spdlog::logger> {
  if (m == mode::json) {
    sink->set_formatter(std::make_unique<detail::json_formatter>());
  } else {
    // Text mode: timestamp, logger name, level, message — the same field
    // set the JSON formatter emits, just human-readable.
    sink->set_pattern("%Y-%m-%dT%H:%M:%S.%e [%n] [%l] %v");
  }
  auto logger = std::make_shared<spdlog::logger>(std::string(name), sink);
  logger->set_level(detail::to_spdlog_level(lvl));
  return logger;
}

auto init(mode m, level lvl) -> void {
  // Mirrors zig: main.zig installs no std.log logFn override, so std.log
  // writes to stderr (its documented default). Logging to stdout would
  // interleave log lines into --json payloads (see log.t.cpp's
  // stdout-clean test).
  auto sink   = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
  auto logger = make_logger("planar", m, sink, lvl);
  spdlog::set_default_logger(logger);
  spdlog::set_level(detail::to_spdlog_level(lvl));
}

auto scoped(std::string_view name) -> std::shared_ptr<spdlog::logger> {
  std::string name_str{name};
  if (auto existing = spdlog::get(name_str)) {
    return existing;
  }

  auto default_logger = spdlog::default_logger();
  auto sinks          = default_logger ? default_logger->sinks() : std::vector<spdlog::sink_ptr>{};
  auto logger         = std::make_shared<spdlog::logger>(name_str, sinks.begin(), sinks.end());
  logger->set_level(default_logger ? default_logger->level() : spdlog::level::info);
  spdlog::register_logger(logger);
  return logger;
}

} // namespace planar::log
