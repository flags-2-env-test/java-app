#!/usr/bin/env python3
"""Adversarial tests for the backend-neutral actor trace state machine."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from validate import check_trace, TraceViolation

def good():
    events = [
        {"kind":"spawn","actor":"alice"},
        {"kind":"spawn","actor":"bob"},
        {"kind":"exclusive_acquire","actor":"alice","resource":"account"},
        {"kind":"send","actor":"alice","target":"bob","message_id":"m1"},
        {"kind":"receive","actor":"bob","message_id":"m1"},
        {"kind":"exclusive_release","actor":"alice","resource":"account"},
        {"kind":"terminate","actor":"bob"},
        {"kind":"terminate","actor":"alice"},
    ]
    return {"schema_version":"1.0.0","complete":True,
            "events":[dict(seq=i, **event) for i,event in enumerate(events)]}

class ActorTraceProof(unittest.TestCase):
    def assert_invalid(self, trace):
        with self.assertRaises(Exception):
            check_trace(trace)

    def test_good_trace(self):
        metrics = check_trace(good())
        self.assertEqual(metrics["messages_received"], 1)
        self.assertEqual(metrics["actors"], 2)

    def test_external_ingress_roundtrip(self):
        trace = {"schema_version":"1.0.0","complete":True,
                 "events":[
                     {"seq":0,"kind":"spawn","actor":"receiver"},
                     {"seq":1,"kind":"admit","actor":"receiver","message_id":"external_1"},
                     {"seq":2,"kind":"receive","actor":"receiver","message_id":"external_1"},
                     {"seq":3,"kind":"terminate","actor":"receiver"},
                 ]}
        self.assertEqual(check_trace(trace)["messages_received"], 1)
        duplicate = copy.deepcopy(trace)
        duplicate["events"].insert(2, dict(seq=2,kind="admit",actor="receiver",
                                           message_id="external_1"))
        for seq,event in enumerate(duplicate["events"]): event["seq"] = seq
        self.assert_invalid(duplicate)
        missing = copy.deepcopy(trace)
        del missing["events"][1]["message_id"]
        self.assert_invalid(missing)
        wrong = copy.deepcopy(trace)
        wrong["events"][1]["actor"] = "unspawned"
        self.assert_invalid(wrong)

    def test_partial_trace_does_not_imply_eventual_delivery(self):
        t = good()
        t["complete"] = False
        t["events"] = t["events"][:4]
        self.assertEqual(check_trace(t)["messages_received"], 0)
        t["complete"] = True
        self.assert_invalid(t)

    def test_duplicate_delivery(self):
        t = good()
        t["events"].insert(5, {"kind":"receive","actor":"bob","message_id":"m1"})
        for i,e in enumerate(t["events"]): e["seq"] = i
        self.assert_invalid(t)

    def test_double_exclusive_owner(self):
        t = good()
        t["events"].insert(3, {"kind":"exclusive_acquire","actor":"bob","resource":"account"})
        for i,e in enumerate(t["events"]): e["seq"] = i
        self.assert_invalid(t)

    def test_wrong_message_recipient(self):
        t = good()
        t["events"][4]["actor"] = "alice"
        self.assert_invalid(t)

    def test_release_by_nonowner(self):
        t = good()
        t["events"][5]["actor"] = "bob"
        self.assert_invalid(t)

    def test_termination_with_held_resource(self):
        t = good()
        t["events"][5] = {"seq":5,"kind":"terminate","actor":"alice"}
        self.assert_invalid(t)

    def test_no_events_after_termination(self):
        t = good()
        t["events"].append({"seq":8,"kind":"send","actor":"alice","target":"bob","message_id":"m2"})
        self.assert_invalid(t)

    def test_duplicate_actor_identity(self):
        t = good()
        t["events"][1]["actor"] = "alice"
        self.assert_invalid(t)

    def test_noncontiguous_sequence(self):
        t = good()
        t["events"][4]["seq"] = 7
        self.assert_invalid(t)

    def test_schema_rejects_unrecognized_events(self):
        t = good()
        t["events"][3]["kind"] = "memory_teleport"
        self.assert_invalid(t)

    def test_kind_cannot_smuggle_extra_fields(self):
        t = good()
        t["events"][1]["resource"] = "account"
        self.assert_invalid(t)

    def test_duplicate_message_id(self):
        t = good()
        t["events"].insert(5, {"kind":"send","actor":"alice","target":"bob","message_id":"m1"})
        for i,e in enumerate(t["events"]): e["seq"] = i
        self.assert_invalid(t)


    def test_seeded_multi_actor_valid_traces(self):
        # Deterministic property tests for the *validator*, not actual actors.
        # All messages are sent by live actors and delivered once to a live
        # target; acquiring/releasing an exclusive lease is properly paired.
        import random
        for seed in range(48):
            rng = random.Random(20261009 + seed)
            actors = ("alpha", "beta", "gamma")
            events = [{"kind": "spawn", "actor": name} for name in actors]
            pending = []
            for i in range(24):
                sender = rng.choice(actors)
                target = rng.choice(actors)
                msg = f"seed{seed}_msg{i}"
                events.append({"kind": "send", "actor": sender,
                               "target": target, "message_id": msg})
                pending.append((target, msg))
            rng.shuffle(pending)
            for target, msg in pending:
                events.append({"kind": "receive", "actor": target,
                               "message_id": msg})
            owner = rng.choice(actors)
            events.extend((
                {"kind": "exclusive_acquire", "actor": owner, "resource": "quota"},
                {"kind": "exclusive_release", "actor": owner, "resource": "quota"},
            ))
            events.extend({"kind": "terminate", "actor": actor}
                          for actor in reversed(actors))
            trace = {"schema_version": "1.0.0", "complete": True,
                     "events": [dict(seq=i, **event)
                                for i, event in enumerate(events)]}
            metrics = check_trace(trace)
            self.assertEqual(metrics["messages_sent"], 24)
            self.assertEqual(metrics["messages_received"], 24)
            self.assertEqual(metrics["actors"], 3)

    def test_seeded_adversarial_mutations_fail_closed(self):
        # Ensure every family of malformed trace fails and never reports a
        # successful ownership/delivery proof. 64 reproducible mutations.
        for case in range(64):
            t = good()
            events = t["events"]
            variant = case % 8
            if variant == 0:  # second send reuses an existing identifier
                events.insert(5, {"kind":"send", "actor":"alice",
                                  "target":"bob", "message_id":"m1"})
            elif variant == 1:  # exclusive resource stolen by a second actor
                events.insert(3, {"kind":"exclusive_acquire",
                                  "actor":"bob", "resource":"account"})
            elif variant == 2:  # receive by the wrong actor
                events[4]["actor"] = "alice"
            elif variant == 3:  # deliver a message twice
                events.insert(5, {"kind":"receive", "actor":"bob",
                                  "message_id":"m1"})
            elif variant == 4:  # drop receipt from a completed trace
                events.pop(4)
            elif variant == 5:  # terminate before releasing an exclusive lease
                events[5] = {"kind":"terminate", "actor":"alice"}
            elif variant == 6:  # use after an actor's final termination
                events.append({"kind":"send", "actor":"alice",
                               "target":"bob", "message_id":"new"})
            else:  # send to never-created actor
                events[3]["target"] = "unborn"
            for i, event in enumerate(events):
                event["seq"] = i
            self.assert_invalid(t)

    def test_trace_schema_mutations_cannot_escape_kind_requirements(self):
        # Structural schema rejection independent of state-machine checks.
        for field, replacement in (
            ("actor", "Unexpected Capital"),
            ("message_id", "bad message id with spaces"),
            ("seq", -1),
            ("kind", "debug_private_heap"),
        ):
            trace = good()
            event = trace["events"][3]
            event[field] = replacement
            self.assert_invalid(trace)

if __name__ == "__main__":
    unittest.main()
