#!/usr/bin/env python3
"""One bounded native MC/mcpp extfs entry, viewer, copy and parent control."""

import argparse
import json
import os
from pathlib import Path
import shutil

from terminal_session import TerminalSession


PAYLOAD = "EXTFS-LOCAL-UI-CONTENTS\n"


def exercise(binary, root, native, tmux="/usr/bin/tmux"):
    left, right, ui = root / "left", root / "right", root / "ui"
    left.mkdir(parents=True)
    right.mkdir()
    log = root / "helper-calls.jsonl"
    source = left / "sample.tiny"
    source.write_text(json.dumps({"nodes": {"data.txt": {"kind": "f", "data": PAYLOAD}}, "log": str(log)}))
    original = source.read_bytes()
    helpers = ui / "data/mc/extfs.d"
    helpers.mkdir(parents=True)
    helper = helpers / "probe"
    shutil.copyfile(Path(__file__).with_name("extfs_fixture.py"), helper)
    helper.chmod(0o700)
    config = ui / "config" / ("mc" if native else "mcpp")
    config.mkdir(parents=True)
    (config / "mc.ext.ini").write_text(
        "[mc.ext.ini]\nVersion=4.0\n[Original fixture]\nShell=.tiny\nOpen=%cd %p/probe://\n"
    )
    (config / ("ini" if native else "config")).write_text(
        ("[Midnight-Commander]\n" if native else "") +
        "use_internal_view=1\nuse_subshell=0\nconfirm_exit=0\npause_after_run=0\n"
    )
    arguments = ["-u", left, right] if native else [left, right]
    uri = str(source) + "/probe://"
    with TerminalSession(binary, tmux, ui, arguments,
                         record_exit=True, record_stderr=True) as terminal:
        terminal.wait_for(source.name)
        terminal.press("Home", "Down", "Enter")
        terminal.wait_for("/probe://")
        terminal.wait_for("data.txt")
        terminal.save("extension-entry")
        terminal.press("Home", "Down", "F3")
        terminal.wait_for(PAYLOAD.strip())
        terminal.save("member-view")
        terminal.press("F10")
        terminal.wait_for(PAYLOAD.strip(), present=False)
        terminal.press("F5")
        terminal.wait_for("Copy")
        terminal.press("Enter")
        terminal.wait_file(right / "data.txt", PAYLOAD.encode())
        terminal.wait_idle()
        terminal.save("member-copy")
        terminal.press("Home", "Enter")
        terminal.wait_for("/probe://", present=False)
        terminal.wait_for(source.name)
        terminal.raw(b"\x1bc")
        terminal.wait_for("Quick cd" if native else "Change directory")
        terminal.type(uri)
        terminal.press("Enter")
        terminal.wait_for("/probe://")
        terminal.wait_for("data.txt")
        terminal.save("quick-cd")
        terminal.press("F10")
        terminal.wait_file(terminal.exit_file, b"0\n")
    assert source.read_bytes() == original, "read-only navigation modified the original container"
    calls = [json.loads(line) for line in log.read_text().splitlines()]
    assert {"list", "copyout"} <= {call[0] for call in calls}, calls
    assert all(call[2] == "data.txt" for call in calls if call[0] == "copyout"), calls
    if not native:
        assert all(call[1] != str(source) for call in calls), calls
    return {"source": str(source), "uri": uri, "calls": calls, "copied": (right / "data.txt").read_text()}


def main():
    if (os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file()):
        raise SystemExit("extfs UI case requires uid1001 in the owned disposable Docker container")
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", help="newly built mcpp executable")
    parser.add_argument("--mcpp-only", action="store_true")
    parser.add_argument("--tmux", default="/usr/bin/tmux")
    arguments = parser.parse_args()
    base = Path("/review/cases/extfs-local-ui")
    base.mkdir(exist_ok=True)
    if base.is_symlink() or base.stat().st_uid != os.geteuid():
        raise SystemExit("fixture base must be owned and not a symlink")
    root = base / ("run-" + str(os.getpid()))
    root.mkdir()
    os.environ["LD_LIBRARY_PATH"] = "/review/control/lib"
    result = {}
    if not arguments.mcpp_only:
        result["native"] = exercise("/review/control/mc", root / "native", True, arguments.tmux)
    result["mcpp"] = exercise(arguments.binary, root / "mcpp", False, arguments.tmux)
    evidence = Path("/review/evidence/session") / ("extfs-local-ui-" + str(os.getpid()) + ".json")
    evidence.write_text(json.dumps(result, indent=2) + "\n")
    prefix = "mcpp" if arguments.mcpp_only else "Native MC 4.8.30 and mcpp"
    print(prefix + ": helper discovery, %cd, Quick cd, viewer, copyout and parent passed")
    print(evidence)


if __name__ == "__main__":
    main()
