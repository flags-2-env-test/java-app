#!/usr/bin/env python3
"""Mutation tests for the real compiled three-actor CTest transcript gate."""
import unittest
from contract import verify_three_actor_pipeline_stderr, verify_three_actor_flow_context

VALID = "\n".join([
    "SOURCE_PIPELINE_ROUNDTRIP_PASS actors=3 typed=1 forward=2 backward=2 result=19",
    "SOURCE_ACTOR_SPAWN_STOP_VERIFIED constructor_on_worker=1 released=1 processed=1 last=13",
    "SOURCE_ACTOR_SPAWN_STOP_VERIFIED constructor_on_worker=1 released=1 processed=2 last=14",
    "SOURCE_ACTOR_SPAWN_STOP_VERIFIED constructor_on_worker=1 released=1 processed=2 last=19",
    "SOURCE_ACTOR_GROUP_VERIFIED active=0 slots=3 peak=3 carriers=4",
    "",
])

CONTEXT = [
    "SOURCE_PIPELINE_CONTEXT_ADMIT flow=90 message=100 parent=0 target=0 direction=0 domain=77 generation=1",
    "SOURCE_PIPELINE_CONTEXT_ADMIT flow=90 message=101 parent=100 target=1 direction=1 domain=77 generation=1",
    "SOURCE_PIPELINE_CONTEXT_ADMIT flow=90 message=102 parent=101 target=2 direction=1 domain=77 generation=1",
    "SOURCE_PIPELINE_CONTEXT_ADMIT flow=90 message=103 parent=102 target=1 direction=2 domain=77 generation=1",
    "SOURCE_PIPELINE_CONTEXT_ADMIT flow=90 message=104 parent=103 target=0 direction=2 domain=77 generation=1",
]
CONTEXT_VALID = "\n".join(CONTEXT) + "\n"

class GuestEvidenceTests(unittest.TestCase):
    def test_compiled_guest_causal_identity(self):
        verify_three_actor_flow_context(CONTEXT_VALID)

    def test_reject_forged_or_replayed_guest_context(self):
        bad_transcripts = [
            CONTEXT_VALID + CONTEXT[3] + "\n",
            CONTEXT_VALID.replace("flow=90 message=102", "flow=91 message=102"),
            CONTEXT_VALID.replace("message=101 parent=100", "message=101 parent=0"),
            CONTEXT_VALID.replace("message=103 parent=102", "message=103 parent=101"),
            CONTEXT_VALID.replace("message=104 parent=103", "message=103 parent=103"),
            CONTEXT_VALID.replace("target=2 direction=1", "target=1 direction=1"),
            CONTEXT_VALID.replace("domain=77 generation=1", "domain=0 generation=1"),
            CONTEXT_VALID.replace("message=102 parent=101", "message=102 parent=101 forged=yes"),
            CONTEXT_VALID.replace("flow=90 message=103", "flow=0 message=103"),
            CONTEXT_VALID + ("forged" * 40000),
        ]
        for case in bad_transcripts:
            with self.subTest(case=case[-96:]), self.assertRaises(ValueError):
                verify_three_actor_flow_context(case)

    def test_complete_compiled_guest_summary(self):
        verify_three_actor_pipeline_stderr(VALID)

    def test_malformed_or_duplicate_summary(self):
        for transcript in [
            VALID.replace("result=19", "result=20"),
            VALID + VALID.splitlines()[0] + "\n",
            VALID.replace("forward=2", "forward=1"),
            VALID.replace("backward=2", "backward=1"),
        ]:
            with self.subTest(transcript=transcript[-65:]), self.assertRaises(ValueError):
                verify_three_actor_pipeline_stderr(transcript)

    def test_three_distinct_finalizers_with_correct_values(self):
        for transcript in [
            VALID.replace("processed=1 last=13", "processed=2 last=13"),
            VALID.replace("processed=2 last=14", "processed=1 last=14"),
            VALID.replace("processed=2 last=19", "processed=2 last=18"),
            VALID.replace("released=1", "released=0"),
            VALID.replace("last=14", "last=19"),
            VALID.replace("slots=3", "slots=4"),
            VALID.replace("active=0", "active=1"),
            VALID.replace("carriers=4", "carriers=0"),
        ]:
            with self.subTest(transcript=transcript[-65:]), self.assertRaises(ValueError):
                verify_three_actor_pipeline_stderr(transcript)

    def test_physical_completion_and_stop_receipts_are_strictly_ordered(self):
        lines = VALID.splitlines()
        mutations = [
            "\n".join([*lines[1:4], lines[0], lines[4]]) + "\n",
            "\n".join([lines[0], lines[4], *lines[1:4]]) + "\n",
            "\n".join([lines[0], lines[2], lines[1], *lines[3:]]) + "\n",
        ]
        for transcript in mutations:
            with self.subTest(transcript=transcript), self.assertRaises(ValueError):
                verify_three_actor_pipeline_stderr(transcript)

    def test_disguised_failure_and_oversized_input_fail_closed(self):
        for transcript in [
            VALID + "logger: SOURCE_PIPELINE_FAILURE code=7\n",
            VALID + "SOURCE_ACTOR_INVARIANT_FAILURE in diagnostic data\n",
            VALID + ("ignored-" * 20000),
            "",
        ]:
            with self.subTest(trailer=transcript[-65:]), self.assertRaises(ValueError):
                verify_three_actor_pipeline_stderr(transcript)

    def test_terminal_failure_is_incompatible_with_success(self):
        for marker in [
            "SOURCE_PIPELINE_FAILURE code=7",
            "SOURCE_PIPELINE_TERMINAL_STATUS code=7",
            "SOURCE_ACTOR_INVARIANT_FAILURE",
        ]:
            with self.subTest(marker=marker), self.assertRaises(ValueError):
                verify_three_actor_pipeline_stderr(VALID + marker + "\n")

if __name__ == "__main__":
    unittest.main()
