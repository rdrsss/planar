# Planar — Zig build/test entry points.
#
# Targets here are thin wrappers around the canonical `zig build` invocations
# declared in CLAUDE.md. They exist to give one consistent surface for humans
# and CI.
#
# The Zig implementation relocated to zig/ at M0 (plan 996, task
# cpp-zig-relocation): build.zig sits at zig/build.zig, Zig modules live
# under zig/src/, and zig/tools/, zig/integration_tests/, zig/vendor/ are
# sibling directories under zig/. migrations/, templates/, skills/, agents/,
# evals/, docs/, scripts/ stay at the repo root (language-agnostic) — the
# relocated build.zig reaches them via relative (`../`) paths. This Makefile
# stays the operator entry point at the repo root; its targets invoke zig
# build against zig/build.zig via --build-file with an ABSOLUTE path rather
# than `cd`-ing into zig/, so the spawned process's cwd (and therefore every
# integration test's repo-root-relative file access, e.g. `skills/src/...`,
# `docs/...`, `migrations/...`) stays at the repo root exactly as it did
# before the relocation. Only `zig build`'s own path resolution (b.path())
# is relative to zig/ (the build root), which is why build.zig's migrations/
# templates/metrics references were rewritten with a leading `../`.

BINARY        := planar
AGENT_BINARY  := planar-agent
WATCH_BINARY  := planar-watch
EXECUTE_BINARY := planar-execute
BIN_DIR       := bin
BIN           := $(BIN_DIR)/$(BINARY)
AGENT_BIN     := $(BIN_DIR)/$(AGENT_BINARY)
WATCH_BIN     := $(BIN_DIR)/$(WATCH_BINARY)
EXECUTE_BIN   := $(BIN_DIR)/$(EXECUTE_BINARY)

ZIG         ?= zig
PREFIX      ?= $(HOME)/.local

# zig/ tree — see the relocation note above. ZIGBUILD invokes the relocated
# build.zig by absolute path so every recipe below can keep running from the
# repo root (Makefile's own invocation directory) without a `cd`.
ZIG_DIR       := $(CURDIR)/zig
ZIG_BUILD_FILE := $(ZIG_DIR)/build.zig
ZIGBUILD      := $(ZIG) build --build-file $(ZIG_BUILD_FILE)

# CMake parity build output — the C++ tree lands binaries here per the
# tech-spec's CMakePresets (`build/<preset>`). Override to point the parity
# lane at a different build directory.
CPP_BIN_DIR   ?= build/debug/bin
CPP_BIN_ABS   := $(abspath $(CPP_BIN_DIR))

# Default optimize mode for production builds. Override via:
#   make build OPTIMIZE=Debug
#   make build OPTIMIZE=ReleaseFast
OPTIMIZE    ?= ReleaseSafe

# Extra args forwarded to underlying zig invocations.
ARGS        ?=

# Share one Zig local cache between the main checkout and every git worktree
# of this repo, so worktree builds (agent dispatch, cycle branches) start warm
# instead of recompiling the vendored C deps and the full module graph from
# scratch. Zig's cache is lock-protected; concurrent builds are safe. Resolves
# to the main checkout's .zig-cache — in the main checkout itself this is
# identical to the default. Outside a git checkout the variable is left unset
# and zig falls back to ./.zig-cache.
GIT_COMMON_DIR := $(shell git rev-parse --path-format=absolute --git-common-dir 2>/dev/null)
ifneq ($(GIT_COMMON_DIR),)
export ZIG_LOCAL_CACHE_DIR ?= $(patsubst %/.git,%,$(GIT_COMMON_DIR))/.zig-cache
endif

.DEFAULT_GOAL := help

.PHONY: help
help:
	@awk 'BEGIN {FS = ":.*##"; printf "Targets:\n"} /^[a-zA-Z0-9_.-]+:.*##/ {printf "  \033[36m%-22s\033[0m %s\n", $$1, $$2}' $(MAKEFILE_LIST)

.PHONY: build
build: ## Build the planar + planar-agent + planar-watch + planar-execute binaries into ./bin/ (at repo root)
	@mkdir -p $(BIN_DIR)
	$(ZIGBUILD) -Doptimize=$(OPTIMIZE) $(ARGS)
	@cp -f $(ZIG_DIR)/zig-out/bin/$(BINARY) $(BIN)
	@cp -f $(ZIG_DIR)/zig-out/bin/$(AGENT_BINARY) $(AGENT_BIN)
	@cp -f $(ZIG_DIR)/zig-out/bin/$(WATCH_BINARY) $(WATCH_BIN)
	@cp -f $(ZIG_DIR)/zig-out/bin/$(EXECUTE_BINARY) $(EXECUTE_BIN)

.PHONY: install
install: ## Build and install the four Planar executables into PREFIX/bin (default: ~/.local/bin)
	$(ZIGBUILD) -Doptimize=$(OPTIMIZE) -Dversion-meta=true --prefix $(PREFIX) $(ARGS)

.PHONY: install-bin
install-bin: install ## Compatibility alias for the binary-only install

.PHONY: install-full
install-full: ## Legacy full install: binaries plus skills, agents, workflows, and vendor wiring
	./install.sh $(INSTALL_FLAGS)

.PHONY: uninstall
uninstall: ## Remove the four Planar executables from PREFIX/bin
	rm -f $(PREFIX)/bin/$(BINARY)
	rm -f $(PREFIX)/bin/$(AGENT_BINARY)
	rm -f $(PREFIX)/bin/$(WATCH_BINARY)
	rm -f $(PREFIX)/bin/$(EXECUTE_BINARY)

.PHONY: uninstall-full
uninstall-full: ## Remove the legacy full install (preserves ~/.planar/planar.db)
	./install.sh --uninstall $(INSTALL_FLAGS)

# Scratch database for hand-run smoke tests, kept in the build dir.
#
# A from-source binary resolves $PLANAR_DB, falling back to the operator's real
# ~/.planar/planar.db — and it applies its pending migrations AUTOMATICALLY on
# first use. So one bare `zig build run` from a branch carrying a new migration
# silently pushes the live database past every installed binary's supported
# version and breaks every other agent on the machine. Always smoke against
# this instead.
SMOKE_DB ?= $(ZIG_DIR)/.zig-cache/smoke/planar.db

.PHONY: run
run: ## Run the CLI from source AGAINST THE REAL DB (use `make smoke` for a throwaway one)
	$(ZIGBUILD) run -- $(ARGS)

.PHONY: smoke
smoke: ## Run the CLI from source against a throwaway build-dir DB: make smoke ARGS="task list"
	@mkdir -p $(dir $(SMOKE_DB))
	PLANAR_DB=$(SMOKE_DB) $(ZIGBUILD) run -- $(ARGS)

.PHONY: smoke-reset
smoke-reset: ## Delete the throwaway smoke database
	rm -rf $(dir $(SMOKE_DB))

.PHONY: test-install-manifest
test-install-manifest: ## Run focused installer manifest ownership/atomicity fixtures
	bash scripts/install-manifest-test.sh

.PHONY: test
test: test-install-manifest ## Run unit tests
	$(ZIGBUILD) test $(ARGS)

.PHONY: test-integration
# -Dtest-binary=true: rebuild the binary with the test-binary flag enabled.
# This activates the PLANAR_DISABLE_WORKTREE_GATE env-var bypass in the
# worktree gate (plan 297 t#2937). The production binary (make build) is
# compiled without this flag and ignores the env var entirely.
# No `build` prerequisite: build.zig points the harness (PLANAR_BIN etc.) at
# the Debug -Dtest-binary=true binaries it installs into zig-out itself, so a
# ReleaseSafe ./bin pre-build would be dead weight the suite never executes.
test-integration: ## Run the integration suite (builds its own Debug test binaries)
	$(ZIGBUILD) test-integration -Dtest-binary=true $(ARGS)

.PHONY: test-integration-files
test-integration-files: ## Run integration tests as one executable per test file
	$(ZIGBUILD) test-integration-files -Dtest-binary=true $(ARGS)

.PHONY: cpp-lint
# ── C++26 rewrite lint gate (M0 boundary review, plan 996 task 6049 F3) ──
# Pinned toolchain per docs/toolchain-parity.md — always the pinned LLVM's
# own binaries, never a PATH-resolved clang-format/clang-tidy, which may
# belong to a different LLVM release and disagree under the same config.
CLANG_FORMAT_BIN := /opt/homebrew/opt/llvm/bin/clang-format
CLANG_TIDY_BIN   := /opt/homebrew/opt/llvm/bin/clang-tidy
CPP_BUILD_DIR    ?= build/debug

# First-party C++ file list: everything under src/ and
# scripts/toolchain-probes/, excluding vendor/, zig/, and any build output —
# find-based so newly added files are picked up without editing this list.
CPP_FILES := $(shell find src scripts/toolchain-probes -type f \( -name '*.cppm' -o -name '*.cpp' \) \
	-not -path '*/vendor/*' -not -path '*/zig/*' -not -path '*/build/*' 2>/dev/null)

cpp-lint: ## Pinned clang-format + clang-tidy + doxygen gate over first-party C++ (docs/toolchain-parity.md)
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

.PHONY: test-parity-cpp
test-parity-cpp: ## Run the zig-side integration suite against CPP_BIN_DIR binaries (parity lane; fails until C++ binaries exist)
	PLANAR_BIN=$(CPP_BIN_ABS)/$(BINARY) \
	PLANAR_AGENT_BIN=$(CPP_BIN_ABS)/$(AGENT_BINARY) \
	PLANAR_WATCH_BIN=$(CPP_BIN_ABS)/$(WATCH_BINARY) \
	PLANAR_EXECUTE_BIN=$(CPP_BIN_ABS)/$(EXECUTE_BINARY) \
	$(ZIGBUILD) test-integration -Dtest-binary=true $(ARGS)

.PHONY: cli-usage-check
cli-usage-check: ## Validate authored surfaces against the live CLI schema and semantic contracts
	$(ZIGBUILD) cli-usage-check

.PHONY: surface-lint
surface-lint: ## Validate authored links, contracts, capabilities, commands, and retired references
	$(ZIGBUILD) surface-lint

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
# plan 996 task 6049 F14): it hardcodes a pinned LLVM path
# ($(CLANG_FORMAT_BIN)/$(CLANG_TIDY_BIN) above, docs/toolchain-parity.md)
# that only exists on a machine that has installed that exact toolchain,
# and it requires $(CPP_BUILD_DIR) already configured+built (clang-tidy
# needs materialized module BMIs, see the note in the cpp-lint target
# itself) — an ordering precondition test-all's other members don't share.
# Wiring it in would make test-all fail-by-default on any machine without
# the pinned LLVM at that path, or silently skip real lint coverage on a
# CI box that has a different clang-format on PATH. Run `make cpp-lint`
# explicitly once the C++ tree is the sole implementation (post-M9,
# D13) and toolchain provisioning is part of the standard dev/CI image;
# revisit composing it into test-all at that point.
.PHONY: test-all
test-all: test test-integration coverage cli-usage-check ## Run unit + integration suites, coverage, and composed authored-surface gates

.PHONY: fmt
fmt: ## Run zig fmt on the source tree
	$(ZIG) fmt $(ZIG_DIR)/build.zig $(ZIG_DIR)/src $(ZIG_DIR)/tools $(ZIG_DIR)/integration_tests

.PHONY: fmt-check
fmt-check: ## Verify zig fmt is clean (CI gate)
	$(ZIG) fmt --check $(ZIG_DIR)/build.zig $(ZIG_DIR)/src $(ZIG_DIR)/tools $(ZIG_DIR)/integration_tests

.PHONY: clean
clean: ## Remove build artifacts
	rm -rf $(BIN_DIR) $(ZIG_DIR)/zig-out $(ZIG_DIR)/.zig-cache $(ZIG_DIR)/zig-cache

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
