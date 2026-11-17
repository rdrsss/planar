from unittest import TestCase
from unittest.mock import patch
import launch
import retained_b_history as history

class HistoricalInventory(TestCase):
    def test_exact_committed_tree_without_executing_old_code(self):
        self.assertEqual(history.historical_inventory(launch.ROOT)['implementation_sha256'],history.INVENTORY)
    def test_wrong_expected_tree_or_inventory_fails(self):
        with patch.object(history,'TREE','wrong'):
            with self.assertRaisesRegex(ValueError,'tree mismatch'): history.historical_inventory(launch.ROOT)
        with patch.object(history,'INVENTORY','wrong'):
            with self.assertRaisesRegex(ValueError,'inventory mismatch'): history.historical_inventory(launch.ROOT)
