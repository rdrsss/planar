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
# task 6315: exit status ALONE is not a reliable discriminator. Doxyfile.lint
# sets WARN_AS_ERROR=YES, whose own documented behavior is "immediately stop
# when a warning is encountered" — and on this 1.18.0 build that stop can
# itself manifest as a signal death (the same >=128 family as the SIGBUS
# flake) instead of a clean exit 1, AFTER the diagnostic line has already
# been written to stderr per WARN_FORMAT. Two real findings were absorbed by
# the retry loop this way (task 6303, task 6330) even though the exit-code
# split below is exactly what the original design intended. So: capture
# every attempt's combined output and scan it for a genuine WARN_FORMAT
# diagnostic line ("$file:$line: $text", see Doxyfile.lint) BEFORE looking at
# the exit code at all. A diagnostic in the output is authoritative and never
# retried, regardless of how the process then exited. Only a crash that
# produced no diagnostic text is treated as the known content-independent
# SIGBUS flake and gets a retry.
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

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/cpp-lint-doxygen.XXXXXX") || exit 1
trap 'rm -rf "$tmpdir"' EXIT

# Matches Doxyfile.lint's WARN_FORMAT = "$file:$line: $text" — a path,
# a colon, a line number, a colon-space. QUIET=YES means this is the only
# kind of line doxygen ever writes on a clean invocation, so any match is a
# genuine diagnostic, never incidental output.
diagnostic_pattern='^[^[:space:]]+:[0-9]+: '

while :; do
	outfile="$tmpdir/attempt-$attempt.out"
	doxygen "$doxyfile" >"$outfile" 2>&1
	status=$?

	if grep -qE "$diagnostic_pattern" "$outfile"; then
		# A real WARN_FORMAT diagnostic was captured on this attempt. This
		# is authoritative regardless of how the process then exited —
		# including a signal death (task 6315: WARN_AS_ERROR's "stop
		# immediately" can itself crash instead of cleanly exit(1) on this
		# doxygen build). Never retried, never masked.
		cat "$outfile" >&2
		if [ "$status" -ge 128 ]; then
			echo "cpp-lint-doxygen: doxygen printed the diagnostic above and then died from a signal (exit $status) on attempt $attempt/$max_attempts; the diagnostic is authoritative — failing immediately with no retry" >&2
		fi
		exit 1
	fi

	if [ "$status" -eq 0 ]; then
		if [ "$attempt" -gt 1 ]; then
			echo "cpp-lint-doxygen: doxygen succeeded on attempt $attempt/$max_attempts; attempt(s) 1-$((attempt - 1)) died from a signal with no diagnostic in their output (known upstream doxygen 1.18.0 SIGBUS flake)" >&2
		fi
		exit 0
	fi

	if [ "$status" -lt 128 ]; then
		# Genuine non-zero, non-signal exit with no WARN_FORMAT line
		# matched above (e.g. a config or invocation error). Never
		# retried, never masked.
		cat "$outfile" >&2
		exit "$status"
	fi

	# exit >= 128 (terminated by signal, 128 + signal number, e.g. 138 =
	# 128 + SIGBUS) AND no diagnostic text captured in this attempt's
	# output: the known content-independent upstream flake.
	if [ "$attempt" -ge "$max_attempts" ]; then
		echo "cpp-lint-doxygen: doxygen died from a signal (exit $status) on attempt $attempt/$max_attempts with no diagnostic captured on any attempt; giving up" >&2
		exit "$status"
	fi

	echo "cpp-lint-doxygen: doxygen died from a signal (exit $status) on attempt $attempt/$max_attempts with no diagnostic in its output; retrying (known upstream doxygen 1.18.0 SIGBUS flake, see docs/toolchain-parity.md)" >&2
	attempt=$((attempt + 1))
done
