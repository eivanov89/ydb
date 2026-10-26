"""Named I-09 assertions shared by mock and explicitly supplied real fixtures.

The fixture adapter owns preparation/failure injection. Assertions always read
the public Bridge wire; mock-only state inspection is additional evidence.
"""
import grpc

from ydb.apps.bridge_skipper.acceptance.lib.harness import wait
from ydb.apps.bridge_skipper.ydbd_mock.lib import model, operations
from ydb.public.api.grpc.draft import ydb_bridge_v1_pb2_grpc
from ydb.public.api.protos.draft import ydb_bridge_pb2 as B


LOCAL_CASES = ("omitted_internal_state", "batch_disconnect", "implicit_generation_rejection", "exact_enum_error",
               "abandoned_promoted_demotion", "add_remove_completion_generation_errors", "rejoin_empty_implicit_names",
               "listener_closure")


class Contracts:
    def __init__(self, endpoints, names, fixture, channel_factory=None):
        if len(endpoints) < 3 or len(names) != 3 or len(set(names)) != 3 or "" not in names:
            raise ValueError("shared local contracts require three exact named piles including empty")
        self.names = list(names)
        self.endpoints = list(endpoints)
        self.fixture = fixture
        self.fixture_evidence = []
        self.channels = [(channel_factory or grpc.insecure_channel)(endpoint) for endpoint in endpoints]
        self.stubs = [ydb_bridge_v1_pb2_grpc.BridgeServiceStub(channel) for channel in self.channels]

    def close(self):
        for channel in self.channels:
            channel.close()

    def call(self, method, request, source=0, status=model.SUCCESS):
        response = getattr(self.stubs[source], method)(request, timeout=10)
        assert response.operation.status == status, response.operation
        return response.operation

    def state(self, source=0):
        return operations.unpack(self.call("GetClusterState", B.GetClusterStateRequest(), source),
                                 B.GetClusterStateResult)

    def states(self, source=0):
        return {pile.pile_name: pile.state for pile in self.state(source).pile_states}

    def update(self, updates, source=0, status=model.SUCCESS):
        req = B.UpdateClusterStateRequest()
        for name, value in updates.items():
            req.updates.add(pile_name=name, state=value)
        return self.call("UpdateClusterState", req, source, status)

    def run(self, case):
        if case not in LOCAL_CASES:
            raise ValueError("unknown shared contract")
        self.fixture("prepare", {"case": case})
        getattr(self, case)()
        state = self.state()
        return {"generation": str(state.generation), "states": {name: model.public_name(value)
                                                                 for name, value in self.states().items()},
                "fixture_evidence": self.fixture_evidence}

    def exact_enum_error(self):
        before = self.state().generation
        op = self.update({self.names[1]: 999}, status=model.INTERNAL_ERROR)
        assert operations.issue_texts(op) == ["Unsupported pile state"], op
        assert self.state().generation == before

    def batch_disconnect(self):
        before = self.state().generation
        self.update({self.names[1]: model.PUB_DISCONNECTED, self.names[2]: model.PUB_DISCONNECTED})
        assert self.states() == {self.names[0]: model.PUB_PRIMARY, self.names[1]: model.PUB_DISCONNECTED,
                                 self.names[2]: model.PUB_DISCONNECTED}
        assert self.state().generation == before + 1

    def omitted_internal_state(self):
        target = self.names[2]
        self.update({target: model.PUB_DISCONNECTED})
        self.fixture("followups", {"hold": ["sync_step"]})
        self.update({target: model.PUB_NOT_SYNCHRONIZED})
        def internal_two():
            # Existing public Bridge state intentionally projects _1 and _2
            # together. The test fixture supplies separately scoped internal
            # state evidence; this is never presented as a public API signal.
            evidence = self.fixture("await_internal_state", {"pile": target, "stored": "NOT_SYNCHRONIZED_2"})
            assert evidence.get("scope") and evidence.get("pile") == target, evidence
            assert evidence["stored"] == "NOT_SYNCHRONIZED_2", evidence
            assert evidence["requires_data_quorum"] and evidence["requires_config_quorum"], evidence
            self.fixture_evidence.append(evidence)
        internal_two()
        assert self.states()[target] == model.PUB_NOT_SYNCHRONIZED
        self.update({self.names[1]: model.PUB_DISCONNECTED})
        internal_two()
        assert self.states()[target] == model.PUB_NOT_SYNCHRONIZED

    def implicit_generation_rejection(self):
        self.fixture("stale_receiver", {"source": self.names[1]})
        authoritative = self.state()
        try:
            op = self.update({self.names[2]: model.PUB_DISCONNECTED}, source=1, status=model.INTERNAL_ERROR)
            assert any("New cluster state generation#" in issue for issue in operations.issue_texts(op)), op
            assert self.state() == authoritative
        finally:
            self.fixture("restore_receiver", {"source": self.names[1]})

    def abandoned_promoted_demotion(self):
        self.fixture("followups", {"hold": ["promote"]})
        self.update({self.names[2]: model.PUB_PROMOTED})
        assert self.states()[self.names[2]] == model.PUB_PROMOTED
        self.update({self.names[0]: model.PUB_DISCONNECTED, self.names[1]: model.PUB_PRIMARY,
                     self.names[2]: model.PUB_SYNCHRONIZED})
        assert self.states(source=1) == {self.names[0]: model.PUB_DISCONNECTED, self.names[1]: model.PUB_PRIMARY,
                                         self.names[2]: model.PUB_SYNCHRONIZED}
        # The original endpoint closes. Preserve a live source for final output.
        self.stubs[0], self.stubs[1] = self.stubs[1], self.stubs[0]

    def add_remove_completion_generation_errors(self):
        target = ""
        assert self.states()[target] == model.PUB_INACTIVE
        self.fixture("followups", {"error_reason": "conformance allocation error"})
        self.call("AddPile", B.AddPileRequest(pile_name=target), status=model.GENERIC_ERROR)
        pending = self.state()
        assert pending.HasField("pending_pile_operation")
        assert pending.pending_pile_operation.pile_name == target
        assert pending.pending_pile_operation.kind == B.PendingPileOperation.ADD
        assert pending.pending_pile_operation.error_reason == "conformance allocation error"
        generation = pending.generation
        self.fixture("followups", {"error_reason": None})
        wait(lambda: not self.state().HasField("pending_pile_operation"), "Add completion")
        assert self.state().generation == generation and self.states()[target] == model.PUB_DISCONNECTED
        self.fixture("followups", {"hold": ["remove"]})
        future = self.stubs[0].RemovePile.future(B.RemovePileRequest(pile_name=target), timeout=10)
        wait(lambda: self.state().HasField("pending_pile_operation"), "Remove pending")
        assert self.state().generation == generation
        self.fixture("followups", {"release": ["remove"]})
        assert future.result().operation.status == model.SUCCESS
        assert self.states()[target] == model.PUB_INACTIVE and self.state().generation == generation + 1

    def rejoin_empty_implicit_names(self):
        target = ""
        self.update({target: model.PUB_DISCONNECTED})
        self.update({target: model.PUB_NOT_SYNCHRONIZED})
        wait(lambda: self.states()[target] == model.PUB_SYNCHRONIZED, "empty-name rejoin")
        assert any(p.pile_name == target for p in self.state().pile_states)

    def listener_closure(self):
        self.update({self.names[2]: model.PUB_DISCONNECTED})
        def refused():
            try:
                self.stubs[2].GetClusterState(B.GetClusterStateRequest(), timeout=0.3)
            except grpc.RpcError as error:
                return error.code() in (grpc.StatusCode.UNAVAILABLE, grpc.StatusCode.DEADLINE_EXCEEDED)
            return False
        wait(refused, "disconnected gRPC listener closure")
        assert self.state().pile_states


class MockFixture:
    def __init__(self, environment):
        self.environment = environment
        self.stale_authority = None

    def __call__(self, stage, body):
        env = self.environment
        if stage == "prepare":
            env.control("/reset", {})
            if body["case"] == "add_remove_completion_generation_errors":
                env.control("/topology", {"configured_piles": [env.topology.pile_names[0]], "bridged": False})
                endpoint = env.manifest["nodes"][0]["dial_endpoint"]
                with grpc.insecure_channel(endpoint) as channel:
                    stub = ydb_bridge_v1_pb2_grpc.BridgeServiceStub(channel)
                    response = stub.ConvertToBridge(B.ConvertToBridgeRequest(), timeout=10)
                    assert response.operation.status == model.SUCCESS
                    wait(lambda: not operations.unpack(stub.GetClusterState(B.GetClusterStateRequest()).operation,
                                                       B.GetClusterStateResult).conversion_in_progress, "conversion setup")
                env.control("/topology", {"configured_piles": env.topology.pile_names})
        elif stage == "followups":
            env.control("/followups", body)
        elif stage == "stale_receiver":
            # A detached proposer rejects its own prepare, so it cannot test a
            # generation rejection. Instead revive a stale connected replica
            # of a publicly disconnected pile. Authoritative state then forbids
            # pulling it back in, matching the mock's stale-revival contract.
            endpoint = env.manifest["nodes"][0]["dial_endpoint"]
            with grpc.insecure_channel(endpoint) as channel:
                stub = ydb_bridge_v1_pb2_grpc.BridgeServiceStub(channel)
                update = B.UpdateClusterStateRequest()
                update.updates.add(pile_name=body["source"], state=model.PUB_DISCONNECTED)
                response = stub.UpdateClusterState(update, timeout=10)
                assert response.operation.status == model.SUCCESS, response.operation
            self.stale_authority = json_state(env, env.topology.pile_names[0])
            generation = self.stale_authority["generation"] - 2
            assert generation >= 1, self.stale_authority
            replacement = {"pile": body["source"], "generation": generation,
                           "states": {body["source"]: "SYNCHRONIZED"}, "detach": False}
            env.control("/replica", replacement)
            stale = json_state(env, body["source"])
            assert stale["generation"] == generation and not stale["detached"], stale
            endpoint = next(node["dial_endpoint"] for node in env.manifest["nodes"] if node["pile"] == body["source"])
            with grpc.insecure_channel(endpoint) as channel:
                stub = ydb_bridge_v1_pb2_grpc.BridgeServiceStub(channel)

                def stale_listener_ready():
                    try:
                        response = stub.GetClusterState(B.GetClusterStateRequest(), timeout=0.3)
                    except grpc.RpcError:
                        return False
                    return (response.operation.status == model.SUCCESS
                            and operations.unpack(response.operation, B.GetClusterStateResult).generation == generation)

                wait(stale_listener_ready, "stale receiver listener and exact generation", timeout=5)
        elif stage == "restore_receiver":
            assert self.stale_authority is not None
            state = self.stale_authority
            replacement = {"pile": body["source"], "generation": state["generation"],
                           "states": {pile["name"]: pile["public"] for pile in state["piles"]}, "detach": False}
            env.control("/replica", replacement)
            restored = json_state(env, body["source"])
            assert restored["generation"] == state["generation"] and not restored["detached"], restored
            assert {pile["name"]: pile["public"] for pile in restored["piles"]} == {
                pile["name"]: pile["public"] for pile in state["piles"]}
        elif stage == "await_internal_state":
            def observed():
                state = json_state(env, env.topology.pile_names[0])
                return next((pile for pile in state["piles"] if pile["name"] == body["pile"]
                             and pile["stored"] == body["stored"]), None)
            pile = wait(observed, "mock fixture internal state %s" % body)
            stored = model.Stored[pile["stored"]]
            return {"scope": "mock_internal_fixture_only", "pile": pile["name"], "stored": pile["stored"],
                    "requires_data_quorum": model.requires_data_quorum(stored),
                    "requires_config_quorum": model.requires_config_quorum(stored)}
        else:
            raise ValueError("unknown fixture stage")


def json_state(environment, pile):
    import json
    return json.loads(environment.ctl("state", pile))
