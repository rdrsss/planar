.DEFAULT_GOAL := help

.PHONY: help install uninstall install-codex uninstall-codex install-claude uninstall-claude evals-setup evals-test evals-plan evals-run evals-evidence-test evals

CASE ?= fresh-interrupted-review
# Export values so shell expansion preserves literal paths.
# Capture command-line and environment values without evaluating Make syntax.
override AUTH_FILE := $(value AUTH_FILE)
override REVIEW_FILE := $(value REVIEW_FILE)
override EVALS_OUTPUT_ROOT := $(value EVALS_OUTPUT_ROOT)
override EVALS_PROJECT_DIR := $(value EVALS_PROJECT_DIR)
override CASE := $(value CASE)
override HOST := $(value HOST)
PLANAR_REPO_ROOT := $(CURDIR)
export PLANAR_REPO_ROOT EVALS_OUTPUT_ROOT EVALS_PROJECT_DIR CASE AUTH_FILE REVIEW_FILE HOST

help:
	@printf '%s\n' 'Planar developer commands:' '  install / uninstall HOST=codex|claude  Select one host' '  install-codex / uninstall-codex  Install/remove the local plugin in Codex' '  install-claude / uninstall-claude Install/remove the local plugin in Claude Code' '  evals-setup  Install locked evaluation dependencies' '  evals-test   Run adapter, case and experiment tests without a model' '  evals-evidence-test  Check explicitly selected retained evidence' '  evals-plan   Preflight CASE (default: fresh-interrupted-review)' '  evals-run    Execute reviewed CASE (requires AUTH_FILE and REVIEW_FILE)' '  evals        Run no-model evaluation tests'

install uninstall:
	@case "$$HOST" in codex|claude) $(MAKE) "$@-$$HOST" ;; *) printf '%s\n' 'HOST must be codex or claude' >&2; exit 2 ;; esac

install-codex:
	codex plugin marketplace add "$$PLANAR_REPO_ROOT"
	codex plugin add planar@planar
uninstall-codex:
	codex plugin remove planar@planar
install-claude:
	claude plugin marketplace add "$$PLANAR_REPO_ROOT"
	claude plugin install planar@planar
uninstall-claude:
	claude plugin uninstall planar@planar --keep-data

evals-setup:
	uv sync --project evals --locked
evals-test:
	uv run --project evals --locked python evals/planar/adapters/eval_test_suite.py portable evals/planar/adapters
	uv run --project evals --locked python evals/planar/adapters/eval_test_suite.py portable evals/planar/cases/fresh-interrupted-review
	PYTHONPATH=evals/planar/adapters uv run --project evals --locked python evals/planar/adapters/eval_test_suite.py portable evals/planar/cases/clean-revision
	PYTHONPATH=evals/planar/adapters:evals/planar/cases/clean-work-review:evals/planar/cases/clean-work-review/tests uv run --project evals --locked python evals/planar/adapters/eval_test_suite.py portable evals/planar/cases/clean-work-review
	PYTHONPATH=evals/planar/cases/retained-a-fresh-b:evals/planar/cases/retained-a-fresh-b/tests:evals/planar/cases/clean-revision:evals/planar/cases/clean-revision/tests uv run --project evals --locked python evals/planar/adapters/eval_test_suite.py portable evals/planar/cases/retained-a-fresh-b
	uv run --project evals --locked python evals/planar/adapters/eval_test_suite.py portable evals/planar/experiments
evals-evidence-test:
	@test -n "$$EVALS_PROJECT_DIR" || { printf '%s\n' 'EVALS_PROJECT_DIR is required for retained evidence' >&2; exit 2; }
	PLANAR_EVALS_PROJECT_DIR="$$EVALS_PROJECT_DIR" uv run --project evals --locked python evals/planar/adapters/eval_test_suite.py evidence evals/planar/adapters
	PLANAR_EVALS_PROJECT_DIR="$$EVALS_PROJECT_DIR" PYTHONPATH=evals/planar/cases/retained-a-fresh-b:evals/planar/cases/retained-a-fresh-b/tests:evals/planar/cases/clean-revision:evals/planar/cases/clean-revision/tests uv run --project evals --locked python evals/planar/adapters/eval_test_suite.py evidence evals/planar/cases/retained-a-fresh-b
	PLANAR_EVALS_PROJECT_DIR="$$EVALS_PROJECT_DIR" uv run --project evals --locked python evals/planar/adapters/eval_test_suite.py evidence evals/planar/experiments
evals-plan:
	PLANAR_EVALS_PROJECT_DIR="$$EVALS_PROJECT_DIR" PLANAR_EVALS_OUTPUT_ROOT="$$EVALS_OUTPUT_ROOT" uv run --project evals --locked python evals/planar/adapters/launch.py --case "$$CASE"
evals-run:
	@test -n "$$AUTH_FILE" && test -n "$$REVIEW_FILE" || { printf '%s\n' 'AUTH_FILE and REVIEW_FILE are required' >&2; exit 2; }
	@PLANAR_EVALS_PROJECT_DIR="$$EVALS_PROJECT_DIR" PLANAR_EVALS_OUTPUT_ROOT="$$EVALS_OUTPUT_ROOT" uv run --project evals --locked python evals/planar/adapters/launch.py --execute --case "$$CASE" --auth-file "$$AUTH_FILE" --review-file "$$REVIEW_FILE"
evals: evals-test
