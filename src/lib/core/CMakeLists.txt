# planar.core — M0 scaffold seed module (plan 996, task cpp-scaffold-presets).
#
# Catch2 is vendored as of task 6023 (cmake/dependencies.cmake);
# version.t.cpp closes the seed-test gap that cycle 3 documented and left
# open. planar_module() globs *.t.cpp automatically, so no SOURCES/test
# wiring is needed here beyond the file existing.

planar_module(core
  INTERFACE version.cppm)

# --- vendor probe (task 6023, scope item 4) --------------------------------
#
# Deliberately not a planar_module() target (see vendor_probe.cpp's header
# comment) — a standalone executable proving glaze/spdlog/the vendored
# SQLite amalgamation compile and link under -Werror without their own
# headers tripping it (SYSTEM YES on every CPMAddPackage in
# cmake/dependencies.cmake).
add_executable(planar_vendor_probe vendor_probe.cpp)
set_target_properties(planar_vendor_probe PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON)
if(PLANAR_WARNINGS_AS_ERRORS)
  target_compile_options(planar_vendor_probe PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Werror>)
endif()
target_link_libraries(planar_vendor_probe PRIVATE
  glaze::glaze
  spdlog::spdlog
  SQLite::SQLite3)
