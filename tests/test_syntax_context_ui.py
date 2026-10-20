#!/usr/bin/env python3
"""Compare a new, hash-pinned mcpp build with the frozen original context controls."""

import argparse
import hashlib
import json
import os
from pathlib import Path

from terminal_session import TerminalSession
from test_syntax_format_ui import colors_for


BASE = Path("/review/cases/l22-context-5fba688")
EVIDENCE = Path("/review/evidence/session")
PRE_CONTEXT_MCPP_SHA256 = {
    "6adce2cccfe562dc441dc763ce8c233b552c52908d7d602c0537c2a29d214847",
    "e886331864b03066cf2eac1343b2835c7ec8b92a264bfab4982378bb3aa12b5c",
}


def main():
    if (os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file()):
        raise SystemExit("requires uid1001 in the owned disposable container")
    parser = argparse.ArgumentParser()
    parser.add_argument("--mcpp", required=True, type=Path)
    parser.add_argument("--sha256", required=True)
    arguments = parser.parse_args()
    expected = json.loads((EVIDENCE / "l22-context-5fba688-expected.json").read_text())
    checkpoint = json.loads((EVIDENCE / "l22-context-5fba688-checkpoint.json").read_text())
    digest = hashlib.sha256(arguments.mcpp.read_bytes()).hexdigest()
    if digest != arguments.sha256:
        raise SystemExit(f"binary hash differs: {digest}")
    if digest in PRE_CONTEXT_MCPP_SHA256 or digest == expected["native_sha256"]:
        raise SystemExit("requires a new matching mcpp build; native controls are never rerun")
    source = Path("/home/mcpp/src/src/syntax_rules.cpp")
    if hashlib.sha256(source.read_bytes()).hexdigest() != checkpoint["production_source_sha256"]:
        raise SystemExit("syntax source differs from the frozen matcher checkpoint")
    if BASE.is_symlink() or BASE.stat().st_uid != os.geteuid():
        raise SystemExit("fixture base must be owned and not a symlink")
    assert len(expected["cases"]) == 8, "use only the frozen tiny context group"
    fixture_hashes = [case["fixture_sha256"] for case in expected["cases"]]
    result_path = EVIDENCE / f"l22-context-ui-{digest[:16]}.json"
    if result_path.is_file():
        result = json.loads(result_path.read_text())
        assert result["sha256"] == digest and result["fixture_hashes"] == fixture_hashes
        if result["status"] == "PASS":
            print(f"Using cached matching-app context comparison: {result_path}")
            return
    else:
        result = {
            "binary": str(arguments.mcpp), "sha256": digest,
            "syntax_source_sha256": checkpoint["production_source_sha256"],
            "fixture_hashes": fixture_hashes, "status": "running", "cases": [],
        }
    run = BASE / ("ui-" + digest[:16])
    run.mkdir(exist_ok=True)
    assert not run.is_symlink() and run.stat().st_uid == os.geteuid()
    os.environ["LD_LIBRARY_PATH"] = "/review/control/lib"
    completed = {case["name"] for case in result["cases"]}
    for case in expected["cases"]:
        assert hashlib.sha256((case["syntax"] + case["text"]).encode()).hexdigest() == (
            case["fixture_sha256"]
        ), "cached original fixture differs"
        if case["name"] in completed:
            continue
        root = run / (case["name"] + "-" + str(os.getpid()))
        root.mkdir()
        fixture = root / "original.ctx"
        fixture.write_text(case["text"])
        ui = root / "ui"
        config = ui / "config/mcpp"
        config.mkdir(parents=True)
        (config / "Syntax").write_text(case["syntax"])
        (config / "config").write_text(
            "auto_save_setup=false\neditor_line_numbers=false\neditor_syntax=true\n"
        )
        command = [
            "--foreground", "--signal=TERM", "--kill-after=1", "15",
            str(arguments.mcpp.resolve()), "--edit", str(fixture),
        ]
        with TerminalSession("/usr/bin/timeout", "/usr/bin/tmux", ui, command,
                             record_exit=True, record_stderr=True) as terminal:
            terminal.wait_for("sentinel", timeout=4)
            terminal.press("C-End")
            terminal.wait_idle()
            terminal.save("context-end")
            colors = {
                line: colors_for(terminal.text(ansi=True), line)
                for line in case["text"].splitlines()
            }
            assert colors == case["colors"], (
                f"{case['name']} differs from cached native context colors: {colors!r}"
            )
            terminal.press("C-Home")
            terminal.wait_idle()
            terminal.save("context-home")
            # Compare every original byte without the cursor's foreground override.
            terminal.press("C-End")
            terminal.wait_idle()
            terminal.save("context-rewind")
            assert {
                line: colors_for(terminal.text(ansi=True), line)
                for line in case["text"].splitlines()
            } == colors, "rewinding changes context colors"
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
        assert fixture.read_text() == case["text"], "context comparison changed file bytes"
        result["cases"].append({"name": case["name"], "root": str(root), "colors": colors})
        result_path.write_text(json.dumps(result, indent=2) + "\n")
    result["status"] = "PASS"
    result_path.write_text(json.dumps(result, indent=2) + "\n")
    print(f"PASS matching mcpp context colors and rewind against frozen native group: {result_path}")


if __name__ == "__main__":
    main()
