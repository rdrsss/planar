# planar_http — the HTTP transport (plan 996, task 6041).
#
# Layer-1 base library (cmake/architecture.cmake): it depends on no planar_*
# target at all, only on the vendored libcurl. It is layer 1 rather than part
# of an engine bucket because TWO layer-2 buckets need the vocabulary and D18
# forbids the edge between them — see http.cppm's header for the argument in
# full, and `scope_ref` / `json_text` for the two prior instances of the same
# move (D19).
#
# ## Why libcurl and not something smaller
#
# The M7 acceptance criterion names it, and the dependency set at the top of
# cmake/dependencies.cmake has reserved a slot for it since M0. The
# alternatives were considered and are worse: a hand-rolled client over raw
# sockets would still need a TLS library (both remotes are HTTPS-only), and a
# header-only client such as cpp-httplib delegates its TLS to OpenSSL anyway,
# so it trades one vendored C dependency for a different one plus a
# hand-audited HTTP implementation. libcurl is taken the same way every other
# application dependency here is — pinned URL + SHA256 through CPM, never
# `find_package(CURL)`, even though a system libcurl 8.7.1 is present on this
# machine and taking it would have been one line.
#
# ## The test binary reaches the loopback interface, and nothing else
#
# `http.t.cpp` exercises `curl_transport` against `fixture_server.hpp`, an
# in-process HTTP/1.1 server bound to 127.0.0.1 on a kernel-assigned port.
# There is no hostname to resolve and no route off the machine: the port is
# only knowable by asking the server object. See fixture_server.hpp's header
# for why a transport stub could not make the same statement (it bypasses
# every line of curl_transport, which is precisely the code under test here).
#
# STDOUT TERMINATOR CONTRACT: not applicable — this module renders nothing.

planar_module(http
  INTERFACE http.cppm
  SOURCES http.cpp)

target_link_libraries(planar_http PRIVATE CURL::libcurl)

if(TARGET planar_http_tests)
  target_link_libraries(planar_http_tests PRIVATE CURL::libcurl)
endif()
