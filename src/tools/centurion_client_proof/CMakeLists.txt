# centurion_client_proof — the tech-spec D9 toolchain proof (plan 1033 M1,
# task 6497): a Catch2 test that links `centurion::client` under Planar's
# pinned LLVM and C++26, starts an installed stock centuriond, and completes
# readiness over its Unix socket. See client_proof.t.cpp.
#
# EXCLUDE_FROM_ALL and NOT registered with ctest: building it compiles
# Centurion's gRPC stack (~6 min cold) and running it needs an installed
# centuriond (another cold gRPC build), so `make test` would get neither for
# free. `make centurion-client-proof` builds both and runs it. It becomes part
# of the default lane once planar-execute itself links the client (M2), at
# which point the gRPC cost is paid by the default build anyway.
#
# A plain target, not planar_module()/planar_binary(): it is not a Planar
# module and sits outside the D15 walk on purpose. The Centurion boundary's
# closure rule (cmake/architecture.cmake, rule 3) still grades
# centurion::client itself.

if(NOT TARGET Catch2::Catch2WithMain OR NOT TARGET centurion::client)
  message(FATAL_ERROR "centurion_client_proof needs Catch2 and centurion::client")
endif()

add_executable(centurion_client_proof EXCLUDE_FROM_ALL client_proof.t.cpp)
set_target_properties(centurion_client_proof PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_MODULE_STD ON
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_link_libraries(centurion_client_proof PRIVATE centurion::client Catch2::Catch2WithMain)
