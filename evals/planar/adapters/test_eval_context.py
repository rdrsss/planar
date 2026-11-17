import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import eval_context

class Selection(unittest.TestCase):
    def setUp(self):
        base = os.environ.get('PLANAR_T055_TEST_SCRATCH')
        self.temp = tempfile.TemporaryDirectory(prefix='context ', dir=base)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.repo = self.root/'repository with spaces'; self.repo.mkdir()
        subprocess.run(['git','init','-q',str(self.repo)],check=True)
        self.home = self.root/'home'; self.project = self.home/'projects'/'demo'; self.project.mkdir(parents=True)
        (self.project/'project.yaml').write_text('name: demo\nrepository:\n  path: '+json.dumps(str(self.repo))+'\n')
        self.env = {'PLANAR_HOME':str(self.home), 'PLANAR_EVALS_PROJECT_DIR':str(self.project)}

    def test_default_external_and_missing_selection(self):
        with patch.dict(os.environ, self.env, clear=True):
            c=eval_context.resolve(self.repo)
            self.assertEqual(c.output_root,str(self.project/'scratch'/'scratch-evals'))
            out=self.root/'external output'
            with patch.dict(os.environ, {'PLANAR_EVALS_OUTPUT_ROOT':str(out)}):
                c=eval_context.resolve(self.repo)
                self.assertEqual(c.output_root,str(out)); self.assertEqual(c.selection_source,'explicit override')
            with patch.dict(os.environ, {'PLANAR_EVALS_PROJECT_DIR':''}):
                with self.assertRaisesRegex(ValueError,'PROJECT_DIR'): eval_context.resolve(self.repo)
        self.assertFalse((self.project/'scratch').exists())

    def test_symlink_mismatch_and_layout(self):
        alias=self.root/'alias'; alias.symlink_to(self.root,target_is_directory=True)
        with patch.dict(os.environ,self.env,clear=True):
            with patch.dict(os.environ,{'PLANAR_EVALS_OUTPUT_ROOT':str(alias/'escape')}):
                with self.assertRaises(ValueError): eval_context.resolve(self.repo)
            with patch.dict(os.environ,{'PLANAR_EVALS_OUTPUT_ROOT':str(self.repo/'output')}):
                with self.assertRaises(ValueError): eval_context.resolve(self.repo)
            (self.project/'reviews').mkdir()
            with patch.dict(os.environ,{'PLANAR_EVALS_LAYOUT_KIND':'legacy-reviews','PLANAR_EVALS_OUTPUT_ROOT':str(self.project/'reviews')}):
                c=eval_context.resolve(self.repo,historical=True)
                self.assertEqual(c.jobs_root,str(self.project/'reviews'/'harbor-t044-jobs'))
                with self.assertRaises(ValueError): eval_context.resolve(self.repo)

    def test_project_override_and_shared_repository(self):
        other=self.root/'other project'; other.mkdir(); (other/'project.yaml').write_text((self.project/'project.yaml').read_text())
        (self.home/'settings.yaml').write_text('projects:\n  demo:\n    path: '+json.dumps(str(other))+'\n')
        with patch.dict(os.environ,self.env,clear=True):
            with self.assertRaisesRegex(ValueError,'settings'): eval_context.resolve(self.repo)
            with patch.dict(os.environ,{'PLANAR_EVALS_PROJECT_DIR':str(other)}):
                self.assertEqual(eval_context.resolve(self.repo).project_dir,str(other))

    def test_forwarded_context_detects_change(self):
        with patch.dict(os.environ,self.env,clear=True):
            original=eval_context.resolve(self.repo)
            with patch.dict(os.environ,{'PLANAR_EVALS_CONTEXT':json.dumps(original.identity())}):
                self.assertEqual(eval_context.resolve(self.repo),original)
                with patch.dict(os.environ,{'PLANAR_EVALS_OUTPUT_ROOT':str(self.root/'different')}):
                    with self.assertRaisesRegex(ValueError,'context mismatch'): eval_context.resolve(self.repo)

    def test_duplicate_settings_refused(self):
        (self.home/'settings.yaml').write_text('projects: {}\nprojects: {}\n')
        with patch.dict(os.environ,self.env,clear=True):
            with self.assertRaisesRegex(ValueError,'duplicate'): eval_context.resolve(self.repo)

    def test_child_recomputes_settings_and_source(self):
        selected=self.root/'configured project'; selected.mkdir()
        (selected/'project.yaml').write_text((self.project/'project.yaml').read_text())
        (self.home/'settings.yaml').write_text('projects:\n  demo:\n    path: '+json.dumps(str(selected))+'\n')
        env={**self.env,'PLANAR_EVALS_PROJECT_DIR':str(selected)}
        with patch.dict(os.environ,env,clear=True): parent=eval_context.resolve(self.repo)
        script='import json,sys; from eval_context import resolve; print(json.dumps(resolve(sys.argv[1]).identity(),sort_keys=True))'
        child={'PATH':os.defpath,'PYTHONPATH':str(Path(eval_context.__file__).parent), **env,'PLANAR_EVALS_CONTEXT':json.dumps(parent.identity())}
        def invoke(changes=None):
            e={**child,**(changes or {})}
            return subprocess.run([os.sys.executable,'-c',script,str(self.repo)],env=e,capture_output=True,text=True)
        self.assertEqual(invoke().returncode,0)
        self.assertNotEqual(invoke({'PLANAR_EVALS_OUTPUT_ROOT':parent.output_root}).returncode,0)
        bad=dict(parent.identity()); bad['settings_home_source']='default'
        self.assertNotEqual(invoke({'PLANAR_EVALS_CONTEXT':json.dumps(bad)}).returncode,0)
        (self.home/'settings.yaml').write_text((self.home/'settings.yaml').read_text()+'# changed bytes\n')
        self.assertNotEqual(invoke().returncode,0)
    def test_plan_context_binds_digest_without_extra_harbor_fields(self):
        import launch
        from harbor.models.job.config import JobConfig
        (self.project/'project.yaml').write_text('name: demo\nrepository:\n  path: '+json.dumps(str(launch.ROOT))+'\n')
        candidate=json.loads((launch.CASES['plugin-smoke']/'candidate.json').read_text())
        with patch.dict(os.environ,self.env,clear=True), patch.object(launch,'inventory',return_value=candidate):
            plan=launch.plan('plugin-smoke')
            context=launch.context()
            self.assertEqual(plan['prerequisites']['selection_context'],context.identity())
            self.assertEqual(plan['prerequisites']['selection_context_sha256'],context.digest())
            typed=JobConfig.model_validate(plan['configuration'])
            self.assertNotIn('selection_context',typed.model_dump())
            self.assertEqual(plan['configuration']['jobs_dir'],context.jobs_root)
            with patch.dict(os.environ,{'PLANAR_EVALS_OUTPUT_ROOT':context.output_root}):
                other=launch.plan('plugin-smoke')
                self.assertNotEqual(plan['configuration_sha256'],other['configuration_sha256'])
    def test_home_source_and_changed_settings_are_bound(self):
        home=self.root/'user'; default=home/'.planar'; (default/'projects').mkdir(parents=True)
        selected=default/'projects'/'demo'; selected.mkdir()
        (selected/'project.yaml').write_text((self.project/'project.yaml').read_text())
        env={'HOME':str(home),'PLANAR_EVALS_PROJECT_DIR':str(selected)}
        with patch.dict(os.environ,env,clear=True):
            implicit=eval_context.resolve(self.repo)
            self.assertEqual(implicit.settings_home_source,'default')
            self.assertEqual(implicit.settings_file_sha256,'absent')
            with patch.dict(os.environ,{'PLANAR_HOME':str(default)}):
                explicit=eval_context.resolve(self.repo)
                self.assertEqual(explicit.settings_home_source,'explicit')
                self.assertNotEqual(implicit.digest(),explicit.digest())
            (default/'settings.yaml').write_text('{}\n')
            self.assertNotEqual(eval_context.resolve(self.repo).digest(),implicit.digest())
    def test_historical_manual_recovery_binds_roots_and_gate(self):
        import hashlib
        import launch, recover
        (self.project/'project.yaml').write_text('name: demo\nrepository:\n  path: '+json.dumps(str(launch.ROOT))+'\n')
        for layout in ('legacy-reviews','override-jobs'):
            output=self.project/'reviews' if layout=='legacy-reviews' else self.root/'old output'
            output.mkdir(exist_ok=True)
            jobs=output/('harbor-t044-jobs' if layout=='legacy-reviews' else 'jobs')
            evidence=jobs/'planar-t044-fixture'; evidence.mkdir(parents=True)
            gate=output/'gate.json'; raw=json.dumps({'verdict':'approve'}).encode(); gate.write_bytes(raw)
            marker=output/'harbor-t044-consumed'/hashlib.sha256(json.dumps(json.loads(raw),sort_keys=True,separators=(',',':')).encode()).hexdigest()
            marker.parent.mkdir(); marker.write_bytes(b'reserved\n')
            config=json.dumps({'jobs_dir':str(evidence)}).encode(); (evidence/'job-config.json').write_bytes(config)
            record={'execution_id':evidence.name,'job_config_sha256':hashlib.sha256(config).hexdigest(), 'review_sha256':hashlib.sha256(raw).hexdigest(), 'prerequisites':{'project_root':str(self.project),'evidence_write_root':str(jobs)}}
            (evidence/'launcher.json').write_text(json.dumps(record)); (evidence/'harbor-process.json').write_text(json.dumps({'execution_id':evidence.name,'pid':12345})); pending=evidence/'recovery-required'; pending.write_text('pending')
            env={**self.env,'PLANAR_EVALS_OUTPUT_ROOT':str(output),'PLANAR_EVALS_LAYOUT_KIND':layout,'PLANAR_EVALS_REVIEW_PATH':str(gate)}
            with patch.dict(os.environ,env,clear=True), patch('os.kill',side_effect=ProcessLookupError), patch.object(recover,'recover_projects') as cleanup:
                with patch.dict(os.environ,{'PLANAR_EVALS_REVIEW_PATH':str(output/'wrong')}):
                    with self.assertRaises(ValueError): recover.manual_recover(evidence)
                cleanup.assert_not_called(); self.assertTrue(pending.exists())
                gate.write_bytes(raw+b' ')
                with self.assertRaisesRegex(ValueError,'hash mismatch'): recover.manual_recover(evidence)
                cleanup.assert_not_called(); gate.write_bytes(raw)
                recover.manual_recover(evidence); cleanup.assert_called_once_with(evidence)
                self.assertFalse(pending.exists())
    def test_watchdog_and_cleanup_reject_unbound_context_before_mutation(self):
        import hashlib
        import launch, recover, watchdog
        from unittest.mock import Mock
        (self.project/'project.yaml').write_text('name: demo\nrepository:\n  path: '+json.dumps(str(launch.ROOT))+'\n')
        output=self.root/'output'; output.mkdir()
        env={**self.env,'PLANAR_EVALS_OUTPUT_ROOT':str(output),'PLANAR_EVALS_LAYOUT_KIND':'new'}
        with patch.dict(os.environ,env,clear=True): ctx=launch.context()
        evidence=Path(ctx.jobs_root)/'planar-t044-fixture'; evidence.mkdir(parents=True)
        (evidence/'recovery-required').write_text('pending'); (evidence/'projects').mkdir()
        config=json.dumps({'jobs_dir':str(evidence)}).encode(); (evidence/'job-config.json').write_bytes(config)
        record={'execution_id':evidence.name,'job_config_sha256':hashlib.sha256(config).hexdigest(),
                'prerequisites':{'project_root':ctx.project_dir,'evidence_write_root':ctx.jobs_root,
                                 'selection_context':ctx.identity(),'selection_context_sha256':ctx.digest()}}
        (evidence/'launcher.json').write_text(json.dumps(record))
        env.update({'PLANAR_EVALS_CONTEXT':json.dumps(ctx.identity()),'PLANAR_T044_EVIDENCE_DIR':str(evidence),'PLANAR_T044_PARENT_PID':str(os.getpid())})
        fake_docker=Mock()
        with patch.dict(os.environ,env,clear=True):
            forged={**ctx.identity(),'settings_file_sha256':'0'*64}
            with patch.dict(os.environ,{'PLANAR_EVALS_CONTEXT':json.dumps(forged)}), patch.object(watchdog,'supervise') as supervise, patch.object(watchdog,'recover_projects') as cleanup:
                with self.assertRaises(ValueError): watchdog.main()
                supervise.assert_not_called(); cleanup.assert_not_called()
                with self.assertRaises(ValueError): recover.recover_projects(evidence,run=fake_docker)
            fake_docker.assert_not_called(); self.assertTrue((evidence/'recovery-required').is_file())
            self.assertFalse((evidence/'watchdog-terminal.json').exists())
            wrong=self.root/'not-owned'; wrong.mkdir(); (wrong/'recovery-required').write_text('pending')
            with self.assertRaises(ValueError): recover.recover_projects(wrong,run=fake_docker)
            fake_docker.assert_not_called()
            self.assertEqual(recover.validate_owned_evidence(evidence),evidence)
            recover.recover_projects(evidence,run=fake_docker)
            fake_docker.assert_not_called()  # empty, exact owned registry
