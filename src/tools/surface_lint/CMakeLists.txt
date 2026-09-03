# surface_lint — C++ port of zig/tools/surface_lint.zig (plan 996, task
# 6402). Deterministic semantic validator for authored agent/skill/doc
# Markdown. See main.cpp's header for the full contract.
#
# Standalone executable, not a planar_module() target — no first-party
# module dependency at all; it does its own JSON emission with a local
# escaper (see main.cpp's header for why it does not reuse planar_json_text).

add_executable(surface_lint main.cpp)
set_target_properties(surface_lint PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_MODULE_STD ON)
if(PLANAR_WARNINGS_AS_ERRORS)
  target_compile_options(surface_lint PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Werror>)
endif()
