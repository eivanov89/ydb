"""Actual C++ SDK boundary; modeled execution never certifies production Auto."""
import tempfile
import unittest
import uuid

import yatest.common

from ydb.apps.bridge_skipper.acceptance.lib.harness import MockEnvironment, wait
from ydb.apps.bridge_skipper.acceptance.lib.contracts import Contracts, LOCAL_CASES, MockFixture
from ydb.apps.bridge_skipper.acceptance.lib.report import Report


MOCK = "ydb/apps/bridge_skipper/ydbd_mock/ydbd_mock"
CONTROLLER = "ydb/apps/bridge_skipper/bridge_skipper"


class ExecutableAcceptanceTest(unittest.TestCase):
    def environment(self, root, **options):
        return MockEnvironment(yatest.common.binary_path(MOCK), yatest.common.binary_path(CONTROLLER), root,
                               artifact_root=yatest.common.output_path("process_logs_" + uuid.uuid4().hex), **options)

    def test_shared_public_wire_contracts(self):
        with tempfile.TemporaryDirectory(prefix="skipper_shared_contracts_") as root:
            environment = self.environment(root)
            try:
                environment.start()
                endpoints = [next(node["dial_endpoint"] for node in environment.manifest["nodes"] if node["pile"] == name)
                             for name in environment.topology.pile_names]
                report = Report("mock", {"controller_binary": CONTROLLER, "mock_binary": MOCK},
                                ["repository PY3TEST named shared contracts"], topology=environment.manifest)
                try:
                    for case in LOCAL_CASES:
                        with self.subTest(contract=case):
                            contracts = Contracts(endpoints, environment.topology.pile_names, MockFixture(environment))
                            try:
                                report.run(case, lambda: contracts.run(case))
                            finally:
                                contracts.close()
                finally:
                    report.skip("implicit_unlabeled_host_resolution", "empty names exercised; real ImplicitPileId recording remains external")
                    report.write(yatest.common.output_path("shared_mock_contracts.json"))
            finally:
                environment.close()

    def workflow_case(self, name, action):
        build = {"source_revision": yatest.common.get_param("source_revision", "workspace-build"),
                 "controller_binary": CONTROLLER, "mock_binary": MOCK}
        report = Report("mock", build, ["repository PY3TEST acceptance target"])
        try:
            with tempfile.TemporaryDirectory(prefix="skipper_acceptance_") as root:
                initial = {"configured_piles": ["pile1"], "bridged": False} if name == "convert" else None
                if name == "add_pile":
                    initial = {"configured_piles": ["pile1"]}
                environment = self.environment(root, initial=initial)
                try:
                    environment.start()
                    self.assertFalse(environment.controller.state()["evidence"]["qualified"])
                    if name in ("rejoin", "remove_pile"):
                        environment.workflow({"action": "failover", "targets": [action["target"]], "successor": None})
                    if name == "add_pile":
                        environment.control("/topology", {"configured_piles": ["pile1", "pile2", ""]})
                    report.run(name, lambda: environment.workflow(
                        action, discard_response=name == "failover", retained_rejoin=name == "rejoin"))
                    if name == "remove_pile":
                        self.assertEqual(environment.vector()[""], "INACTIVE")
                    if name == "add_pile":
                        self.assertEqual(environment.vector()[""], "DISCONNECTED")
                    if name == "failover":
                        self.assertEqual(environment.vector()["pile2"], "DISCONNECTED")
                        self.assertEqual(environment.vector()[""], "DISCONNECTED")
                finally:
                    environment.close()
        finally:
            report.skip("real_cli_demo_compatibility", "retained separate CLI/demo tier; no external CLI dependency here")
            report.skip("real_ydb_and_application_durability", "requires supplied supported-build cluster and application workload")
            report.write(yatest.common.output_path("mock_acceptance_%s.json" % name))

    def test_workflow_failover(self):
        self.workflow_case("failover", {"action": "failover", "targets": ["pile2", ""], "successor": None})

    def test_workflow_switchover(self):
        self.workflow_case("switchover", {"action": "switchover", "target": "pile2"})

    def test_workflow_takedown(self):
        self.workflow_case("takedown", {"action": "takedown", "target": "pile2"})

    def test_workflow_primary_maintenance(self):
        self.workflow_case("primary_maintenance", {"action": "primary_maintenance", "target": "pile1", "successor": "pile2"})

    def test_workflow_rejoin_retains_unqualified_obligation(self):
        self.workflow_case("rejoin", {"action": "rejoin", "target": "pile2"})

    def test_workflow_convert(self):
        self.workflow_case("convert", {"action": "convert"})

    def test_workflow_add_exact_empty_name(self):
        self.workflow_case("add_pile", {"action": "add_pile", "target": ""})

    def test_workflow_remove_exact_empty_name(self):
        self.workflow_case("remove_pile", {"action": "remove_pile", "target": ""})

    def test_lost_mutation_replies_timeout_and_no_delivered_request_replay(self):
        cases = (("after_acceptance", "unavailable"), ("after_commit", "unavailable"), ("after_commit", "hold"))
        for stage, action in cases:
            with self.subTest(stage=stage, action=action), tempfile.TemporaryDirectory(prefix="skipper_lost_reply_") as root:
                environment = self.environment(root)
                try:
                    environment.start()
                    method = "/Ydb.Bridge.V1.BridgeService/UpdateClusterState"
                    # Same configured gate on every possible direct endpoint.
                    for pile in environment.topology.pile_names:
                        point = {"target": pile, "method": method, "stage": stage, "action": action, "once": True}
                        environment.control("/failpoints", point)
                    before = {event["rpc_id"] for event in environment.mutation_events()}
                    operation_id, body = environment.controller.submit(
                        {"action": "failover", "targets": ["pile2"], "successor": None})

                    def failpoint_hit():
                        return next((e for e in environment.events()
                                     if e["kind"] == "failpoint.hit" and e.get("rpc_id") not in before), None)

                    hit = wait(failpoint_hit, "mutation failpoint event")
                    wait(lambda: any(e["kind"] in ("rpc.end", "rpc.cancelled") and e.get("rpc_id") == hit["rpc_id"]
                                     for e in environment.events()), "correlated terminal RPC event", timeout=15)
                    if action == "hold":
                        # Release after the actual SDK timeout/cancellation event,
                        # never infer timeout from a sleep.
                        for pile in environment.topology.pile_names:
                            point = {"target": pile, "method": method, "stage": stage, "action": "release"}
                            environment.control("/failpoints", point)
                    events = [e for e in environment.mutation_events() if e["rpc_id"] not in before]
                    self.assertEqual(len(events), 1, events)
                    self.assertEqual(events[0]["rpc_id"], hit["rpc_id"])
                    operation = environment.controller.operation(operation_id)
                    self.assertEqual(len(operation["attempts"]), 1, operation)
                    if stage == "after_commit":
                        wait(lambda: environment.vector()["pile2"] == "DISCONNECTED", "committed replica after lost reply")
                    else:
                        self.assertEqual(environment.vector()["pile2"], "SYNCHRONIZED")
                    # Replaying the exact durable HTTP receipt cannot create a
                    # second SDK invocation, even after the preview changed.
                    environment.controller.post("/api/operations", body)
                    self.assertEqual(len([e for e in environment.mutation_events() if e["rpc_id"] not in before]), 1)
                finally:
                    environment.close()

    def test_production_auto_remains_unqualified_with_synthetic_self_check(self):
        with tempfile.TemporaryDirectory(prefix="skipper_unqualified_") as root:
            environment = self.environment(root)
            try:
                environment.start()
                for pile in environment.topology.pile_names:
                    environment.control("/fault", {"target": pile, "op": "storage", "piles": ["pile2"]})
                environment.controller.mode("Auto")
                wait(lambda: environment.controller.state()["controller"]["requested_mode"] == "Auto", "Auto requested mode")
                state = environment.controller.state()
                self.assertFalse(state["evidence"]["qualified"])
                self.assertFalse(state["controller"]["autonomy_ready"])
                self.assertEqual(environment.mutation_events(), [])
                environment.controller.mode("Manual")
                environment.workflow({"action": "failover", "targets": ["pile2"], "successor": None})
            finally:
                environment.close()
