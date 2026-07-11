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
DOC_BINARY    := planar-doc
EXECUTE_BINARY := planar-execute
BIN_DIR       := bin
BIN           := $(BIN_DIR)/$(BINARY)
AGENT_BIN     := $(BIN_DIR)/$(AGENT_BINARY)
WATCH_BIN     := $(BIN_DIR)/$(WATCH_BINARY)
DOC_BIN       := $(BIN_DIR)/$(DOC_BINARY)
EXECUTE_BIN   := $(BIN_DIR)/$(EXECUTE_BINARY)

ZIG         ?= zig

# Default optimize mode for production builds. Override via:
#   make build OPTIMIZE=Debug
#   make build OPTIMIZE=ReleaseFast
OPTIMIZE    ?= ReleaseSafe

# Extra args forwarded to underlying zig invocations.
ARGS        ?=

.DEFAULT_GOAL := help

.PHONY: help
help:
	@awk 'BEGIN {FS = ":.*##"; printf "Targets:\n"} /^[a-zA-Z0-9_.-]+:.*##/ {printf "  \033[36m%-22s\033[0m %s\n", $$1, $$2}' $(MAKEFILE_LIST)

.PHONY: build
build: ## Build the planar + planar-agent + planar-watch + planar-doc + planar-execute binaries into ./bin/ (at repo root)
	@mkdir -p $(BIN_DIR)
	$(ZIG) build -Doptimize=$(OPTIMIZE) $(ARGS)
	@cp -f zig-out/bin/$(BINARY) $(BIN)
	@cp -f zig-out/bin/$(AGENT_BINARY) $(AGENT_BIN)
	@cp -f zig-out/bin/$(WATCH_BINARY) $(WATCH_BIN)
	@cp -f zig-out/bin/$(DOC_BINARY) $(DOC_BIN)
	@cp -f zig-out/bin/$(EXECUTE_BINARY) $(EXECUTE_BIN)

.PHONY: install
install: ## Full install via ./install.sh — pass extra flags as INSTALL_FLAGS="..."
	./install.sh $(INSTALL_FLAGS)

.PHONY: install-bin
install-bin: ## Install just the binary into ~/.planar/bin/planar (skips vendor surface staging)
	@mkdir -p $(HOME)/.planar/bin
	$(ZIG) build -Doptimize=$(OPTIMIZE) --prefix $(HOME)/.planar $(ARGS)

.PHONY: uninstall
uninstall: ## Uninstall via ./install.sh --uninstall (preserves ~/.planar/planar.db)
	./install.sh --uninstall $(INSTALL_FLAGS)

.PHONY: run
run: ## Run the CLI from source: make run ARGS="task list"
	$(ZIG) build run -- $(ARGS)

.PHONY: test
test: ## Run unit tests
	$(ZIG) build test $(ARGS)

.PHONY: test-integration
# -Dtest-binary=true: rebuild the binary with the test-binary flag enabled.
# This activates the PLANAR_DISABLE_WORKTREE_GATE env-var bypass in the
# worktree gate (plan 297 t#2937). The production binary (make build) is
# compiled without this flag and ignores the env var entirely.
test-integration: build ## Run the integration suite against the built binary
	PLANAR_BIN=$(CURDIR)/$(BIN) PLANAR_AGENT_BIN=$(CURDIR)/$(AGENT_BIN) PLANAR_WATCH_BIN=$(CURDIR)/$(WATCH_BIN) PLANAR_DOC_BIN=$(CURDIR)/$(DOC_BIN) PLANAR_EXECUTE_BIN=$(CURDIR)/$(EXECUTE_BIN) $(ZIG) build test-integration -Dtest-binary=true $(ARGS)

.PHONY: test-integration-files
test-integration-files: build ## Run integration tests as one executable per test file
	PLANAR_BIN=$(CURDIR)/$(BIN) PLANAR_AGENT_BIN=$(CURDIR)/$(AGENT_BIN) PLANAR_WATCH_BIN=$(CURDIR)/$(WATCH_BIN) PLANAR_DOC_BIN=$(CURDIR)/$(DOC_BIN) PLANAR_EXECUTE_BIN=$(CURDIR)/$(EXECUTE_BIN) $(ZIG) build test-integration-files -Dtest-binary=true $(ARGS)

.PHONY: parity-check
parity-check: build ## Diff zig binary against Go archive binary (plan 351 Phase 5; skips when Go binary unreachable)
	scripts/parity-check.sh

.PHONY: cli-usage-check
cli-usage-check: ## Validate authored CLI invocations (agents/, skills/src/, docs/) against the live command schema
	$(ZIG) build cli-usage-check

.PHONY: surface-lint
surface-lint: ## Validate authored links, contracts, capabilities, commands, and retired references
	$(ZIG) build surface-lint

.PHONY: bench-verify
bench-verify: build ## planar-doc verify latency tracker — prints cold + warm wall-clock
	@echo "planar-doc verify: cold + warm wall-clock (rough; integration tests own the latency contract)"
	@$(DOC_BIN) build > /dev/null 2>&1 || true
	@start_cold=$$(date +%s%N); \
	  $(DOC_BIN) verify > /dev/null 2>&1 || true; \
	  end_cold=$$(date +%s%N); \
	  cold_ms=$$(( (end_cold - start_cold) / 1000000 )); \
	  start_warm=$$(date +%s%N); \
	  $(DOC_BIN) verify > /dev/null 2>&1 || true; \
	  end_warm=$$(date +%s%N); \
	  warm_ms=$$(( (end_warm - start_warm) / 1000000 )); \
	  echo "verify: $${warm_ms}ms (warm) / $${cold_ms}ms (cold)"

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
test-all: test test-integration parity-check coverage cli-usage-check ## Run unit + integration suites + parity-check gate + coverage ratchet + CLI-usage lint

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
