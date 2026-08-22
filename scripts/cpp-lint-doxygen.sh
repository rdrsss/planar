#!/usr/bin/env bash
# cpp-lint-doxygen.sh — run `doxygen <doxyfile>` with a bounded retry that
# ONLY covers the process dying from a signal (SIGBUS et al.), never a
# genuine lint failure.
#
# Background (plan 996, task 6077): doxygen 1.18.0 has a nondeterministic
# upstream defect that kills the process with SIGBUS on an idle machine,
# unrelated to load, config, or content — measured at roughly a 25-50%
# crash rate against an unchanged tree (docs/toolchain-parity.md). Retrying
# blindly on ANY non-zero exit would also swallow real lint failures:
# Doxyfile.lint sets WARN_AS_ERROR=YES, so an undocumented exported entity
# yields exit 1. That must fail on the first attempt, every time, with no
# retry and no masking.
#
# Distinction: a shell-terminated-by-signal exit status is 128+signal, so
# any exit code >= 128 means the process died from a signal (crash), not a
# doxygen-reported lint violation. Exit 1 (WARN_AS_ERROR) and any other
# exit in [1,127] is a genuine finding and propagates immediately.
set -u

doxyfile="${1:?usage: cpp-lint-doxygen.sh <Doxyfile>}"

# Bounded at 3 attempts (1 initial + 2 retries): the measured crash rate is
# ~25-50% per run and crashes are treated as independent (upstream memory-
# corruption bug, not state carried across runs), so the odds of 3
# consecutive crashes at a 50% rate are ~12.5%, and worse-case correlated
# runs still make a persistent doxygen breakage visible via the retry
# messages below rather than infinite-looping silently.
max_attempts=3
attempt=1

while :; do
	doxygen "$doxyfile"
	status=$?

	if [ "$status" -eq 0 ]; then
		exit 0
	fi

	if [ "$status" -lt 128 ]; then
		# Genuine doxygen-reported exit (e.g. WARN_AS_ERROR=YES lint
		# failure = exit 1). Never retried, never masked.
		exit "$status"
	fi

	# exit >= 128 => terminated by signal (128 + signal number), e.g.
	# 138 = 128 + SIGBUS. This is the known upstream flake.
	if [ "$attempt" -ge "$max_attempts" ]; then
		echo "cpp-lint-doxygen: doxygen died from a signal (exit $status) on attempt $attempt/$max_attempts; giving up" >&2
		exit "$status"
	fi

	echo "cpp-lint-doxygen: doxygen died from a signal (exit $status) on attempt $attempt/$max_attempts; retrying (known upstream doxygen 1.18.0 SIGBUS flake, see docs/toolchain-parity.md)" >&2
	attempt=$((attempt + 1))
done
