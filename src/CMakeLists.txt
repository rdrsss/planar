# src/ — tech-spec § Target layout / § File-level tree.
#
# `cmd/` (operator/agent-callable binaries) is not wired in yet — it lands
# alongside the modules its handlers link, per the port order in the
# tech-spec's § engine buckets ("identity -> planning -> workbench -> runtime
# (agent) -> external -> execute"). Only `lib/` exists at this M0 scaffold
# task.

add_subdirectory(lib)
