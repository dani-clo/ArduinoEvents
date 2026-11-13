"""Check target selection and reporting without downloading toolchains."""

from argparse import Namespace
from contextlib import redirect_stderr
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import arduino_ci as ci


class ArduinoCITests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        for name in ("EventBasics", "TimersBasics", "RunAsyncBasics",
                     "RunAsyncTimeoutCancel", "StateMachineWithEvents", "AsyncHttpConnect"):
            sketch = root / "examples" / name / f"{name}.ino"
            sketch.parent.mkdir(parents=True)
            sketch.touch()
        (root / "examples/EventBasics/ExtraTab.ino").touch()
        patcher = patch.object(ci, "ROOT", root)
        patcher.start()
        self.addCleanup(patcher.stop)

    def load(self, config):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.json"
            path.write_text(json.dumps(config))
            return ci.load_config(path)

    def config(self):
        return {"versions": ["1.0.0"], "targets": [
            {"id": "test", "fqbn": "arduino:zephyr_main:giga",
             "examples": {"include": ["*"], "exclude": ["AsyncHttp*"]}}]}

    def test_latest_is_always_added_and_duplicates_are_removed(self):
        config = self.config()
        config["versions"] = ["1.0.0", "1.0.0", "latest"]
        self.assertEqual(ci.make_matrix(self.load(config)), {"include": [
            {"target": "test", "version": "1.0.0"},
            {"target": "test", "version": "latest"}]})

    def test_wildcards_and_exclusions(self):
        target = self.load(self.config())["targets"][0]
        self.assertEqual(len(target["sketches"]), 5)
        self.assertNotIn("AsyncHttpConnect", target["sketches"])
        config = self.config()
        config["targets"][0]["examples"] = {"include": ["RunAsync*"]}
        self.assertEqual(self.load(config)["targets"][0]["sketches"],
                         ["RunAsyncBasics", "RunAsyncTimeoutCancel"])

    def test_target_can_override_versions_but_still_gets_latest(self):
        config = self.config()
        config["targets"][0]["versions"] = ["1.1.0"]
        self.assertEqual(self.load(config)["targets"][0]["versions"], ["1.1.0", "latest"])

    def test_typos_empty_selections_and_duplicate_ids_are_rejected(self):
        for selection in ({"include": ["Missing*"]}, {"exclude": ["Typo"]},
                          {"exclude": ["*"]}):
            config = self.config()
            config["targets"][0]["examples"] = selection
            with self.assertRaises(ValueError):
                self.load(config)
        config = self.config()
        config["targets"] *= 2
        with self.assertRaises(ValueError):
            self.load(config)

    def test_compile_failure_does_not_skip_other_examples_and_writes_report(self):
        config = self.load(self.config())
        config["targets"][0]["sketches"] = ["EventBasics", "TimersBasics"]
        installed = json.dumps({"platforms": [{"id": "arduino:zephyr_main",
                                               "installed_version": "1.0.0"}]})
        with tempfile.TemporaryDirectory() as directory:
            args = Namespace(target="test", version="1.0.0", skip_install=True,
                             report_dir=Path(directory), build_dir=Path(directory) / "build")
            with patch.object(ci.subprocess, "check_output", return_value=installed), \
                    patch.object(ci, "run_logged", side_effect=[1, 0]) as run:
                self.assertEqual(ci.build(config, args), 1)
            self.assertEqual(run.call_count, 2)
            report = json.loads((Path(directory) / "report.json").read_text())
            self.assertEqual([e["exit_code"] for e in report["examples"]], [1, 0])
            self.assertEqual(report["resolved_version"], "1.0.0")
            self.assertIn("FAIL", (Path(directory) / "summary.md").read_text())

    def test_install_failure_also_writes_report(self):
        with tempfile.TemporaryDirectory() as directory:
            args = Namespace(target="test", version="1.0.0", skip_install=False,
                             report_dir=Path(directory), build_dir=Path(directory) / "build")
            with patch.object(ci, "run_logged", return_value=1), redirect_stderr(io.StringIO()):
                self.assertEqual(ci.build(self.load(self.config()), args), 1)
            report = json.loads((Path(directory) / "report.json").read_text())
            self.assertEqual(report["status"], "failed")
            self.assertIn("index", report["error"])

    def test_wrong_installed_version_cannot_pass(self):
        installed = json.dumps({"platforms": [{"id": "arduino:zephyr_main",
            "installed_version": "0.90.0", "latest_version": "1.1.0"}]})
        for version in ("1.0.0", "latest"):
            with tempfile.TemporaryDirectory() as directory:
                args = Namespace(target="test", version=version, skip_install=True,
                                 report_dir=Path(directory), build_dir=Path(directory) / "build")
                with patch.object(ci.subprocess, "check_output", return_value=installed), \
                        patch.object(ci, "run_logged") as run, redirect_stderr(io.StringIO()):
                    self.assertEqual(ci.build(self.load(self.config()), args), 1)
                run.assert_not_called()
                report = json.loads((Path(directory) / "report.json").read_text())
                self.assertIn("Expected arduino:zephyr_main@", report["error"])

    def test_pinned_and_latest_install_specs(self):
        installed = json.dumps({"platforms": [{"id": "arduino:zephyr_main",
            "installed_version": "1.0.0", "latest_version": "1.0.0"}]})
        for version, spec in [("1.0.0", "arduino:zephyr_main@1.0.0"),
                              ("latest", "arduino:zephyr_main")]:
            with tempfile.TemporaryDirectory() as directory:
                args = Namespace(target="test", version=version, skip_install=False,
                                 report_dir=Path(directory), build_dir=Path(directory) / "build")
                with patch.object(ci.subprocess, "check_output", return_value=installed), \
                        patch.object(ci, "run_logged", return_value=0) as run:
                    self.assertEqual(ci.build(self.load(self.config()), args), 0)
                self.assertEqual(run.call_args_list[1].args[0],
                                 ["arduino-cli", "core", "install", spec])


if __name__ == "__main__":
    unittest.main()
