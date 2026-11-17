import hashlib
import json
import shutil
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest import TestCase
from unittest.mock import patch

import clean_work_evidence as evidence
import clean_work_live as live
from clean_work_outcomes import observe
from clean_work_resources import check_projects

SOURCE=Path('/Users/mn/.planar/projects/planar/reviews/t045-clean-work-live-prep-20261117/output/jobs/planar-t044-891e08ce96d2474487fe2eab0ea6984b')
def h(path): return hashlib.sha256(path.read_bytes()).hexdigest()


class OneStep(TestCase):
    def test_retained_worker_evidence_and_fresh_checks(self):
        self.check_retained(SOURCE, 'incomplete')

    def test_retained_behavioral_worker_evidence_and_fresh_checks(self):
        self.check_retained(Path('/Users/mn/.planar/projects/planar/reviews/t045-clean-clarified-live-prep-20261117/output/jobs/planar-t044-6bd4371146a84b89b323e6b70e81c85d'), 'behavioral_failure')

    def check_retained(self, source, expected):
        native=json.loads((source/'native-trial-start.json').read_text()); original_job=Path(native['job_dir']); original_trial=original_job/native['trial_name']
        with TemporaryDirectory() as tmp:
            root=Path(tmp).resolve(); jobs=root/'jobs'; target=jobs/source.name; target.mkdir(parents=True); job=target/source.name; trial=job/native['trial_name']; trial.mkdir(parents=True)
            pairs=[(original_job/'result.json',job/'result.json'),(original_job/'config.json',job/'config.json'),(original_trial/'result.json',trial/'result.json'),(original_trial/'steps/worker/verifier/status.json',trial/'steps/worker/verifier/status.json'),(original_trial/'steps/worker/verifier/reward.txt',trial/'steps/worker/verifier/reward.txt')]
            for src,dst in pairs:
                dst.parent.mkdir(parents=True,exist_ok=True); shutil.copyfile(src,dst)
            transform={'source_sha256':{str(src):h(src) for src,_ in pairs},'copy_sha256':{str(dst):h(dst) for _,dst in pairs},'transform':'trial.config.trials_dir rewritten to isolated temporary job; all outcomes retained'}
            t=json.loads((trial/'result.json').read_text()); t['config']['trials_dir']=str(job); (trial/'result.json').write_text(json.dumps(t))
            transform['transformed_trial_sha256']=h(trial/'result.json'); (root/'source-transform-manifest.json').write_text(json.dumps(transform,sort_keys=True))
            actual=observe(trial,root,t); self.assertEqual((actual['worker']['status'],actual['worker']['reward']),(expected,0)); self.assertIsNone(actual['review'])
            evidence.validate_native((job/'result.json').read_bytes(),(trial/'result.json').read_bytes(),(job/'config.json').read_bytes(),job_id=native['job_id'],trial_id=native['trial_id'],trial_name=native['trial_name'],job_dir=job,task_name=t['task_name'],actual=actual,strategy='final')
            for name in ('launcher-terminal.json','watchdog-terminal.json','harbor-process.json','worker-planar-cleanup.json','worker-planar-terminal.json','verifier-compose-up-worker.json'):
                shutil.copyfile(source/name,target/name)
            shutil.copytree(source/'projects',target/'projects')
            calls=[]
            def empty(cmd,**kwargs): calls.append(cmd); return SimpleNamespace(returncode=0,stdout='')
            self.assertEqual(len(check_projects(target,native['trial_name'],t,run=empty)['queried_projects']),3); self.assertEqual(len(calls),9)
            with patch.object(live.launch,'reviews_root',return_value=root),patch.object(live.launch,'jobs_root',return_value=jobs),patch.object(live.os,'killpg',side_effect=ProcessLookupError),patch.object(live,'check_projects',return_value={'queried_projects':['a','b','c']}),patch.object(live.retained_b_census,'check',return_value={'matching_commands':0,'unresolved':0}):
                self.assertEqual(live.check(target,native['trial_name'],t)['native_group'],'absent')
                cleanup=target/'worker-planar-cleanup.json'; saved=cleanup.read_bytes(); altered=json.loads(saved); altered['paths']=['/wrong']; cleanup.write_text(json.dumps(altered))
                with self.assertRaisesRegex(ValueError,'cleanup unproven'): live.check(target,native['trial_name'],t)
                cleanup.write_bytes(saved)
                with patch.object(live.os,'killpg',return_value=None):
                    with self.assertRaisesRegex(ValueError,'process group active'): live.check(target,native['trial_name'],t)
                (target/'worker-planar-cleanup.json').unlink()
                with self.assertRaises((OSError,ValueError)): live.check(target,native['trial_name'],t)
            receipt=target/'verifier-compose-up-review.json'; receipt.write_text('{}')
            with self.assertRaisesRegex(ValueError,'contradictory review receipt'): check_projects(target,native['trial_name'],t,run=empty)
            receipt.unlink()
            review_name=(target.name+'-'+native['trial_name']).lower()+'__verifier__review'; foreign=target/'projects'/(hashlib.sha256(review_name.encode()).hexdigest()+'.json'); foreign.write_text(json.dumps({'execution_id':target.name,'compose_project':review_name}))
            with self.assertRaisesRegex(ValueError,'contradictory review registration'): check_projects(target,native['trial_name'],t,run=empty)
            foreign.unlink()
            for status,reward in (('pass',1),('behavioral_failure',0)):
                status_path=trial/'steps/worker/verifier/status.json'; reward_path=trial/'steps/worker/verifier/reward.txt'; before=status_path.read_bytes(); old_reward=reward_path.read_bytes(); status_path.write_text(json.dumps({'status':status,'reason':'fixture'})); reward_path.write_text(str(reward)+'\n'); bad_grade=json.loads(json.dumps(t)); bad_grade['step_results'][0]['verifier_result']['rewards']['reward']=float(reward)
                with self.assertRaisesRegex(ValueError,'one-step worker outcome'): observe(trial,root,bad_grade)
                status_path.write_bytes(before); reward_path.write_bytes(old_reward)
            bad=json.loads(json.dumps(t)); bad['step_results'][0]['verifier_result']['rewards']['reward']=1.0
            with self.assertRaises(ValueError): observe(trial,root,bad)
            bad=json.loads(json.dumps(t)); bad['step_results'][0]['step_name']='review'
            with self.assertRaises(ValueError): observe(trial,root,bad)
