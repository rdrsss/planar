import unittest
from outcome import pending_review


class Verdict(unittest.TestCase):
    def test_actual_verdict_is_preserved_without_granting_pass(self):
        for verdict in ('approve', 'request-changes', 'open-question'):
            with self.subTest(verdict=verdict):
                result=pending_review(verdict,packet_bound=True,native_child_bound=True,check_bound=True)
                self.assertEqual((result['status'],result['reward'],result['review_verdict']),('incomplete',0,verdict))

    def test_missing_bindings_and_unknown_verdict_do_not_claim_review(self):
        for verdict, binding in (('approve',False),('approve',1),('unknown',True)):
            result=pending_review(verdict,packet_bound=binding,native_child_bound=True,check_bound=True)
            self.assertEqual(result['status'],'incomplete')
            self.assertNotIn('review_verdict',result)
