#!/usr/bin/env python3
"""One bounded source-less extfs UI case with real directory-origin exits."""

import argparse
import json
import os
from pathlib import Path
import shutil

from terminal_session import TerminalSession


def parent_selects(terminal, directory):
    terminal.press("Home", "Enter")
    terminal.wait_for("localprobe://", present=False)
    terminal.wait(lambda: directory.name in terminal.text().splitlines()[-5],
                  "root parent selects the real mount directory")


def exercise(binary, root, native, tmux):
    left, right, mount, ui = root / "entry-cwd", root / "right", root / "other-mount", root / "ui"
    left.mkdir(parents=True)
    right.mkdir()
    mount.mkdir()
    payload = "SOURCELESS-ENTRY-CONTEXT\n"
    parent_payload = "SOURCELESS-PARENT-CONTEXT\n"
    (left / "context.txt").write_text(payload)
    (left / "open.localset").write_text("original extension trigger\n")
    (root / "context.txt").write_text(parent_payload)
    (mount / "context.txt").write_text("WRONG-MOUNT-CONTEXT\n")
    helpers = ui / "data/mc/extfs.d"
    helpers.mkdir(parents=True)
    helper = helpers / "localprobe+"
    shutil.copyfile(Path(__file__).with_name("extfs_sourceless_fixture.py"), helper)
    helper.chmod(0o700)
    log = root / "helper-calls.jsonl"
    Path(str(helper) + ".json").write_text(json.dumps({
        "log": str(log),
        "nodes": {
            "folder": {"kind": "d"},
            "data.txt": {"kind": "f", "from_working": "context.txt"},
            "folder/child.txt": {"kind": "f", "data": "SOURCELESS-NESTED\n"},
        },
    }))
    config = ui / "config" / ("mc" if native else "mcpp")
    config.mkdir(parents=True)
    (config / "mc.ext.ini").write_text(
        "[mc.ext.ini]\nVersion=4.0\n[Original source-less fixture]\n"
        "Shell=.localset\nOpen=%cd localprobe://\n"
    )
    (config / ("ini" if native else "config")).write_text(
        ("[Midnight-Commander]\n" if native else "") +
        "use_internal_view=1\nuse_subshell=0\nconfirm_exit=0\npause_after_run=0\n"
    )
    arguments = ["-u", left, right] if native else [left, right]
    uri = str(mount) + "/localprobe://folder"
    with TerminalSession(binary, tmux, ui, arguments, record_exit=True, record_stderr=True) as terminal:
        terminal.wait_for("open.localset")
        terminal.press("Home", "Down", "Down", "Enter")
        terminal.wait_for("localprobe://")
        terminal.wait_for("data.txt")
        terminal.save("bare-extension-entry")
        terminal.press("Home", "Down", "Down", "F3")
        terminal.wait_for(payload.strip())
        terminal.save("entry-context-view")
        terminal.press("F10")
        terminal.wait_for(payload.strip(), present=False)
        terminal.press("F5")
        terminal.wait_for("Copy")
        terminal.press("Enter")
        terminal.wait_file(right / "data.txt", payload.encode())
        terminal.wait_idle()
        parent_selects(terminal, left)
        terminal.save("bare-root-parent")
        terminal.raw(b"\x1bc")
        terminal.wait_for("Quick cd" if native else "Change directory")
        terminal.type(uri)
        terminal.press("Enter")
        terminal.wait_for("child.txt")
        terminal.press("Home", "Down", "F3")
        terminal.wait_for("SOURCELESS-NESTED")
        terminal.press("F10")
        terminal.wait_for("SOURCELESS-NESTED", present=False)
        terminal.press("Home", "Enter")
        terminal.wait_for("data.txt")
        terminal.press("Home", "Down", "Down", "F3")
        terminal.wait_for(parent_payload.strip())
        terminal.save("distinct-mount-working-context")
        terminal.press("F10")
        terminal.wait_for(parent_payload.strip(), present=False)
        parent_selects(terminal, mount)
        terminal.save("explicit-root-parent")
        terminal.press("F10")
        terminal.wait_file(terminal.exit_file, b"0\n")
    calls = [json.loads(line) for line in log.read_text().splitlines()]
    assert all(call["argv"] == ["list"] for call in calls if call["argv"][0] == "list"), calls
    copied = [call for call in calls if call["argv"][0] == "copyout"]
    assert copied and all(len(call["argv"]) == 4 for call in copied), calls
    for call in copied:
        directory, member, _ = call["argv"][1:]
        assert Path(directory).is_dir() and member in {"data.txt", "folder/child.txt"}, call
        assert call["cwd"] == (str(left) if directory == str(left) else str(root)), call
        assert directory in {str(left), str(mount)}, call
    assert {call["argv"][0] for call in calls} <= {"list", "copyout"}, calls
    return {"mount": str(mount), "working_at_entry": str(left), "uri": uri, "calls": calls,
            "copied": (right / "data.txt").read_text()}


def main():
    if (os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file()):
        raise SystemExit("source-less UI requires uid1001 in the owned disposable container")
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("--native-only", action="store_true")
    parser.add_argument("--tmux", default="/usr/bin/tmux")
    arguments = parser.parse_args()
    base = Path("/review/cases/extfs-local-sourceless-ui")
    base.mkdir(exist_ok=True)
    if base.is_symlink() or base.stat().st_uid != os.geteuid():
        raise SystemExit("fixture base must be owned and not a symlink")
    root = base / ("run-" + str(os.getpid()))
    root.mkdir()
    os.environ["LD_LIBRARY_PATH"] = "/review/control/lib"
    result = exercise("/review/control/mc" if arguments.native_only else arguments.binary,
                      root, arguments.native_only, arguments.tmux)
    evidence = Path("/review/evidence/session") / ("extfs-local-sourceless-ui-" + str(os.getpid()) + ".json")
    evidence.write_text(json.dumps(result, indent=2) + "\n")
    print(("Native MC" if arguments.native_only else "mcpp") +
          ": source-less %cd, two directory contexts, view/copy and real root parents passed")
    print(evidence)


if __name__ == "__main__":
    main()
