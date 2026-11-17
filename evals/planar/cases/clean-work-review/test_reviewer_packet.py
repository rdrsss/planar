import unittest
from reviewer_packet import PacketError, verify


class PacketTests(unittest.TestCase):
    def test_required_packet_evidence(self):
        shared = '\n'.join(f'## {name}\n\nfull {name}\n' for name in ('Task', 'Paths you may create or change', 'Cited spec sections, in full', 'Checks this task must pass'))
        worker = '# Worker packet\n\n' + shared
        checks = '# Checks\n\nactual execution and results\n'
        diff = 'diff --git a/RESULT.txt b/RESULT.txt\n+2\n'
        packet = ('# Reviewer packet: T-001, attempt 1\n\n## Resolved configuration\n\nreviewer gpt-6.1-sol medium revision-fault.yaml ea05cae38488f1e509db44eacb82dc057e9ce1018652e2abc315b793c09650f7 and ownership\n\n' + shared +
                  '\n## Change\n\n~~~diff\n' + diff + '~~~\n\n## Unattributed changes\n\nNone.\n\n## Earlier uncommitted tasks\n\nNone.\n\n## Checks\n\n~~~\n' + checks + '~~~\n\n## Earlier reviews\n\nNone.\n\nstatus: complete\n')
        # Canonical attempt 1 may omit the empty prior-review section, and
        # the Checks fence may carry its markdown language tag.
        verify(packet, worker, checks, diff)
        canonical=packet.replace('## Earlier reviews\n\nNone.\n\n','').replace('## Checks\n\n~~~\n','## Checks\n\n~~~~markdown\n').replace(checks+'~~~\n',checks+'~~~~\n')
        verify(canonical, worker, checks, diff)
        with self.assertRaises(PacketError): verify(packet.replace('## Earlier reviews\n\nNone.','## Earlier reviews\n\nInvented review.'),worker,checks,diff)
        with self.assertRaises(PacketError): verify(canonical.replace('actual execution and results','passed'),worker,checks,diff)
        for bad in (packet.replace('+2', '+3'), packet.replace('full Task', 'partial Task'),
                    packet.replace('None.', ''), packet.replace('actual execution and results', 'passed')):
            with self.subTest(packet=bad), self.assertRaises(PacketError):
                verify(bad, worker, checks, diff)
