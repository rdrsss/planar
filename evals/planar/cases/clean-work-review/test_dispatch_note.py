from pathlib import Path
from unittest import TestCase
import step_identity


class DispatchNote(TestCase):
    def test_worker_instruction_retains_reviewed_capability_limits(self):
        case=Path(__file__).resolve().parent; instructions=step_identity.load_instructions(case)
        note=("The required implement child pin is `gpt-6.1-terra` with `medium` effort. "
              "In an earlier retained Codex CLI 0.169.0 native trial, `collaboration.spawn_agent` accepted an explicit request with this pin and `fork_turns: none`, produced a linked worker child, and that child completed. "
              "In the later clean-work trial, that invocation's tool description omitted Terra from an advertised override list, while its `model` argument accepted a string; no spawn was attempted. "
              "The earlier result does not establish current availability or observed backend identity. "
              "If the current dispatch tool accepts the explicit required pin, proceed with the ordinary worker packet. "
              "If it rejects the pin or reports a mismatch, retain the error and stop. Do not substitute a model.")
        self.assertEqual(instructions['worker'][0].count(note),1)
        self.assertNotIn(note,instructions['review'][0])
        step_identity.validate_selection(instructions,'gpt-6.1-terra','medium')
        self.assertEqual(step_identity.identify(instructions['worker'][0],instructions),'worker')
        with self.assertRaises(ValueError): step_identity.identify(instructions['worker'][0]+' ',instructions)
