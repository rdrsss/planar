import unittest

from textkit import dedupe_words


class DedupeWordsTests(unittest.TestCase):
    def test_no_duplicates_is_unchanged(self) -> None:
        self.assertEqual(dedupe_words("a b c"), "a b c")

    def test_collapses_consecutive_duplicates(self) -> None:
        self.assertEqual(dedupe_words("a a b b b c"), "a b c")

    def test_preserves_the_final_word(self) -> None:
        self.assertEqual(dedupe_words("x y y"), "x y")

    def test_non_adjacent_repeats_are_kept(self) -> None:
        self.assertEqual(dedupe_words("a b a"), "a b a")

    def test_empty_string(self) -> None:
        self.assertEqual(dedupe_words(""), "")


if __name__ == "__main__":
    unittest.main()
