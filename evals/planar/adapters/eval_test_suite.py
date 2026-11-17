"""Explicit portable and retained-evidence suites; never infer project ownership."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

ADAPTER_CLASSES = {
    'test_retained_b_early_evidence.EarlyEvidence',
    'test_retained_b_early_release.EarlyRelease',
    'test_retained_b_early_snapshots.EarlySnapshots',
    'test_retained_b_early_trace.EarlyTrace',
    'test_terminal_failure.ActualTimeoutEvidence',
}
EVIDENCE_METHODS = {
    'test_source_stop_instruction.SourceStopInstruction.test_consumed_old_gate_refuses_changed_case_identity',
    'test_schema2_pretrial_release.RetainedPretrialEvidence.test_source_hashed_copies_and_static_mutations',
    'test_t055_historical_context.PublishedHistorical.test_exact_marker_and_tamper',
}
EXPECTED = {'adapters':21, 'retained-a-fresh-b':1, 'experiments':2}


def flatten(suite):
    for item in suite:
        if isinstance(item,unittest.TestSuite): yield from flatten(item)
        else: yield item


def is_evidence(test):
    name=test.id()
    return name in EVIDENCE_METHODS or any(name.startswith(prefix+'.') for prefix in ADAPTER_CLASSES)


class NoSkips(unittest.TextTestResult):
    def addSkip(self, test, reason):
        self.failures.append((test, 'required retained-evidence test skipped: '+reason))
        if self.showAll: self.stream.writeln('FAIL (required evidence unavailable)')
        elif self.dots: self.stream.write('F')


def run():
    if len(sys.argv)!=3 or sys.argv[1] not in ('portable','evidence'):
        raise SystemExit('usage: eval_test_suite.py portable|evidence DISCOVERY_DIR')
    mode, directory=sys.argv[1:]; group=Path(directory).name
    tmp=None
    if mode=='portable':
        tmp=tempfile.TemporaryDirectory(prefix='planar-evals-test-')
        base=Path(tmp.name).resolve(); home=base/'home'; project=home/'projects'/'fixture'; project.mkdir(parents=True)
        (project/'reviews').mkdir(); repo=Path(__file__).resolve().parents[3]
        (project/'project.yaml').write_text('name: fixture\nrepository:\n  path: '+json.dumps(str(repo))+'\n')
        os.environ['PLANAR_HOME']=str(home); os.environ['PLANAR_EVALS_PROJECT_DIR']=str(project)
        for key in ('PLANAR_EVALS_OUTPUT_ROOT','PLANAR_EVALS_LAYOUT_KIND','PLANAR_EVALS_CONTEXT'): os.environ.pop(key,None)
    else:
        if not os.environ.get('PLANAR_EVALS_PROJECT_DIR'):
            raise SystemExit('evidence suite requires explicit PLANAR_EVALS_PROJECT_DIR')
        from eval_context import resolve
        selected=resolve(Path(__file__).resolve().parents[3])
        if selected.layout_kind!='new': raise SystemExit('evidence project must be selected without a historical output override')
        if group=='experiments':
            os.environ['PLANAR_EVALS_LAYOUT_KIND']='override-jobs'
            os.environ['PLANAR_EVALS_OUTPUT_ROOT']=str(Path(selected.project_reviews_dir)/'t046-schema2-native-proof-prep-20261117/output')
    try:
        suite=unittest.defaultTestLoader.discover(directory,pattern='test_*.py')
        tests=list(flatten(suite)); evidence=[test for test in tests if is_evidence(test)]
        expected=EXPECTED.get(group,0)
        if len(evidence)!=expected: raise SystemExit(f'evidence suite inventory mismatch for {group}: expected {expected}, found {len(evidence)}')
        chosen=evidence if mode=='evidence' else [test for test in tests if not is_evidence(test)]
        print(f'{mode}: {len(chosen)} tests; {len(evidence)} separately selected retained-evidence tests',flush=True)
        runner=unittest.TextTestRunner(verbosity=2,resultclass=NoSkips if mode=='evidence' else unittest.TextTestResult)
        result=runner.run(unittest.TestSuite(chosen))
        if mode=='portable' and result.skipped:
            print(f'ERROR: portable suite unexpectedly skipped {len(result.skipped)} tests',file=sys.stderr)
            return 1
        return 0 if result.wasSuccessful() else 1
    finally:
        if tmp is not None: tmp.cleanup()

if __name__=='__main__': raise SystemExit(run())
