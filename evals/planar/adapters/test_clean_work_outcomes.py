import json
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import TestCase
from clean_work_outcomes import observe


class CleanWorkOutcomes(TestCase):
    def test_preserves_actual_values_and_rejects_missing_or_noninteger_reward(self):
        with TemporaryDirectory() as tmp:
            root=Path(tmp).resolve(); trial=root/'trial'
            for name in ('worker','review'):
                base=trial/'steps'/name/'verifier'; base.mkdir(parents=True)
                (base/'status.json').write_text(json.dumps({'status':'pass'} if name=='worker' else {'status':'incomplete','reason':'raw audit required'}))
                (base/'reward.txt').write_text('1\n' if name=='worker' else '0\n')
            native={'step_results':[{'step_name':'worker','exception_info':None,'verifier_result':{'rewards':{'reward':1.0}}},{'step_name':'review','exception_info':None,'verifier_result':{'rewards':{'reward':0.0}}}]}
            actual=observe(trial,root,native)
            self.assertEqual((actual['worker']['status'],actual['worker']['reward']),('pass',1))
            self.assertEqual((actual['review']['status'],actual['review']['reason'],actual['review']['reward']),('incomplete','raw audit required',0))
            reward=trial/'steps/review/verifier/reward.txt'; reward.write_text('0.0\n')
            with self.assertRaisesRegex(ValueError,'reward'): observe(trial,root,native)
            reward.unlink()
            with self.assertRaisesRegex(ValueError,'pair'): observe(trial,root,native)
            with self.assertRaisesRegex(ValueError,'step result'): observe(trial,root,{'step_results':native['step_results'][:1]})
