# Planar — Zig build/test entry points.
#
# Targets here are thin wrappers around the canonical `zig build` invocations
# declared in CLAUDE.md. They exist to give one consistent surface for humans
# and CI.
#
# The Zig package root IS the repo root: build.zig sits next to this Makefile,
# Zig modules live under src/, and tools/, integration_tests/, vendor/, and
# migrations/ are sibling top-level directories.

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
	$(ZIG) build -Doptimize=$(OPTIMIZE) $(ARGS)
	@cp -f zig-out/bin/$(BINARY) $(BIN)
	@cp -f zig-out/bin/$(AGENT_BINARY) $(AGENT_BIN)
	@cp -f zig-out/bin/$(WATCH_BINARY) $(WATCH_BIN)
	@cp -f zig-out/bin/$(EXECUTE_BINARY) $(EXECUTE_BIN)

.PHONY: install
install: ## Build and install the four Planar executables into PREFIX/bin (default: ~/.local/bin)
	$(ZIG) build -Doptimize=$(OPTIMIZE) -Dversion-meta=true --prefix $(PREFIX) $(ARGS)

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

.PHONY: run
run: ## Run the CLI from source: make run ARGS="task list"
	$(ZIG) build run -- $(ARGS)

.PHONY: test-install-manifest
test-install-manifest: ## Run focused installer manifest ownership/atomicity fixtures
	bash scripts/install-manifest-test.sh

.PHONY: test
test: test-install-manifest ## Run unit tests
	$(ZIG) build test $(ARGS)

.PHONY: test-integration
# -Dtest-binary=true: rebuild the binary with the test-binary flag enabled.
# This activates the PLANAR_DISABLE_WORKTREE_GATE env-var bypass in the
# worktree gate (plan 297 t#2937). The production binary (make build) is
# compiled without this flag and ignores the env var entirely.
# No `build` prerequisite: build.zig points the harness (PLANAR_BIN etc.) at
# the Debug -Dtest-binary=true binaries it installs into zig-out itself, so a
# ReleaseSafe ./bin pre-build would be dead weight the suite never executes.
test-integration: ## Run the integration suite (builds its own Debug test binaries)
	$(ZIG) build test-integration -Dtest-binary=true $(ARGS)

.PHONY: test-integration-files
test-integration-files: ## Run integration tests as one executable per test file
	$(ZIG) build test-integration-files -Dtest-binary=true $(ARGS)

.PHONY: parity-check
parity-check: build ## Diff zig binary against Go archive binary (plan 351 Phase 5; skips when Go binary unreachable)
	scripts/parity-check.sh

.PHONY: cli-usage-check
cli-usage-check: ## Validate authored surfaces against the live CLI schema and semantic contracts
	$(ZIG) build cli-usage-check

.PHONY: surface-lint
surface-lint: ## Validate authored links, contracts, capabilities, commands, and retired references
	$(ZIG) build surface-lint

.PHONY: coverage
coverage: build ## Check integration-test leaf-coverage ratio against scripts/coverage-baseline.txt
	scripts/coverage-check.sh

.PHONY: coverage-report
coverage-report: build ## Print per-verb integration-test leaf coverage table
	scripts/coverage-check.sh --report

.PHONY: coverage-update
coverage-update: build ## Re-seed scripts/coverage-baseline.txt with the current coverage ratio
	scripts/coverage-check.sh --update

.PHONY: test-all
test-all: test test-integration parity-check coverage cli-usage-check ## Run unit + integration suites + parity, coverage, and composed authored-surface gates

.PHONY: fmt
fmt: ## Run zig fmt on the source tree
	$(ZIG) fmt build.zig src tools integration_tests

.PHONY: fmt-check
fmt-check: ## Verify zig fmt is clean (CI gate)
	$(ZIG) fmt --check build.zig src tools integration_tests

.PHONY: clean
clean: ## Remove build artifacts
	rm -rf $(BIN_DIR) zig-out .zig-cache zig-cache

.PHONY: docs-manifest
docs-manifest: ## Regenerate the docs/.manifest-docs Merkle index
	planar doc manifest --write
