.DEFAULT_GOAL := help

.PHONY: help install uninstall install-codex uninstall-codex install-claude uninstall-claude evals-setup evals-test evals-plan evals-run evals

CASE ?= fresh-interrupted-review
# Export values so shell expansion preserves literal paths.
ifndef EVALS_OUTPUT_ROOT
EVALS_OUTPUT_ROOT := $(CURDIR)/evals/results
endif
# Capture command-line and environment values without evaluating Make syntax.
override AUTH_FILE := $(value AUTH_FILE)
override REVIEW_FILE := $(value REVIEW_FILE)
override EVALS_OUTPUT_ROOT := $(value EVALS_OUTPUT_ROOT)
override CASE := $(value CASE)
override HOST := $(value HOST)
PLANAR_REPO_ROOT := $(CURDIR)
export PLANAR_REPO_ROOT EVALS_OUTPUT_ROOT CASE AUTH_FILE REVIEW_FILE HOST

help:
	@printf '%s\n' 'Planar developer commands:' '  install / uninstall HOST=codex|claude  Select one host' '  install-codex / uninstall-codex  Install/remove the local plugin in Codex' '  install-claude / uninstall-claude Install/remove the local plugin in Claude Code' '  evals-setup  Install locked evaluation dependencies' '  evals-test   Run adapter, case and experiment tests without a model' '  evals-plan   Preflight CASE (default: fresh-interrupted-review)' '  evals-run    Execute reviewed CASE (requires AUTH_FILE and REVIEW_FILE)' '  evals        Run no-model evaluation tests'

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
	uv run --project evals --locked python -m unittest discover -s evals/planar/adapters -p 'test_*.py' -v
	uv run --project evals --locked python -m unittest discover -s evals/planar/cases/fresh-interrupted-review -p 'test_*.py' -v
	PYTHONPATH=evals/planar/adapters uv run --project evals --locked python -m unittest discover -s evals/planar/cases/clean-revision -p 'test_*.py' -v
	uv run --project evals --locked python -m unittest discover -s evals/planar/experiments -p 'test_*.py' -v
evals-plan:
	PLANAR_EVALS_OUTPUT_ROOT="$$EVALS_OUTPUT_ROOT" uv run --project evals --locked python evals/planar/adapters/launch.py --case "$$CASE"
evals-run:
	@test -n "$$AUTH_FILE" && test -n "$$REVIEW_FILE" || { printf '%s\n' 'AUTH_FILE and REVIEW_FILE are required' >&2; exit 2; }
	@PLANAR_EVALS_OUTPUT_ROOT="$$EVALS_OUTPUT_ROOT" uv run --project evals --locked python evals/planar/adapters/launch.py --execute --case "$$CASE" --auth-file "$$AUTH_FILE" --review-file "$$REVIEW_FILE"
evals: evals-test
