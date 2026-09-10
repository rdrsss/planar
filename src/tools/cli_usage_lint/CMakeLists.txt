# cli_usage_lint — C++ port of zig/tools/cli_usage_lint.zig (plan 996, task
# 6402). Validates authored CLI invocations against the live `<bin> schema`
# catalog. See main.cpp's header for the full contract.
#
# Standalone executable, not a planar_module() target — it shells the
# binaries it lints rather than linking their command trees, so the only
# first-party dependency is planar_json_dom (to parse `<bin> schema`
# output).

add_executable(cli_usage_lint main.cpp)
set_target_properties(cli_usage_lint PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_MODULE_STD ON)
if(PLANAR_WARNINGS_AS_ERRORS)
  target_compile_options(cli_usage_lint PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Werror>)
endif()
target_link_libraries(cli_usage_lint PRIVATE planar_json_dom)
