/// @file check.cpp
/// @brief planar.core.check — the cold failure path (task 6347).

module planar.core.check;

import std;

[[noreturn]] void fail(std::string_view expr, const std::source_location& loc) {
  // Written directly rather than through planar.log: a broken invariant may
  // mean the logger's own state is unsound, and this must not depend on
  // anything that could itself be the thing that failed.
  std::cerr << std::format("planar: invariant violated: {}\n  at {}:{} in {}\n", expr, loc.file_name(), loc.line(),
                           loc.function_name());
  std::cerr.flush();
  std::abort();
}
