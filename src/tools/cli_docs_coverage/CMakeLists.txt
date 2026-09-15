# cli_docs_coverage — a regrowth guard against undocumented CLI leaves
# (plan 996, task 6758). See main.cpp's header for the full contract.
#
# Standalone executable, not a planar_module() target, same shape as its
# sibling cli_usage_lint — it shells the binary it checks rather than
# linking its command tree, so the only first-party dependency is
# planar_json_dom (to parse `planar schema` output).

add_executable(cli_docs_coverage main.cpp)
set_target_properties(cli_docs_coverage PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_MODULE_STD ON)
if(PLANAR_WARNINGS_AS_ERRORS)
  target_compile_options(cli_docs_coverage PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Werror>)
endif()
target_link_libraries(cli_docs_coverage PRIVATE planar_json_dom)
