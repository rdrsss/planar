import unittest
from unittest.mock import patch
from native import MissingEvidence, InvalidBehavior
import target_verifier

class CandidateVerifierControls(unittest.TestCase):
    def setUp(self):
        binding={'source_packet_sha256':'packet','assessment':{'lineage':{'parents':['source'],'children':['worker']}}}
        self.patches=[patch('source_packet.validate_inprocess',return_value=binding),patch('verify_restore._verify'),
            patch('capture.load',return_value={}),patch('chronology.receipt',return_value={}),
            patch('review_structure.verify',return_value={'file_structure':'pass'}),
            patch('target_native.correlate_target',return_value={'parents':['target'],'children':['a','b','c']}),
            patch('target_native.load_target',return_value=([],[])),patch('check_binding.verify',return_value={'checks':2})]
        for item in self.patches: item.start(); self.addCleanup(item.stop)
    def test_no_reward_without_target_host_gate(self):
        result=target_verifier.assess('packet','candidate','target')
        self.assertEqual(result['status'],'incomplete'); self.assertIn('host gate',result['reason'])
    def test_missing_native_evidence_never_passes(self):
        with patch('check_binding.verify',side_effect=MissingEvidence('check missing')):
            self.assertEqual(target_verifier.assess('packet','candidate','target')['status'],'incomplete')
    def test_explicit_false_check_is_failure(self):
        with patch('check_binding.verify',side_effect=InvalidBehavior('check failed')):
            self.assertEqual(target_verifier.assess('packet','candidate','target')['status'],'behavioral_failure')
    def test_source_binding_drift_never_passes(self):
        with patch('source_packet.validate_inprocess',side_effect=ValueError('source drift')):
            self.assertEqual(target_verifier.assess('packet','candidate','target')['status'],'incomplete')

if __name__=='__main__': unittest.main()
