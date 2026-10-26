"""Versioned evidence report. Mock success never qualifies production Auto."""
import datetime
import hashlib
import json
import os
import platform
import time


CONTRACT_CASES = (
    "omitted_internal_state", "batch_disconnect", "implicit_generation_rejection", "exact_enum_error",
    "abandoned_promoted_demotion", "add_remove_completion_generation_errors", "rejoin_empty_implicit_names",
    "listener_closure", "certificate_authentication", "primary_loss_static_evidence", "failed_internal_two",
)


def file_evidence(path):
    with open(path, "rb") as source:
        content = source.read()
    return {"path": os.path.abspath(path), "sha256": hashlib.sha256(content).hexdigest(), "bytes": len(content)}


class Report:
    def __init__(self, mode, build, commands, topology=None, fixtures=None):
        if mode not in ("mock", "real") or not build:
            raise ValueError("explicit mock/real mode and supported-build identity required")
        if mode == "real" and not fixtures:
            raise ValueError("real execution requires user-supplied fixture provenance")
        self.value = {
            "schema_version": 1, "kind": "skipper_acceptance", "execution_mode": mode,
            "started_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "build": build, "commands": commands, "hardware": {"platform": platform.platform(),
                                                                  "machine": platform.machine(), "cpus": os.cpu_count()},
            "topology": topology or {}, "fixtures": fixtures or [], "cases": [], "metrics": {},
            "versioned_differences": [],
            "production_auto_qualified": False,
            "external_gates": ["release-pinned real YDB contract executions", "independent application reads/writes",
                               "diagnostic load, memory, CPU and latency measurements", "operator deployment acceptance"],
        }

    def run(self, case_id, action, failure_injection=None):
        started = time.monotonic()
        entry = {"id": case_id, "failure_injection": failure_injection, "status": "failed"}
        self.value["cases"].append(entry)
        try:
            entry["evidence"] = action()
            entry["status"] = "passed"
            return entry["evidence"]
        except BaseException as error:
            entry["error"] = "%s: %s" % (type(error).__name__, error)
            raise
        finally:
            entry["duration_seconds"] = time.monotonic() - started

    def skip(self, case_id, reason):
        self.value["cases"].append({"id": case_id, "status": "not_run", "reason": reason})

    def write(self, path):
        self.value["finished_at"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        self.value["passed"] = (any(c["status"] == "passed" for c in self.value["cases"])
                                and all(c["status"] != "failed" for c in self.value["cases"])
                                and not self.value.get("execution_error"))
        with open(path, "w") as output:
            json.dump(self.value, output, indent=2, sort_keys=True)
            output.write("\n")
