import io
import tarfile
import unittest
from safe_export import select_recovery_home


def archive(entries):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode='w') as tar:
        for name, kind in entries:
            info = tarfile.TarInfo(name)
            if kind == 'link': info.type = tarfile.SYMTYPE; info.linkname = 'elsewhere'; tar.addfile(info)
            elif kind == 'dir': info.type = tarfile.DIRTYPE; tar.addfile(info)
            else:
                info.size = 1; tar.addfile(info, io.BytesIO(b'x'))
    return stream.getvalue()

class RecoveryExport(unittest.TestCase):
    def test_declared_and_new_review(self):
        data = archive([('projects/revision-fault/tasks/T-001/checks-1.md','file'), ('projects/revision-fault/tasks/T-001/review-1-all.md','file')])
        self.assertEqual(len(select_recovery_home(data, {'projects/revision-fault/tasks/T-001/checks-1.md'})), 2)
    def test_project_decisions_exact_path_and_archive_safety(self):
        retained = {'projects/revision-fault/tasks/T-001/checks-1.md'}
        check = 'projects/revision-fault/tasks/T-001/checks-1.md'
        task = 'projects/revision-fault/tasks/T-001/session-worker-1-1.md'
        decisions = 'projects/revision-fault/decisions.md'
        self.assertEqual(set(select_recovery_home(archive([(check,'file'),(task,'file'),(decisions,'file')]), retained)), {check,task,decisions})
        self.assertEqual(set(select_recovery_home(archive([(check,'file')]), retained)), {check})
        for entries in ([(check,'file'),('projects/revision-fault/notes.md','file')],
                        [(check,'file'),('projects/other/decisions.md','file')],
                        [(check,'file'),(decisions,'link')],
                        [(check,'file'),(decisions,'file'),(decisions,'file')]):
            with self.subTest(entries=entries), self.assertRaises(ValueError):
                select_recovery_home(archive(entries), retained)

    def test_revision_and_cleanup_paths_remain_within_selected_subtrees(self):
        retained = {'projects/revision-fault/tasks/T-001/checks-1.md'}
        task = 'projects/revision-fault/tasks/T-001/'
        reviews = 'projects/revision-fault/reviews/'
        names = [task+name for name in ('worker-packet-2.md','worker-report-2.md','checks-2.md',
                 'reviewer-packet-2.md','review-1-all.md','review-2-all.md',
                 'session-worker-2-1.md','session-reviewer-2-1.md')]
        names += [reviews+'run-status.md',reviews+'cleanup-20261117-1.md']
        self.assertEqual(set(select_recovery_home(archive([(name,'file') for name in names]), retained)),set(names))

    def test_home_scoped_names(self):
        retained = {'projects/revision-fault/tasks/T-001/checks-1.md', 'settings.yaml'}
        data = archive([('projects/revision-fault/tasks/T-001/review-1-all.md','file'), ('settings.yaml','file')])
        self.assertEqual(len(select_recovery_home(data, retained)), 2)

    def test_home_root_contract_rejects_unprefixed_and_mixed_entries(self):
        retained = {'projects/revision-fault/tasks/T-001/checks-1.md', 'projects/revision-fault/reviews/spec-check-1.md'}
        good='projects/revision-fault/tasks/T-001/review-1-all.md'
        self.assertEqual(set(select_recovery_home(archive([('./'+good,'file')]), retained)),{good})
        for name,kind in (('tasks','dir'), ('tasks/T-001','dir'), ('reviews','dir'),
                          ('tasks/T-001/outside.md','file'), ('reviews/outside.md','file')):
            with self.subTest(name=name), self.assertRaises(ValueError):
                select_recovery_home(archive([(name,kind)]),retained)
            with self.subTest(mixed=name), self.assertRaises(ValueError):
                select_recovery_home(archive([(good,'file'),(name,kind)]),retained)
        with self.assertRaises(ValueError):
            select_recovery_home(archive([(good,'file')]),{'tasks/T-001/checks-1.md'})

    def test_fresh_packet_observation_names(self):
        retained={'projects/revision-fault/tasks/T-001/checks-1.md'}
        prefix='projects/revision-fault/tasks/T-001/'
        names=[prefix+'session-review-packet-1-replacement-1-1.md', prefix+'session-review-packet-1-replacement-1-2.md']
        self.assertEqual(set(select_recovery_home(archive([(n,'file') for n in names]),retained)),set(names))
        for name in ('projects/other/tasks/T-001/session-reviewer-1-1.md',prefix+'../session-review-packet-1-replacement-1-1.md',prefix+'auth-token.md'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                select_recovery_home(archive([(name,'file')]),retained)

    def test_canonical_retry_and_scoped_status(self):
        retained={'projects/revision-fault/tasks/T-001/checks-1.md', 'projects/revision-fault/reviews/spec-check-1.md'}
        names=['projects/revision-fault/tasks/T-001/session-reviewer-1-retry-'+str(n)+'.md' for n in (1,2,3)]
        names += ['projects/revision-fault/tasks/T-001/session-reviewer-2-retry-7-4.md',
                  'projects/revision-fault/tasks/T-001/preflight-review-1-replacement-1.md',
                  'projects/revision-fault/reviews/run-status.md', 'projects/revision-fault/reviews/cleanup-resume-20261116-1.md']
        self.assertEqual(set(select_recovery_home(archive([(n,'file') for n in names]),retained)),set(names))

    def test_canonical_task_local_invocation_ids(self):
        retained={'projects/revision-fault/tasks/T-001/checks-1.md'}
        prefix='projects/revision-fault/tasks/T-001/'
        ids=('review-preflight-1','worker-1','driver-5','reviewer-retry-1',
             '550e8400-e29b-41d4-a716-446655440000','arbitrary_safe_id')
        names=[prefix+'session-'+i+'-1.md' for i in ids]
        self.assertEqual(set(select_recovery_home(archive([(n,'file') for n in names]),retained)),set(names))
        for name in (prefix+'session-../x-1.md', 'projects/other/tasks/T-001/session-review-preflight-1-1.md', prefix+'x.yaml'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                select_recovery_home(archive([(name,'file')]),retained)

    def test_canonical_cleanup_run_ids_and_scope(self):
        retained={'projects/revision-fault/reviews/spec-check-1.md'}
        prefix='projects/revision-fault/reviews/'
        good=['cleanup-resume-20261116-1.md','cleanup-run_7-retry-2.md','cleanup-550e8400-e29b-41d4-a716-446655440000-17.md']
        self.assertEqual(set(select_recovery_home(archive([(prefix+n,'file') for n in good]),retained)),{prefix+n for n in good})
        with self.assertRaises(ValueError): select_recovery_home(archive([('projects/other/reviews/cleanup-resume-20261116-1.md','file')]),retained)
        with self.assertRaises(ValueError): select_recovery_home(archive([(prefix+good[0],'link')]),retained)

    def test_unknown_markdown_admitted_without_verifier_credit(self):
        retained={'projects/revision-fault/tasks/T-001/checks-1.md'}
        name='projects/revision-fault/tasks/T-001/custom-preflight-note.md'
        self.assertIn(name,select_recovery_home(archive([(name,'file')]),retained))

    def test_first_safe_offender_and_secret_redaction(self):
        retained={'projects/revision-fault/tasks/T-001/checks-1.md'}
        with self.assertRaisesRegex(ValueError, 'projects/revision-fault/tasks/T-001/unexpected.yaml'):
            select_recovery_home(archive([('projects/revision-fault/tasks/T-001/unexpected.yaml','file')]),retained)
        with self.assertRaises(ValueError) as caught:
            select_recovery_home(archive([('projects/revision-fault/tasks/T-001/secret-token.md','file')]),retained)
        self.assertIn('<redacted>',str(caught.exception))
        self.assertNotIn('secret-token',str(caught.exception))

    def test_reject_unsafe_and_extra(self):
        for name, kind in [('projects/revision-fault/tasks/T-001/checks-1.md','link'), ('../escape','file'), ('credentials','file'), ('projects/revision-fault/tasks/T-001/review-1-all.md/../../x','file')]:
            with self.subTest(name=name):
                with self.assertRaises(ValueError): select_recovery_home(archive([(name,kind)]), {'projects/revision-fault/tasks/T-001/checks-1.md'})
    def test_reject_duplicate(self):
        with self.assertRaises(ValueError): select_recovery_home(archive([('projects/revision-fault/tasks/T-001/checks-1.md','file')]*2), {'projects/revision-fault/tasks/T-001/checks-1.md'})
