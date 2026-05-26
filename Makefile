# Planar — Zig build/test entry points.
#
# Targets here are thin wrappers around the canonical `zig build` invocations
# declared in CLAUDE.md. They exist to give one consistent surface for humans
# and CI.
#
# The Zig package root IS the repo root: build.zig sits next to this Makefile,
# Zig modules live under src/, and tools/, integration_tests/, vendor/, and
# migrations/ are sibling top-level directories.

BINARY      := planar
BIN_DIR     := bin
BIN         := $(BIN_DIR)/$(BINARY)

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
build: ## Build the planar binary into ./bin/planar (at repo root)
	@mkdir -p $(BIN_DIR)
	$(ZIG) build -Doptimize=$(OPTIMIZE) $(ARGS)
	@cp -f zig-out/bin/$(BINARY) $(BIN)

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
test-integration: build ## Run the integration suite against the built binary
	PLANAR_BIN=$(CURDIR)/$(BIN) $(ZIG) build test-integration $(ARGS)

.PHONY: parity-check
parity-check: build ## Diff zig binary against Go archive binary (plan 351 Phase 5; skips when Go binary unreachable)
	scripts/parity-check.sh

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
test-all: test test-integration parity-check coverage ## Run unit + integration suites + parity-check gate + coverage ratchet

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
