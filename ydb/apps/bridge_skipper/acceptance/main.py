"""Explicit mock/real conformance recorder. Never claims an unexecuted real run."""
import argparse
import json
import os
import sys
import tempfile

import grpc
from google.protobuf import json_format

from ydb.apps.bridge_skipper.acceptance.lib.harness import Controller, MockEnvironment, request
from ydb.apps.bridge_skipper.acceptance.lib.contracts import Contracts, LOCAL_CASES, MockFixture
from ydb.apps.bridge_skipper.acceptance.lib.report import CONTRACT_CASES, Report, file_evidence
from ydb.public.api.grpc.draft import ydb_bridge_v1_pb2_grpc
from ydb.public.api.protos.draft import ydb_bridge_pb2 as bridge


def _subset(actual, expected):
    if isinstance(expected, dict):
        assert isinstance(actual, dict), (actual, expected)
        for key, value in expected.items():
            assert key in actual, (key, actual)
            _subset(actual[key], value)
    else:
        assert actual == expected, (actual, expected)


def wire_case(config, case):
    """Same generated-wire assertions for either backend; no implicit retries."""
    endpoint = case["endpoint"]
    if endpoint not in config["ydb_endpoints"]:
        raise ValueError("case endpoint must occur explicitly in supplied config")
    address = endpoint.split("://", 1)[1]
    auth = config.get("ydb_auth", {})
    metadata = []
    if auth.get("token_file"):
        with open(auth["token_file"]) as source:
            metadata.append(("x-ydb-auth-ticket", source.read().strip()))
    if endpoint.startswith("grpcs://"):
        def read(name):
            if not auth.get(name):
                return None
            with open(auth[name], "rb") as source:
                return source.read()
        channel = grpc.secure_channel(address, grpc.ssl_channel_credentials(
            root_certificates=read("ca_file"), private_key=read("client_key_file"),
            certificate_chain=read("client_cert_file")))
    else:
        channel = grpc.insecure_channel(address)
    try:
        method = case["method"]
        if method not in ("GetClusterState", "UpdateClusterState", "ConvertToBridge", "AddPile", "RemovePile"):
            raise ValueError("unsupported contract method")
        request = json_format.ParseDict(case.get("request", {}), getattr(bridge, method + "Request")())
        response = getattr(ydb_bridge_v1_pb2_grpc.BridgeServiceStub(channel), method)(
            request, metadata=metadata, timeout=case.get("timeout_seconds", 10))
        actual = json_format.MessageToDict(response.operation, preserving_proto_field_name=True)
        _subset(actual, case["expect_operation"])
        return {"method": method, "endpoint": endpoint, "operation": actual}
    finally:
        channel.close()


def entry():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", required=True, choices=("mock", "real"))
    parser.add_argument("--controller-binary", required=True)
    parser.add_argument("--mock-binary")
    parser.add_argument("--config", help="required for real mode; explicitly supplied endpoints and credentials")
    parser.add_argument("--fixtures", help="versioned user scenario and fixture provenance manifest")
    parser.add_argument("--build-id", required=True, help="release/source/build identity, never inferred from mock output")
    parser.add_argument("--report", required=True)
    args = parser.parse_args()
    if args.mode == "real" and (not args.config or not args.fixtures):
        parser.error("real mode requires --config and --fixtures; cluster provision and failure injection are external")
    if args.mode == "mock" and not args.mock_binary:
        parser.error("mock mode requires the repository-built --mock-binary")
    fixture = None
    provenance = []
    if args.fixtures:
        with open(args.fixtures) as source:
            fixture = json.load(source)
        if fixture.get("schema_version") != 1 or not isinstance(fixture.get("cases"), list):
            parser.error("fixtures require schema_version=1 and cases array")
        if args.mode == "real" and (fixture.get("source_build") != args.build_id or not fixture.get("recordings")):
            parser.error("real fixtures require matching source_build and nonempty recordings provenance")
        provenance.append(file_evidence(args.fixtures))
        for path in fixture.get("recordings", []):
            provenance.append(file_evidence(os.path.join(os.path.dirname(args.fixtures), path)))
    report = Report(args.mode, {"id": args.build_id, "controller_binary": file_evidence(args.controller_binary)},
                    [sys.argv], fixtures=provenance)
    environment = None
    controller = None
    try:
        with tempfile.TemporaryDirectory(prefix="skipper_conformance_") as root:
            try:
                if args.mode == "mock":
                    environment = MockEnvironment(args.mock_binary, args.controller_binary, root,
                                                  artifact_root=args.report + ".artifacts")
                    environment.start()
                    controller = environment.controller
                    config = controller.config
                    report.value["topology"] = environment.manifest
                else:
                    controller = Controller(args.controller_binary, args.config, root)
                    controller.start()
                    config = controller.config
                    if fixture.get("endpoints") != config["ydb_endpoints"]:
                        raise ValueError("real fixture endpoint list must exactly match supplied config")
                    report.value["topology"] = {"endpoints": config["ydb_endpoints"],
                                                "config_sha256": file_evidence(args.config)["sha256"]}
                cases = fixture["cases"] if fixture else [{"id": case, "shared_contract": True} for case in LOCAL_CASES]
                executed = set()
                for case in cases:
                    case_id = case["id"]
                    executed.add(case_id)
                    if case.get("shared_contract"):
                        if environment:
                            endpoints = [next(node["dial_endpoint"] for node in environment.manifest["nodes"]
                                              if node["pile"] == name) for name in environment.topology.pile_names]
                            names = environment.topology.pile_names
                            adapter = MockFixture(environment)
                        else:
                            actuator = fixture.get("fixture_control_url")
                            if not actuator or not actuator.startswith(("http://", "https://")):
                                raise ValueError("shared real contracts require explicit fixture_control_url")
                            names = [pile["name"] for pile in fixture["piles"]]
                            uris = [pile["endpoint"] for pile in fixture["piles"]]
                            if any(uri not in config["ydb_endpoints"] for uri in uris):
                                raise ValueError("fixture piles must use configured direct endpoints")
                            endpoints = [uri.split("://", 1)[1] for uri in uris]
                            if any(not uri.startswith("grpc://") for uri in uris):
                                raise ValueError("shared TLS cases require channel adapter; use wire cases for TLS")

                            def adapter(stage, body):
                                status, result = request(actuator.rstrip("/") + "/" + stage, body)
                                assert status == 200 and result.get("ok"), (stage, status, result)
                                return result.get("fixture_evidence", result)
                        contracts = Contracts(endpoints, names, adapter)
                        try:
                            report.run(case_id, lambda: contracts.run(case_id), case.get("failure_injection"))
                        finally:
                            contracts.close()
                    elif "method" in case:
                        report.run(case_id, lambda case=case: wire_case(config, case), case.get("failure_injection"))
                    else:
                        def workflow(case=case):
                            controller.mode("Manual")
                            if environment:
                                return environment.workflow(case["action"], retained_rejoin=case.get("retained_rejoin", False))
                            operation_id, body = controller.submit(case["action"])
                            operation = controller.completed(operation_id, timeout=case.get("timeout_seconds", 120))
                            _subset(operation, case.get("expect_operation", {"status": "succeeded"}))
                            return {"operation": operation, "request_id": body["request_id"]}

                        report.run(case_id, workflow, case.get("failure_injection"))
                for case_id in CONTRACT_CASES:
                    if case_id not in executed:
                        report.skip(case_id, "no matching supplied scenario executed")
            finally:
                if environment:
                    environment.close()
                elif controller:
                    controller.stop()
    except BaseException as error:
        report.value["execution_error"] = "%s: %s" % (type(error).__name__, error)
        raise
    finally:
        report.write(args.report)


if __name__ == "__main__":
    entry()
