"""Generate the CI matrix and compile examples using Arduino CLI (stdlib only)."""

import argparse
from fnmatch import fnmatchcase
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]


def string_list(value, label):
    if not isinstance(value, list) or any(not isinstance(x, str) or not x for x in value):
        raise ValueError(f"{label} must be a list of nonempty strings")
    return value


def load_config(path):
    config = json.loads(path.read_text())
    versions = string_list(config["versions"], "versions")
    targets = config["targets"]
    if not isinstance(targets, list) or not targets:
        raise ValueError("targets must be a nonempty list")
    available = sorted(p.parent.name for p in (ROOT / "examples").glob("*/*.ino")
                       if p.stem == p.parent.name)
    ids = set()
    for target in targets:
        name = target["id"]
        if not re.fullmatch(r"[a-zA-Z0-9_-]+", name) or name in ids:
            raise ValueError(f"Invalid or duplicate target id: {name}")
        ids.add(name)
        if not re.fullmatch(r"[\w-]+:[\w-]+:[\w-]+(?::[^\s]+)?", target["fqbn"]):
            raise ValueError(f"Invalid FQBN for {name}")
        requested = string_list(target.get("versions", versions), f"{name}.versions")
        if any(not re.fullmatch(r"\d+\.\d+\.\d+(?:[-+][\w.-]+)?|latest", v)
               for v in requested):
            raise ValueError(f"Invalid version for {name}")
        target["versions"] = list(dict.fromkeys([*requested, "latest"]))
        selection = target.get("examples", {})
        include = string_list(selection.get("include", ["*"]), f"{name}.include")
        exclude = string_list(selection.get("exclude", []), f"{name}.exclude")
        for pattern in include + exclude:
            if not any(fnmatchcase(example, pattern) for example in available):
                raise ValueError(f"{name}: pattern {pattern!r} matches no examples")
        target["sketches"] = [e for e in available
                              if any(fnmatchcase(e, p) for p in include)
                              and not any(fnmatchcase(e, p) for p in exclude)]
        if not target["sketches"]:
            raise ValueError(f"{name}: no examples selected")
    return config


def make_matrix(config):
    entries = [{"target": t["id"], "version": v}
               for t in config["targets"] for v in t["versions"]]
    if len(entries) > 256:
        raise ValueError("GitHub Actions supports at most 256 matrix jobs")
    return {"include": entries}


def run_logged(command, path):
    print(f"$ {shlex.join(command)}", flush=True)
    with path.open("w") as log:
        log.write(f"$ {shlex.join(command)}\n")
        log.flush()
        with subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True) as process:
            for line in process.stdout:
                print(line, end="", flush=True)
                log.write(line)
                log.flush()
            return process.wait()


def build(config, args):
    target = next((t for t in config["targets"] if t["id"] == args.target), None)
    if target is None or args.version not in target["versions"]:
        raise ValueError("Target/version is not in the configured matrix")
    reports = args.report_dir.resolve()
    reports.mkdir(parents=True, exist_ok=True)
    core = ":".join(target["fqbn"].split(":")[:2])
    report = {"target": target["id"], "fqbn": target["fqbn"], "core": core,
              "requested_version": args.version, "resolved_version": None,
              "status": "failed", "examples": []}
    try:
        if not args.skip_install:
            if run_logged(["arduino-cli", "core", "update-index"], reports / "index.log"):
                raise RuntimeError("Failed to update package index")
            spec = core if args.version == "latest" else f"{core}@{args.version}"
            if run_logged(["arduino-cli", "core", "install", spec], reports / "install.log"):
                raise RuntimeError(f"Failed to install {spec}")
        installed = subprocess.check_output(["arduino-cli", "core", "list", "--json"],
                                            cwd=ROOT, text=True)
        (reports / "cores.json").write_text(installed)
        platform = next(p for p in json.loads(installed)["platforms"] if p["id"] == core)
        actual = platform["installed_version"]
        report["resolved_version"] = actual
        expected = platform["latest_version"] if args.version == "latest" else args.version
        if actual != expected:
            raise RuntimeError(f"Expected {core}@{expected}, found {actual}")
        for sketch in target["sketches"]:
            started = time.monotonic()
            command = ["arduino-cli", "compile", "--fqbn", target["fqbn"],
                       "--library", str(ROOT), "--warnings", "default", "--jobs", "2",
                       "--build-path", str(args.build_dir.resolve() / sketch),
                       str(ROOT / "examples" / sketch)]
            code = run_logged(command, reports / f"{sketch}.log")
            report["examples"].append({"name": sketch, "exit_code": code,
                                       "seconds": round(time.monotonic() - started, 2)})
        if all(e["exit_code"] == 0 for e in report["examples"]):
            report["status"] = "passed"
    except (OSError, RuntimeError, subprocess.SubprocessError, KeyError, StopIteration) as error:
        report["error"] = str(error)
        print(f"Build failed: {error}", file=sys.stderr)
    finally:
        (reports / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        lines = [f"### {target['id']} / {args.version}", "",
                 f"FQBN: `{target['fqbn']}`; resolved core: `{report['resolved_version']}`.",
                 "", "| Example | Result |", "| --- | --- |"]
        lines += [f"| {e['name']} | {'PASS' if e['exit_code'] == 0 else 'FAIL'} |"
                  for e in report["examples"]]
        if "error" in report:
            lines += ["", f"Setup/build error: {report['error']}"]
        summary = "\n".join(lines) + "\n"
        (reports / "summary.md").write_text(summary)
        if os.environ.get("GITHUB_STEP_SUMMARY"):
            with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as output:
                output.write(summary)
    return 0 if report["status"] == "passed" else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=ROOT / ".github/arduino-ci.json")
    commands = parser.add_subparsers(dest="command", required=True)
    matrix = commands.add_parser("matrix")
    matrix.add_argument("--github-output", type=Path)
    compile_parser = commands.add_parser("build")
    compile_parser.add_argument("--target", required=True)
    compile_parser.add_argument("--version", required=True)
    compile_parser.add_argument("--report-dir", type=Path, required=True)
    compile_parser.add_argument("--build-dir", type=Path, required=True)
    compile_parser.add_argument("--skip-install", action="store_true",
                                help="Use an already installed matching core for local checks")
    args = parser.parse_args()
    try:
        config = load_config(args.config)
        if args.command == "build":
            return build(config, args)
        value = json.dumps(make_matrix(config), separators=(",", ":"))
        print(value)
        if args.github_output:
            with args.github_output.open("a") as output:
                output.write(f"matrix={value}\n")
        return 0
    except (ValueError, KeyError, TypeError, OSError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    sys.exit(main())
