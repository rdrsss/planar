"""textkit: a small, dependency-free text utility used by this fixture's
own test suite. Flat module layout (no src/), on purpose -- this repository
exists to exercise the Planar lifecycle evaluator against a fixture whose
conventions deliberately differ from Planar's own.
"""

from __future__ import annotations


def dedupe_words(text: str) -> str:
    """Collapse consecutive duplicate words (case-sensitive) to one.

    "a a b b b c" -> "a b c"
    """
    words = text.split()
    result: list[str] = []
    for index, word in enumerate(words):
        if index == 0 or words[index - 1] != word:
            result.append(word)
    # Seeded defect: this slice drops the last kept word from the
    # result, so "x y y" incorrectly returns "x" instead of "x y".
    return " ".join(result[:-1])
