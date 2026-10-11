# planar_engine_hostqueue — engine/hostqueue bucket (plan 1080, task
# hq-enqueue): the host-wide build and test queue's engine. Its tables live in
# planar.db (plan 1089, migration 00040); it was over the retired agent
# database before that (decision 1181).
#
# Layer-2 `engine_*` module (cmake/architecture.cmake): may DEPEND on
# layer-1 modules only, never sideways onto another `engine_*` bucket
# (D15/D18). Tech spec 647 § Components makes that explicit for this bucket:
# it does not import `engine_config`; the `queue` handlers read the `[queue]`
# table and pass slot counts and intervals in as arguments, and process
# identity (host id, pid, start time) and both clocks arrive as plain values
# too, so the engine is testable with a scratch store and controlled
# identities.
#
# `db` supplies the connection wrapper and the embedded main migration chain
# the tests migrate a scratch planar.db with (`scratch_store.hpp`, the one place
# a test store's chain is chosen). `json_text` is the one shared RFC 8259 string
# escape used to write the `argv` and `cancelled_by` columns; `json_dom`
# parses them back.
#
# `history.cppm` (task hq-history) ends entries and prunes history; it
# imports `queue.cppm` for the shared error and argv types, and `queue.cpp`
# imports it back for the pruning enqueue (an implementation-unit import,
# so the interfaces form no cycle).
#
# `process` supplies `planar.process.identity`, which the liveness
# module's default probe forwards to (task hq-liveness); tests replace the
# probe with a fake.
#
# `poll.cppm` (task hq-poll-transaction) is the poll transaction; it
# imports queue, history and liveness from this bucket.
#
# `terminate.cppm` (task hq-terminate) stops a running entry's command in
# two steps, signalling only outside any transaction; it imports queue,
# history, liveness and poll from this bucket (poll for `poll_and_stop`,
# task hq-orphan-deadline, which signals what a poll marked after it
# commits). poll does not import terminate, so the interfaces form no cycle.
#
# `nested.cppm` (task hq-nested-entry) inserts a nested run's entry after
# checking its parent; it imports queue and liveness from this bucket, and
# `nested.cpp` imports terminate for its `same_host` test (decision 1209; an
# implementation-unit import, so the interfaces form no cycle).
#
# `guard.cppm` (task hq-command-guard) is the pure command guard: it imports
# nothing but the standard library.
#
# `rule.cppm` (task hq-rule-text) exposes the agent rule text. The one
# authored source, `queue-rule.md`, lives in the layer-1 `queuerule` module
# (`src/lib/queuerule/`, task 7110), which embeds it; `rule.cpp` forwards to
# that module's `queue_rule_text()` so the API here is unchanged.
#
# `schema.cppm` (plan 1089, task qp-queue-compat) is the queue tables' own
# compatibility check inside planar.db: the `queue_schema` marker, the
# hand-maintained column lists and the protocol constants. It imports only
# `db`.
#
# `hostqueue.cppm` is the umbrella re-export, matching
# `src/engine/closure/closure.cppm`.

planar_module(engine_hostqueue
  INTERFACE
    queue.cppm
    history.cppm
    liveness.cppm
    poll.cppm
    terminate.cppm
    nested.cppm
    status.cppm
    observer.cppm
    guard.cppm
    rule.cppm
    schema.cppm
    hostqueue.cppm
  SOURCES
    queue.cpp
    history.cpp
    liveness.cpp
    poll.cpp
    terminate.cpp
    nested.cpp
    status.cpp
    observer.cpp
    guard.cpp
    rule.cpp
    schema.cpp
  DEPENDS
    db
    json_dom
    json_text
    process
    queuerule
  TEST_DEPENDS
    sha256)

if(TARGET planar_engine_hostqueue_tests)
  # The test binary opens real on-disk scratch planar.db files migrated from
  # the main chain (scratch_store.hpp) and probes the schema with raw SQL,
  # exactly as engine/closure's suite does.
  target_link_libraries(planar_engine_hostqueue_tests PRIVATE SQLite::SQLite3)
  # rule_methodology.t.cpp reads the authored agents/methodology.md from the
  # source tree to compare its queue section with the embedded rule text.
  # schema.t.cpp fingerprints the SQL literals of this directory's own
  # `*.cpp` sources (tech spec 656 § Enforcement), so it reads them from the
  # source tree. separability.t.cpp scans every first-party source under
  # `src/` for SQL literals that name a queue table (tech spec 656 §
  # Separability), so it is told where the tree is.
  target_compile_definitions(planar_engine_hostqueue_tests PRIVATE
    "PLANAR_METHODOLOGY_PATH=\"${PROJECT_SOURCE_DIR}/agents/methodology.md\""
    "PLANAR_HOSTQUEUE_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\""
    "PLANAR_SOURCE_ROOT=\"${PROJECT_SOURCE_DIR}/src\"")
endif()
