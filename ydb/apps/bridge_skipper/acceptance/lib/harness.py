"""Run the actual C++ SDK controller against direct mock or supplied real endpoints.

Mock ownership is per-test and uses the standalone supervisor executable. Real
mode owns only Skipper; cluster setup and fixture capture remain external.
"""
import http.client
import json
import os
import shutil
import socket
import subprocess
import time
import urllib.error
import urllib.request
import uuid

import yaml

from ydb.apps.bridge_skipper.ydbd_mock.lib.topology import Policies, Timings, Topology


MUTATIONS = ("UpdateClusterState", "ConvertToBridge", "AddPile", "RemovePile")


def wait(predicate, message, timeout=40, diagnostic=None):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            last = predicate()
        except (OSError, ValueError) as error:
            last = error
        if last and not isinstance(last, BaseException):
            return last
        time.sleep(0.05)
    detail = ""
    if diagnostic:
        try:
            snapshot = diagnostic()
        except Exception as error:
            snapshot = {"diagnostic_error": "%s: %s" % (type(error).__name__, error)}
        detail = "; diagnostic=" + json.dumps(snapshot, sort_keys=True, default=str)[:16000]
    raise AssertionError("timed out waiting for %s; last=%r%s" % (message, last, detail))


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def request(url, body=None):
    payload = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(url, data=payload, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=15) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as error:
        return error.code, json.load(error)


class Controller:
    def __init__(self, binary, config_path, root, artifact_root=None):
        self.binary = binary
        self.config_path = config_path
        with open(config_path) as source:
            self.config = yaml.safe_load(source)
        self.address = self.config["http"]["listen"]
        self.url = "http://" + self.address
        self.root = root
        self.process = None
        self.output = None
        self.last_state = None
        self.last_state_saved = 0
        self.artifact_root = artifact_root
        self.last_plan = None
        self.last_operation = None

    def start(self):
        self.output = open(os.path.join(self.root, "controller.log"), "ab")
        self.process = subprocess.Popen([self.binary, "--config", self.config_path, "--state-dir",
                                         os.path.join(self.root, "state"), "--member-id", "acceptance"],
                                        stdout=self.output, stderr=subprocess.STDOUT)
        wait(lambda: self.process.poll() is None and self.state().get("controller"), "controller listener",
             diagnostic=lambda: {"process_exit_code": self.process.poll(), "state": self.last_state})
        return self

    def stop(self):
        if self.process is not None:
            if self.process.poll() is None:
                try:
                    self.state(force=True)
                except (OSError, ValueError, AssertionError):
                    pass
            self.process.terminate()
            try:
                self.process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        if self.output:
            self.output.close()

    def get(self, path):
        status, body = request(self.url + path)
        if status != 200:
            raise AssertionError((path, status, body))
        return body

    def post(self, path, body, allowed=(200, 202)):
        status, result = request(self.url + path, body)
        if status not in allowed:
            raise AssertionError((path, status, result))
        return result

    def state(self, force=False):
        self.last_state = self.get("/api/state")
        now = time.monotonic()
        if force or now - self.last_state_saved >= 1:
            self.last_state_saved = now
            for directory in {self.root, self.artifact_root} - {None}:
                os.makedirs(directory, exist_ok=True)
                with open(os.path.join(directory, "controller_state.json"), "w") as output:
                    json.dump(self.last_state, output, indent=2, sort_keys=True)
        return self.last_state

    def mode(self, mode="Manual"):
        state = self.state()
        return self.post("/api/mode", {
            "request_id": str(uuid.uuid4()), "mode": mode, "controller_revision": state["controller_revision"],
            "reason": "explicit acceptance workflow; production Auto remains unqualified",
            "ownership_acknowledgement": {"exclusive_administration": True, "independent_failure_domain": True,
                                          "deployment_plan_reference": "explicit-acceptance-fixture"}})

    def plan(self, action):
        return wait(lambda: self._dispatchable_plan(action), "dispatchable %s plan" % action,
                    diagnostic=lambda: {"plan": self.last_plan, "controller_state": self.state()})

    def _dispatchable_plan(self, action):
        status, plan = request(self.url + "/api/plans", action)
        self.last_plan = {"status": status, "body": plan}
        return plan if status == 200 and plan.get("dispatchable") else None

    def submit(self, action, discard_response=False):
        plan = self.plan(action)
        remove = action["action"] == "remove_pile"
        body = {"request_id": str(uuid.uuid4()), "preview": plan["preview"], "acknowledgements": {
            "confirmed": plan["required_acknowledgements"], "typed_pile_name": action["target"] if remove else None,
            "empty_name": remove and action["target"] == "", "deletion": remove}}
        if discard_response:
            # Deliberately lose the HTTP admission reply after the server has
            # answered, retaining the exact UUID and body for reconciliation.
            connection = http.client.HTTPConnection(self.address, timeout=15)
            try:
                connection.request("POST", "/api/operations", json.dumps(body), {"Content-Type": "application/json"})
                connection.getresponse()
            finally:
                connection.close()
            receipt = wait(lambda: self.receipt(body["request_id"]), "lost-admission receipt")
        else:
            receipt = self.post("/api/operations", body)
            receipt = wait(lambda: self.receipt(body["request_id"]), "durable admission receipt")
        assert receipt["status"] == "committed", receipt
        # Receipt lookup/replay precedes staleness checks and must keep identity.
        duplicate = self.post("/api/operations", body)
        assert duplicate["operation_id"] == receipt["operation_id"], (duplicate, receipt)
        return receipt["operation_id"], body

    def receipt(self, request_id):
        status, receipt = request(self.url + "/api/requests/" + request_id)
        if status in (202, 404):
            return None
        assert status == 200, (status, receipt)
        return receipt if receipt.get("status") in ("committed", "rejected") else None

    def operation(self, operation_id):
        self.last_operation = self.get("/api/operations/" + operation_id)["operation"]
        if self.artifact_root:
            os.makedirs(self.artifact_root, exist_ok=True)
            with open(os.path.join(self.artifact_root, "operation_%s.json" % operation_id), "w") as output:
                json.dump(self.last_operation, output, indent=2, sort_keys=True)
        return self.last_operation

    def completed(self, operation_id, timeout=40):
        def check():
            operation = self.operation(operation_id)
            if operation["status"] in ("rejected", "needs_attention", "externally_resolved"):
                raise AssertionError(operation)
            return operation if operation["status"] == "succeeded" else None
        return wait(check, "successful operation " + operation_id, timeout,
                    diagnostic=lambda: {"operation": self.last_operation, "controller_state": self.state()})


class MockEnvironment:
    def __init__(self, mock_binary, controller_binary, root, names=("pile1", "pile2", ""), initial=None,
                 artifact_root=None):
        self.mock_binary = mock_binary
        self.root = root
        self.artifact_root = artifact_root
        self.run_dir = os.path.join(root, "mock")
        self.env = dict(os.environ, YDBD_MOCK_EXE=mock_binary, PYTHONDONTWRITEBYTECODE="1")
        self.topology = Topology.with_free_ports(list(names), 3, policies=Policies(),
                                               timings=Timings(lifecycle_delay=0.1, promote_delay=0.1,
                                                               suspend_delay=0.1, connect_delay=0.1,
                                                               sync_step_delay=0.1), initial=initial)
        self.topology_path = os.path.join(root, "topology.yaml")
        self.topology.save(self.topology_path)
        self.controller_binary = controller_binary
        self.controller = None
        self.supervisor = None
        self.supervisor_output = None

    def ctl(self, *args):
        result = subprocess.run([self.mock_binary, "cluster", "--run-dir", self.run_dir] + list(args),
                                env=self.env, capture_output=True, text=True, timeout=45)
        if result.returncode:
            raise AssertionError((args, result.returncode, result.stdout, result.stderr))
        return result.stdout

    def start(self):
        self.ctl("up", "--topology", self.topology_path, "--fresh")
        self.manifest = json.loads(self.ctl("manifest"))
        supervisor_port = free_port()
        self.supervisor_url = "http://127.0.0.1:%d/api/v1" % supervisor_port
        self.supervisor_output = open(os.path.join(self.root, "supervisor.log"), "ab")
        self.supervisor = subprocess.Popen([self.mock_binary, "cluster", "--run-dir", self.run_dir,
                                           "serve", "--host", "127.0.0.1", "--port", str(supervisor_port)],
                                          env=self.env, stdout=self.supervisor_output, stderr=subprocess.STDOUT)
        wait(lambda: request(self.supervisor_url + "/manifest")[0] == 200, "supervisor manifest")
        endpoints = [("grpcs://" if node["tls"] else "grpc://") + node["dial_endpoint"]
                     for node in self.manifest["nodes"]]
        config = {"schema_version": 1, "cluster_id": self.manifest["cluster_id"],
                  "controller": {"authority_backend": "local"}, "ydb_endpoints": endpoints,
                  "http": {"listen": "127.0.0.1:%d" % free_port()},
                  "evidence": {"profile": "global_bsc_v1"},
                  "polling": {"config_interval_seconds": 1},
                  "operations": {"mutation_client_timeout_seconds": 2, "membership_client_timeout_seconds": 2}}
        config_path = os.path.join(self.root, "skipper.yaml")
        with open(config_path, "w") as output:
            yaml.safe_dump(config, output)
        self.controller = Controller(self.controller_binary, config_path, self.root, self.artifact_root)
        self.controller.start()
        wait(lambda: self.controller.state()["cluster"]["topology_status"] == "confirmed", "confirmed topology",
             diagnostic=lambda: self.controller.last_state)
        self.controller.mode()
        wait(lambda: self.controller.state()["controller"]["requested_mode"] == "Manual", "Manual ownership",
             diagnostic=lambda: self.controller.last_state)
        return self

    def close(self):
        errors = []
        try:
            if self.controller:
                self.controller.stop()
        except Exception as error:
            errors.append(error)
        try:
            if self.supervisor:
                self.supervisor.terminate()
                try:
                    self.supervisor.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    self.supervisor.kill()
                    self.supervisor.wait(timeout=5)
            if self.supervisor_output:
                self.supervisor_output.close()
        except Exception as error:
            errors.append(error)
        try:
            self.ctl("down")
        except Exception as error:
            errors.append(error)
        if self.artifact_root:
            os.makedirs(self.artifact_root, exist_ok=True)
            for name in ("controller.log", "controller_state.json", "supervisor.log", "topology.yaml"):
                path = os.path.join(self.root, name)
                if os.path.isfile(path):
                    shutil.copy2(path, os.path.join(self.artifact_root, name))
            logs = os.path.join(self.run_dir, "logs")
            if os.path.isdir(logs):
                shutil.copytree(logs, os.path.join(self.artifact_root, "mock_logs"), dirs_exist_ok=True)
            for name in ("endpoint_manifest.json", "cluster.json"):
                path = os.path.join(self.run_dir, name)
                if os.path.isfile(path):
                    shutil.copy2(path, os.path.join(self.artifact_root, name))
        if errors:
            raise errors[0]

    def control(self, path, body):
        status, result = request(self.supervisor_url + path, body)
        assert status == 200, (path, status, result)
        return result

    def events(self):
        return [json.loads(line) for line in self.ctl("events").splitlines() if line.strip()]

    def mutation_events(self, after=0):
        return [event for event in self.events() if event["kind"] == "rpc.start" and event.get("ts", 0) >= after
                and event.get("method", "").rsplit("/", 1)[-1] in MUTATIONS]

    def vector(self, source="pile1"):
        state = json.loads(self.ctl("state", source))
        return {pile["name"]: pile["public"] for pile in state["piles"]}

    def workflow(self, action, discard_response=False, retained_rejoin=False):
        before = {event["rpc_id"] for event in self.mutation_events()}
        operation_id, body = self.controller.submit(action, discard_response)
        if retained_rejoin:
            target = action["target"]
            wait(lambda: self.vector().get(target) == "SYNCHRONIZED", "mock rejoin synchronization")
            operation = wait(lambda: self.controller.operation(operation_id)
                             if self.controller.operation(operation_id)["background_work"] else None,
                             "retained unqualified rejoin obligation")
            assert operation["status"] != "succeeded", operation
            assert any(not work["complete"] for work in operation["background_work"]), operation
        else:
            operation = self.controller.completed(operation_id)
        events = [event for event in self.mutation_events() if event["rpc_id"] not in before]
        assert len(events) == len(operation["steps"]), (operation, events)
        return {"operation_id": operation_id, "request_id": body["request_id"], "operation": operation,
                "mutation_rpc_ids": [event["rpc_id"] for event in events], "replica": self.vector()}
