# Planar — C++26 build/test entry points.
#
# Targets here are thin wrappers around the canonical CMake presets. They
# exist to give one consistent surface for humans and CI.
#
# The C++ implementation is built with the debug/release CMake presets.
# `cli_usage_lint` and `surface_lint` (src/tools/, plan 996 task 6402) are
# CMake targets in the same build as every other binary. NOTHING in this
# file shells `zig` any more: the M10 cutover (task 6045, decisions 963/982)
# deleted `zig/` outright, and the targets that drove it were removed rather
# than stubbed — see the note above `test-cpp-report`.

BINARY        := planar
AGENT_BINARY  := planar-agent
WATCH_BINARY  := planar-watch
EXECUTE_BINARY := planar-execute
EXT_BINARY    := planar-ext
BIN_DIR       := bin
BIN           := $(BIN_DIR)/$(BINARY)
AGENT_BIN     := $(BIN_DIR)/$(AGENT_BINARY)
WATCH_BIN     := $(BIN_DIR)/$(WATCH_BINARY)
EXECUTE_BIN   := $(BIN_DIR)/$(EXECUTE_BINARY)
EXT_BIN       := $(BIN_DIR)/$(EXT_BINARY)

PREFIX      ?= $(HOME)/.local

# CMake build output — binaries land under `build/<preset>/bin` per
# CMakePresets.json. Override to point a target at a different build
# directory.
CPP_BIN_DIR   ?= build/debug/bin
CPP_BIN_ABS   := $(abspath $(CPP_BIN_DIR))
CPP_RELEASE_BIN_DIR ?= build/release/bin

CPP_BUILD_DIR ?= build/debug
CLI_USAGE_LINT := $(CPP_BUILD_DIR)/src/tools/cli_usage_lint/cli_usage_lint
SURFACE_LINT := $(CPP_BUILD_DIR)/src/tools/surface_lint/surface_lint

# Extra args forwarded to the relevant underlying build command.
ARGS        ?=

.DEFAULT_GOAL := help

.PHONY: help
help:
	@awk 'BEGIN {FS = ":.*##"; printf "Targets:\n"} /^[a-zA-Z0-9_.-]+:.*##/ {printf "  \033[36m%-22s\033[0m %s\n", $$1, $$2}' $(MAKEFILE_LIST)

.PHONY: build
build: ## Build the planar + planar-agent + planar-watch + planar-execute + planar-ext binaries into ./bin/ (at repo root)
	@mkdir -p $(BIN_DIR)
	cmake --preset release -DPLANAR_VERSION_META=OFF
	cmake --build build/release $(ARGS)
	@cp -f $(CPP_RELEASE_BIN_DIR)/$(BINARY) $(BIN)
	@cp -f $(CPP_RELEASE_BIN_DIR)/$(AGENT_BINARY) $(AGENT_BIN)
	@cp -f $(CPP_RELEASE_BIN_DIR)/$(WATCH_BINARY) $(WATCH_BIN)
	@cp -f $(CPP_RELEASE_BIN_DIR)/$(EXECUTE_BINARY) $(EXECUTE_BIN)
	@cp -f $(CPP_RELEASE_BIN_DIR)/$(EXT_BINARY) $(EXT_BIN)

.PHONY: install
install: ## Build and install the five Planar executables into PREFIX/bin (default: ~/.local/bin)
	cmake --preset release -DPLANAR_VERSION_META=ON
	cmake --build build/release $(ARGS)
	cmake --install build/release --prefix $(PREFIX)

.PHONY: install-bin
install-bin: install ## Compatibility alias for the binary-only install

.PHONY: install-full
install-full: ## Legacy full install: binaries plus skills, agents, workflows, and vendor wiring
	./install.sh $(INSTALL_FLAGS)

.PHONY: uninstall
uninstall: ## Remove the five Planar executables from PREFIX/bin
	rm -f $(PREFIX)/bin/$(BINARY)
	rm -f $(PREFIX)/bin/$(AGENT_BINARY)
	rm -f $(PREFIX)/bin/$(WATCH_BINARY)
	rm -f $(PREFIX)/bin/$(EXECUTE_BINARY)
	rm -f $(PREFIX)/bin/$(EXT_BINARY)

.PHONY: uninstall-full
uninstall-full: ## Remove the legacy full install (preserves ~/.planar/planar.db)
	./install.sh --uninstall $(INSTALL_FLAGS)

# Scratch database for hand-run smoke tests, kept in the build dir.
#
# A from-source binary resolves $PLANAR_DB, falling back to the operator's real
# ~/.planar/planar.db — and it applies its pending migrations AUTOMATICALLY on
# first use. So one bare run of a freshly built binary from a branch carrying a
# new migration silently pushes the live database past every installed binary's
# supported version and breaks every other agent on the machine. Always smoke
# against this instead.
SMOKE_DB ?= $(CPP_BUILD_DIR)/.smoke/planar.db

.PHONY: run
run: ## Run the CLI from the debug build AGAINST THE REAL DB (use `make smoke` for a throwaway one)
	cmake --build $(CPP_BUILD_DIR) --target planar_cmd_planar
	$(CPP_BIN_ABS)/$(BINARY) $(ARGS)

.PHONY: smoke
smoke: ## Run the CLI from the debug build against a throwaway DB: make smoke ARGS="task list"
	cmake --build $(CPP_BUILD_DIR) --target planar_cmd_planar
	@mkdir -p $(dir $(SMOKE_DB))
	PLANAR_DB=$(SMOKE_DB) $(CPP_BIN_ABS)/$(BINARY) $(ARGS)

.PHONY: smoke-reset
smoke-reset: ## Delete the throwaway smoke database
	rm -rf $(dir $(SMOKE_DB))

.PHONY: test-install-manifest
test-install-manifest: ## Run focused installer manifest ownership/atomicity fixtures
	bash scripts/install-manifest-test.sh

.PHONY: test-install-deps
test-install-deps: ## Run focused installer compiler-preflight fixture
	bash scripts/install-deps-test.sh

.PHONY: test
test: test-install-manifest test-install-deps ## Run unit tests
	cmake --preset debug
	cmake --build build/debug $(ARGS)
	ctest --test-dir build/debug --output-on-failure $(ARGS)

.PHONY: test-vendor-mtkahypar-offline
test-vendor-mtkahypar-offline: ## Recurrence guard (task 6461): -DPLANAR_WITH_MTKAHYPAR=ON must configure offline from the committed vendor cache. macOS/sandbox-exec only; not part of test-all (slow, platform-specific) -- run after touching cmake/dependencies.cmake's mtkahypar block, or wire into make test-cpp-solver, the lane that configures with the solver ON (task 6532).
	bash scripts/vendor-mtkahypar-offline-test.sh

# THE ZIG-HARNESS TARGETS ARE GONE (plan 996, task 6045; decisions 963/982,
# 1035). `oracle-retirement-gate`, `test-integration`,
# `test-integration-files` and `test-parity-cpp` all shelled `zig build`
# against `zig/`, which this commit deletes. They are removed rather than
# stubbed: a target that runs nothing and exits 0 is the exact failure mode
# the retired gate existed to prevent. Their coverage moved before the
# deletion, not with it — task 6546 gave the eight uncovered CLI leaves
# black-box coverage, task 6547 ported the irreplaceable cross-process cases
# onto `parity_harness.hpp`'s `run_pinned()`, and task 6436 re-pointed
# `scripts/coverage-check.sh` at the C++ corpus, so `make coverage` still
# grades leaf coverage.

.PHONY: cpp-lint
# ── C++26 rewrite lint gate (M0 boundary review, plan 996 task 6049 F3) ──
# Pinned toolchain per docs/toolchain-parity.md — always the pinned LLVM's
# own binaries, never a PATH-resolved clang-format/clang-tidy, which may
# belong to a different LLVM release and disagree under the same config.
#
# DISCOVERED, not hardcoded (plan 996, task 6054). These used to be the
# literal Homebrew-ARM-macOS paths `/opt/homebrew/opt/llvm/bin/clang-*`,
# which made `make cpp-lint` unrunnable on an Intel Mac
# (`/usr/local/opt/llvm`) or any Linux box, and was one of the two stated
# reasons cpp-lint is not composed into test-all. The resolution order
# below keeps the pin STRICTER than a PATH lookup rather than looser:
#
#   1. An explicit LLVM_PREFIX= / CLANG_FORMAT_BIN= / CLANG_TIDY_BIN= on
#      the command line or in the environment always wins.
#   2. Otherwise the prefix is read out of $(CPP_BUILD_DIR)'s CMakeCache —
#      i.e. the LLVM that ACTUALLY BUILT this tree, resolved from
#      CMAKE_CXX_COMPILER. This is the strongest available binding: the
#      formatter and the compiler cannot drift apart, which a second
#      independent lookup (even a correct one) does not guarantee.
#   3. Failing that (no build dir yet), `brew --prefix llvm`.
#
# There is deliberately NO fallback to a PATH-resolved clang-format: the
# recipe below fails loudly with the override to set, because silently
# linting with another release's formatter is exactly the outcome the pin
# exists to prevent. See docs/toolchain-parity.md § clang-format.
LLVM_PREFIX      ?= $(shell \
	cxx=$$(sed -n 's|^CMAKE_CXX_COMPILER:[^=]*=||p' $(CPP_BUILD_DIR)/CMakeCache.txt 2>/dev/null); \
	if [ -n "$$cxx" ]; then dirname "$$(dirname "$$cxx")"; \
	else brew --prefix llvm 2>/dev/null; fi)
CLANG_FORMAT_BIN ?= $(LLVM_PREFIX)/bin/clang-format
CLANG_TIDY_BIN   ?= $(LLVM_PREFIX)/bin/clang-tidy

# Fail with the fix rather than with "No such file or directory" from deep
# inside a pipeline.
define require_pinned_llvm
	@test -x "$(1)" || { \
	  echo "make cpp-lint: pinned LLVM tool not found at '$(1)'."; \
	  echo "  Resolved LLVM_PREFIX='$(LLVM_PREFIX)' (from $(CPP_BUILD_DIR)/CMakeCache.txt, else 'brew --prefix llvm')."; \
	  echo "  Install the pinned LLVM (docs/toolchain-parity.md), configure $(CPP_BUILD_DIR) first,"; \
	  echo "  or override explicitly:  make cpp-lint LLVM_PREFIX=/path/to/llvm"; \
	  exit 1; }
endef

# First-party C++ file list: everything under src/ and
# scripts/toolchain-probes/, excluding vendor/ and any build output —
# find-based so newly added files are picked up without editing this list.
CPP_FILES := $(shell find src scripts/toolchain-probes -type f \( -name '*.cppm' -o -name '*.cpp' \) \
	-not -path '*/vendor/*' -not -path '*/build/*' 2>/dev/null)

.PHONY: cpp-lint-gate
cpp-lint-gate: ## The GATING half of cpp-lint (clang-format --Werror + doxygen); composed into test-all
	# TASK 6054. `cpp-lint` has three steps and only TWO of them gate: a
	# nonzero exit from `clang-format --Werror` or from the Doxygen pass
	# (WARN_AS_ERROR=YES) stops the recipe. `clang-tidy` is advisory — no
	# --warnings-as-errors, no WarningsAsErrors key in .clang-tidy, 105
	# residual findings (task 6439) — so it exits 0 regardless and cannot
	# gate anything.
	#
	# That asymmetry is what let this composition stay deferred: the
	# objection recorded above `test-all` was clang-tidy's RUNTIME over the
	# whole module graph, and that it is advisory. Both are true of
	# clang-tidy and neither is true of the two steps that actually gate,
	# so this target runs those two and `test-all` takes it.
	#
	# Doxygen still needs the module BMIs materialized, same as clang-tidy
	# (task 6049 F3), but `test-all` reaches this only after `test` and
	# `cli-usage-check` have configured and built $(CPP_BUILD_DIR), so the
	# build below is a no-op in that path and a correctness guard when the
	# target is run alone.
	$(call require_pinned_llvm,$(CLANG_FORMAT_BIN))
	@echo "== cpp-lint-gate: clang-format --dry-run --Werror (pinned $(CLANG_FORMAT_BIN)) =="
	$(CLANG_FORMAT_BIN) --dry-run --Werror $(CPP_FILES)
	cmake --build $(CPP_BUILD_DIR)
	@echo "== cpp-lint-gate: doxygen Doxyfile.lint (retries only on signal death; see docs/toolchain-parity.md) =="
	# A HIGHER retry bound than interactive `cpp-lint` uses. doxygen 1.18.0
	# has a content-independent SIGBUS at a measured 25-50% per run (task
	# 6077/6315), so the default bound of 3 leaves a composed gate failing
	# somewhere between 1% and 12% of the time for no reason at all -- and a
	# gate that fails randomly is one people learn to ignore. Eight attempts
	# puts that under a percent.
	#
	# This cannot mask a finding: cpp-lint-doxygen.sh treats a genuine
	# WARN_FORMAT diagnostic as authoritative and never retries it, however
	# many attempts remain. Only a crash with NO diagnostic text is retried.
	DOXYGEN_MAX_ATTEMPTS=8 scripts/cpp-lint-doxygen.sh Doxyfile.lint

cpp-lint: ## Pinned clang-format + clang-tidy + doxygen gate over first-party C++ (docs/toolchain-parity.md)
	$(call require_pinned_llvm,$(CLANG_FORMAT_BIN))
	$(call require_pinned_llvm,$(CLANG_TIDY_BIN))
	@echo "== cpp-lint: clang-format --dry-run --Werror (pinned $(CLANG_FORMAT_BIN)) =="
	$(CLANG_FORMAT_BIN) --dry-run --Werror $(CPP_FILES)
	@echo "== cpp-lint: clang-tidy (pinned $(CLANG_TIDY_BIN); src/ tree only, see note below) =="
	# clang-tidy needs the module BMIs (*.pcm) already materialized under
	# $(CPP_BUILD_DIR) — unlike ninja's dyndep scan, clang-tidy does NOT run
	# its own P1689 module-dependency scan, so a source that `import`s a
	# module (`import std;`, `import planar.core;`) resolves only against an
	# already-built tree. Verified empirically (task 6049, F3): against a
	# freshly configured-but-unbuilt build dir, clang-tidy fails with
	# "module 'std' not found" / "no such file ... .modmap". `cmake --build`
	# here makes that a non-issue rather than an operator-order footgun.
	cmake --build $(CPP_BUILD_DIR)
	find src -type f \( -name '*.cppm' -o -name '*.cpp' \) -print0 | xargs -0 $(CLANG_TIDY_BIN) -p $(CPP_BUILD_DIR)
	@echo "== cpp-lint: doxygen Doxyfile.lint (retries only on signal death; see docs/toolchain-parity.md) =="
	scripts/cpp-lint-doxygen.sh Doxyfile.lint
	# NOTE — scripts/toolchain-probes/*.cpp are deliberately clang-format-
	# checked above but NOT clang-tidied: they are standalone toolchain-
	# verification probes compiled directly against the pinned clang++
	# (docs/toolchain-parity.md), not CMake targets, so they have no entry
	# in $(CPP_BUILD_DIR)/compile_commands.json. Without a compile-commands
	# entry clang-tidy falls back to synthetic default flags and misfires on
	# probe_embed.cpp's deliberate `-Wc23-extensions` case (the whole point
	# of that probe — see toolchain-parity.md's "#embed" section). Inventing
	# a fake compile command to force a pass would be lying about coverage;
	# this is the genuine boundary of what clang-tidy can evaluate for a
	# file outside the build graph.

.PHONY: test-cpp-report
test-cpp-report: ## Run the C++ ctest suite and REPORT its skip tally (a skipped case asserted nothing)
	scripts/ctest-report.sh --build-dir build/debug $(ARGS)

.PHONY: test-cpp-solver
test-cpp-solver: ## Run the ctest suite against a solver-ON build (decision 1032)
	cmake --preset debug -DPLANAR_WITH_MTKAHYPAR=ON
	cmake --build $(CPP_BUILD_DIR)
	ctest --test-dir $(CPP_BUILD_DIR) --output-on-failure $(ARGS)

# This lane builds WITH the solver (decision 1032): groups_recommend's
# task-4247 test hard-asserts optimal_available, and the developer default
# keeps PLANAR_WITH_MTKAHYPAR OFF so nobody builds the 330MB library. It
# inherited the solver-ON posture from the retired `test-parity-cpp` target,
# which was the only lane that configured it. The ON build is hermetic since
# task 6461 pinned CPM's cache key; verify with
# `make test-vendor-mtkahypar-offline`. Task 6536: a solver-ON build can
# produce binaries that pass every in-process test and still fail to launch
# (`Library not loaded: @rpath/libmtkahypar.dylib`), so the install smoke in
# `install.sh` — not this target — is what catches that class.

.PHONY: cli-usage-check
cli-usage-check: ## Validate authored surfaces against the live CLI schema and semantic contracts
	cmake --preset debug
	cmake --build $(CPP_BUILD_DIR) --target cli_usage_lint surface_lint planar_cmd_planar planar_cmd_planar_agent planar_cmd_planar_watch planar_cmd_planar_ext
	$(CLI_USAGE_LINT) $(CURDIR) $(CPP_BIN_ABS)/$(BINARY) $(CPP_BIN_ABS)/$(AGENT_BINARY) $(CPP_BIN_ABS)/$(WATCH_BINARY) $(CPP_BIN_ABS)/$(EXT_BINARY)
	$(SURFACE_LINT) $(CURDIR)

.PHONY: surface-lint
surface-lint: ## Validate authored links, contracts, capabilities, commands, and retired references
	cmake --preset debug
	cmake --build $(CPP_BUILD_DIR) --target surface_lint
	$(SURFACE_LINT) $(CURDIR)

.PHONY: surface-check
# Fast local gate for the CLI colocation refactor (task 6401/6612, decision
# 1068): diffs each binary's `schema` catalog, root --help, and every leaf's
# --help against scripts/surface-baseline.txt. The refactor moves 260
# declared paths between files and is supposed to change NOTHING a binary
# exposes; this fails in seconds when it does. Run it in the same recipe
# position as cli-usage-check — at every commit during the refactor, not
# just at the end.
#
# A DELIBERATE surface change (an intended new/removed/renamed leaf or flag)
# requires re-running `scripts/surface-snapshot.sh capture` in its OWN commit
# with the resulting baseline diff reviewed on purpose — never absorbed
# silently into an unrelated commit as a side effect of a passing gate.
surface-check: ## Diff each binary's live schema/help surface against scripts/surface-baseline.txt
	cmake --preset debug
	cmake --build $(CPP_BUILD_DIR) --target planar_cmd_planar planar_cmd_planar_agent planar_cmd_planar_watch planar_cmd_planar_ext
	scripts/surface-snapshot.sh verify

.PHONY: coverage
coverage: build ## Check integration-test leaf-coverage ratio against scripts/coverage-baseline.txt
	scripts/coverage-check.sh

.PHONY: coverage-report
coverage-report: build ## Print per-verb integration-test leaf coverage table
	scripts/coverage-check.sh --report

.PHONY: coverage-update
coverage-update: build ## Re-seed scripts/coverage-baseline.txt with the current coverage ratio
	scripts/coverage-check.sh --update

# The Go-archive parity gate was RETIRED (planar task 5623). The reference was
# a frozen archive whose last migration is 00030; the port moved past it, so
# the two binaries could no longer open the same database — and the audit's
# premise was that both operate on identical state. The integration suite is
# the standing guard, and the `parity_*` suites still assert the user-facing
# contracts the audit originally surfaced.
# cpp-lint is deliberately NOT composed into test-all (M0 boundary review,
# plan 996 task 6049 F14). Task 6054 removed ONE of the two original
# reasons: the hardcoded `/opt/homebrew/opt/llvm` paths are now discovered
# (see CLANG_FORMAT_BIN above), so the target is no longer
# Homebrew-ARM-macOS-only. The reason that REMAINS is the ordering
# precondition none of test-all's other members share: cpp-lint requires
# $(CPP_BUILD_DIR) already configured, and clang-tidy needs the module
# BMIs materialized (see the note in the cpp-lint target itself), so
# composing it in would make `make test-all` configure and build the
# entire C++ tree before the cheap gates run. That reason is now WEAKER
# than it was: since the M10 cutover (task 6045) the C++ tree is the only
# implementation, so `make test` builds it anyway and the precondition
# costs nothing at the point cpp-lint would run.
#
# RESOLVED AT TASK 6054 by splitting the recipe rather than composing all of
# it. `cpp-lint-gate` runs the two steps that actually GATE — clang-format
# --Werror and the Doxygen pass — and `test-all` takes that. Full `cpp-lint`
# stays an explicit target because clang-tidy is the slow half AND the
# advisory half (no --warnings-as-errors; 105 residual findings, task 6439),
# so composing it would add runtime while gating nothing.
#
# C++ format and doc-comment drift therefore no longer rides on operator
# discipline. Tidy drift still does, as does everything else, because this
# repo has no .github/workflows at all today.
.PHONY: test-all
test-all: test coverage cli-usage-check surface-check cpp-lint-gate ## Run the unit suite, coverage, the authored-surface gates, and the gating half of cpp-lint

# RE-POINTED AT clang-format (plan 996, task 6045). These ran `zig fmt` over
# `zig/`. With that tree deleted the formatter of record is the PINNED LLVM's
# clang-format, resolved exactly the way `cpp-lint` resolves it — see
# CLANG_FORMAT_BIN above and docs/toolchain-parity.md. `fmt-check` is the
# same check `cpp-lint` runs first; it exists separately so the cheap
# formatting gate can run without clang-tidy's build-and-BMI precondition.
.PHONY: fmt
fmt: ## Reformat first-party C++ in place with the pinned clang-format
	$(call require_pinned_llvm,$(CLANG_FORMAT_BIN))
	$(CLANG_FORMAT_BIN) -i $(CPP_FILES)

.PHONY: fmt-check
fmt-check: ## Verify clang-format is clean (no build required, unlike cpp-lint)
	$(call require_pinned_llvm,$(CLANG_FORMAT_BIN))
	$(CLANG_FORMAT_BIN) --dry-run --Werror $(CPP_FILES)

.PHONY: clean
clean: ## Remove build artifacts
	rm -rf $(BIN_DIR) build

.PHONY: docs-manifest
docs-manifest: ## Regenerate the docs/.manifest-docs Merkle index
	planar doc manifest --write

# ── Skill/agent evaluation suites (returned from armarium with the
#    orchestration layer). Deterministic lanes are provider-free; live lanes
#    invoke a model host and are opt-in.

.PHONY: eval
eval: eval-render eval-orchestrator-unit eval-orchestrator eval-orchestrator-fixtures ## Deterministic render, contract, negative-control, and lifecycle-fixture checks

.PHONY: eval-render
eval-render: ## Dry-run scriptorium render of skills/src + agents
	scriptorium render -config scriptorium.yaml -dry-run

.PHONY: eval-installed
eval-installed: ## Check installed projections match this checkout (run after ./install.sh)
	./scripts/check-self-installed.sh

.PHONY: eval-orchestrator
eval-orchestrator: ## Orchestrator contract evals
	./scripts/eval-orchestrator.sh --contract-only

.PHONY: eval-orchestrator-fast
eval-orchestrator-fast: eval-orchestrator-unit ## Provider-free contract lane (direct Python, no shell graders)
	PYTHONDONTWRITEBYTECODE=1 python3 \
		evals/orchestrator/harness.py --contract-only

.PHONY: eval-orchestrator-unit
eval-orchestrator-unit: ## Harness unit tests
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
		-s evals/orchestrator -p 'test_*.py'

.PHONY: eval-orchestrator-contract
eval-orchestrator-contract: ## Backward-compatible alias for the contract lane
	./scripts/eval-orchestrator.sh --contract-only

.PHONY: eval-orchestrator-fixtures
eval-orchestrator-fixtures: ## Lifecycle fixture replay without a model host
	./scripts/eval-orchestrator.sh --lifecycle-fixture-only

# Live host smoke. Override with VENDOR=claude SURFACE=agent as needed.
VENDOR ?= codex
SURFACE ?= skill
.PHONY: eval-orchestrator-live
eval-orchestrator-live: ## Live host smoke (opt-in; needs provider credentials)
	./scripts/eval-orchestrator.sh --live --vendor $(VENDOR) --surface $(SURFACE)

.PHONY: eval-orchestrator-lifecycle
eval-orchestrator-lifecycle: ## Full controlled lifecycle through a live orchestrator (opt-in)
	./scripts/eval-orchestrator.sh --lifecycle --vendor $(VENDOR) --surface agent

# Semantic evaluation lines for the planning surfaces (plan 948). Live by
# nature — NOT part of `make eval`. Use --dry-run to validate a case:
#   make eval-planning CASE=spec-draft-quality ARGS="--dry-run"
.PHONY: eval-planning
eval-planning: ## Live planning-surface eval: make eval-planning CASE=<case> [ARGS=...]
	@test -n "$(CASE)" || { echo "usage: make eval-planning CASE=<case> [ARGS=...]"; \
	  echo "available:"; ls evals/planning/cases/*.json | xargs -n1 basename | sed 's/\.json$$//;s/^/  /'; exit 2; }
	python3 evals/planning/harness.py --case $(CASE) $(ARGS)

.PHONY: eval-planning-unit
eval-planning-unit: ## Planning harness unit tests
	python3 evals/planning/test_harness.py

# Verifies every candidate in agents/models.md §Candidate Presets actually
# spawns on its host. DELIBERATELY NOT part of `make eval`: it costs one live
# invocation per candidate. Detection only — repair is an operator edit.
.PHONY: eval-candidate-spawn
eval-candidate-spawn: ## Verify every Tier Table candidate spawns (opt-in, live)
	python3 evals/candidate-spawn/verify.py $(ARGS)

# Offline rollout report for the adaptive routing plane (plan 949). Invokes NO
# model — reads Planar's own read verbs and reports class separation, cohort
# isolation, exclusions with reasons, rollback availability.
.PHONY: rollout-report
rollout-report: ## Offline adaptive-routing rollout report
	python3 evals/rollout/report.py $(ARGS)

.PHONY: rollout-report-unit
rollout-report-unit: ## Rollout report unit tests
	python3 evals/rollout/test_report.py
