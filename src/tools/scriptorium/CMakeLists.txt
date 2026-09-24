add_executable(scriptorium main.cpp core.cpp)
target_sources(scriptorium PRIVATE FILE_SET CXX_MODULES FILES core.cppm)
set_target_properties(scriptorium PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_MODULE_STD ON
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_include_directories(scriptorium SYSTEM PRIVATE
  "${glaze_SOURCE_DIR}/include"
  "${inja_SOURCE_DIR}/include"
  "${nlohmann_json_SOURCE_DIR}/include")
if(PLANAR_WARNINGS_AS_ERRORS)
  target_compile_options(scriptorium PRIVATE -Werror)
endif()
install(TARGETS scriptorium RUNTIME DESTINATION bin)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME scriptorium_projection
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/core.test.py"
    "$<TARGET_FILE:scriptorium>" "${PROJECT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/planar-golden.sha256")
