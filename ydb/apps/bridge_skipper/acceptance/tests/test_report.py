import json
import os
import tempfile
import unittest

from ydb.apps.bridge_skipper.acceptance.lib.report import Report, file_evidence


class ReportTest(unittest.TestCase):
    def test_execution_provenance_and_external_gates_survive_failure(self):
        with tempfile.TemporaryDirectory() as root:
            fixture = os.path.join(root, "fixture.json")
            with open(fixture, "w") as output:
                output.write('{"pile_name":""}')
            provenance = file_evidence(fixture)
            report = Report("real", {"id": "explicit-supported-build"}, [["explicit-command"]], fixtures=[provenance])
            self.assertEqual(report.run("exact_empty_identity", lambda: {"pile_name": ""}), {"pile_name": ""})
            report.skip("primary_loss", "no real failure recording executed")
            with self.assertRaisesRegex(AssertionError, "failed assertion"):
                report.run("failed", lambda: self.fail("failed assertion"))
            path = os.path.join(root, "report.json")
            report.write(path)
            with open(path) as source:
                saved = json.load(source)
            self.assertEqual(saved["execution_mode"], "real")
            self.assertFalse(saved["passed"])
            self.assertFalse(saved["production_auto_qualified"])
            self.assertEqual([case["status"] for case in saved["cases"]], ["passed", "not_run", "failed"])
            self.assertEqual(saved["fixtures"][0]["sha256"], provenance["sha256"])
            self.assertTrue(saved["external_gates"])

    def test_real_mode_cannot_omit_recording_provenance(self):
        with self.assertRaises(ValueError):
            Report("real", {"id": "build"}, [])
