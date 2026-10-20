#!/usr/bin/env python3
"""The single reserved input-role UI case; requires the parent's post-change build."""

import argparse
import json
import os
from pathlib import Path
import re
from unittest.mock import patch

from terminal_session import TerminalSession
from test_viewer_help_styles_ui import COLORS, capture, cell_at, check_style, digest, private_directory


COMMON = Path("/home/mcpp/src/build/local-parity/mcpp")
COMMON_SHA256 = "507f0060e933236faa8f81224637bfe59d463bae665637ac49c57b069b82eae3"
PRE_CHANGE_SHA256 = {
    COMMON_SHA256,
    "9b26bfcf9ea036b5b83c0b9622d699a2e5d7ddc97fae86d99f71b759421bccd1",
    "ee2d01f8d7d3b63e079ecad769cf48cd59b637d2eb2433c887da0e4dab28bc35",
}
FILENAME = "original-é中_$[x].txt"
PAYLOAD = "ORIGINAL_LITERAL α中 $$ [] ;\n".encode()
LITERAL_DIRECTORY = "éZ中_$[x]"
PREFERENCES = b"auto_save_setup=false\nuse_subshell=false\n"
SKIN = b"""[skin]
description=Original editable input role fixture
[core]
_default_=lightgray;black;none
selected=black;cyan;none
input=lightgray;red;bold
inputmark=yellow;green;underline
inputunchanged=cyan;magenta;bold
[dialog]
_default_=lightgray;blue;bold+underline
dfocus=black;lightgray;bold+underline
"""
ROLES = {
    "input": ("brightcyan", "red", "italic", COLORS[14], COLORS[1], 2),
    "inputmark": ("black", "magenta", "none", COLORS[0], COLORS[5], 0),
    "inputunchanged": ("brightgreen", "blue", "none", COLORS[10], COLORS[4], 0),
}


def styled(rows, needle, role, offset=0):
    return check_style(cell_at(rows, needle, offset), *ROLES[role][3:])


def cursor(terminal):
    return terminal.run("display-message", "-p", "-t", "test:0.0",
                        "#{cursor_x},#{cursor_y},#{cursor_flag}").strip()


def prompt_cell(rows):
    label = next(index for index, row in enumerate(rows)
                 if "Directory name:" in "".join(cell["text"] for cell in row))
    for row in rows[label + 1:]:
        text = "".join(cell["text"] for cell in row)
        if "> " in text:
            return row[text.index("> ") + 2]
    raise AssertionError("the original editable directory field is missing")


def exercise(binary, tmux, root, evidence, result):
    left, right, profile = root / "left", root / "right", root / "ui"
    left.mkdir()
    right.mkdir()
    preferences = profile / "config/mcpp/config"
    preferences.parent.mkdir(parents=True)
    preferences.write_bytes(PREFERENCES)
    document, resource = left / FILENAME, root / "original-input.ini"
    document.write_bytes(PAYLOAD)
    resource.write_bytes(SKIN)
    originals = {str(path): digest(path) for path in (document, resource, preferences)}
    arguments = ["-u", "-c", "--nokeymap", "-S", resource, "-C",
                 ":".join(role + "=" + ",".join(style[:3]) for role, style in ROLES.items()),
                 "--", left, right]
    observations = {}
    result.update({"arguments": list(map(str, arguments)), "resources_before": originals,
                   "observations": observations})
    environment = {"COLORTERM": "truecolor", "LANGUAGE": "C", "LC_MESSAGES": "C",
                   "LD_LIBRARY_PATH": "/review/control/lib"}
    with patch.dict(os.environ, environment):
        with TerminalSession(binary, tmux, profile, arguments, width=110, height=30,
                             record_exit=True, record_stderr=True) as terminal:
            terminal.wait_for(FILENAME)
            terminal.press("Home", "Down", "F5")
            terminal.wait_for("Source mask:")
            destination = str(right) + "/"
            terminal.wait_for(destination)
            rows = capture(terminal, evidence, "pristine-copy")
            observations["pristine"] = styled(rows, destination, "inputunchanged")
            styled(rows, destination, "inputunchanged", len(destination) + 3)
            styled(rows, "> *", "inputunchanged", 2)
            initial_cursor = cursor(terminal)
            terminal.press("Tab")
            rows = capture(terminal, evidence, "blurred-pristine")
            styled(rows, destination, "inputunchanged")
            terminal.press("BTab", "End")
            rows = capture(terminal, evidence, "cursor-action-without-edit")
            observations["cursor_action"] = styled(rows, destination, "input")
            assert cursor(terminal) == initial_cursor, "End changed an already-at-end cursor"
            terminal.type("X")
            terminal.wait_for(destination + "X")
            terminal.press("BSpace")
            terminal.wait_for(destination)
            rows = capture(terminal, evidence, "old-bytes-remain-edited")
            styled(rows, destination, "input")
            styled(rows, destination, "input", len(destination) + 3)
            terminal.press("Home", "S-Right")
            rows = capture(terminal, evidence, "manual-selection")
            observations["manual_selection"] = styled(rows, destination, "inputmark")
            styled(rows, destination, "input", 1)
            selection_cursor = cursor(terminal)
            terminal.press("Tab")
            rows = capture(terminal, evidence, "blurred-selection")
            styled(rows, destination, "inputmark")
            terminal.press("BTab")
            terminal.wait_idle()
            assert cursor(terminal) == selection_cursor, "blur/refocus moved the selection cursor"
            terminal.type("Q")
            terminal.wait_for("Q" + destination[1:])
            terminal.press("Escape")
            terminal.wait_for("Source mask:", present=False)

            terminal.press("F7")
            terminal.wait_for("Directory name:")
            rows = capture(terminal, evidence, "empty-pristine")
            observations["empty_pristine"] = check_style(prompt_cell(rows), *ROLES["inputunchanged"][3:])
            terminal.press("End")
            rows = capture(terminal, evidence, "empty-after-cursor-action")
            check_style(prompt_cell(rows), *ROLES["input"][3:])
            terminal.press("Escape")
            terminal.wait_for("Directory name:", present=False)
            terminal.press("F7")
            terminal.wait_for("Directory name:")
            terminal.type("é中_$[x]")
            terminal.wait_for("é中_$[x]")
            terminal.press("Home", "Right")
            terminal.type("Z")
            terminal.wait_for(LITERAL_DIRECTORY)
            terminal.press("S-Right")
            rows = capture(terminal, evidence, "unicode-selection")
            observations["unicode_selection"] = styled(rows, LITERAL_DIRECTORY, "inputmark", 2)
            styled(rows, LITERAL_DIRECTORY, "input", 0)
            unicode_cursor = cursor(terminal)
            terminal.press("Tab")
            rows = capture(terminal, evidence, "blurred-unicode-selection")
            styled(rows, LITERAL_DIRECTORY, "inputmark", 2)
            terminal.press("BTab")
            terminal.wait_idle()
            assert cursor(terminal) == unicode_cursor, "Unicode selection lost its cursor on refocus"
            terminal.type("中")
            terminal.wait_for(LITERAL_DIRECTORY)
            terminal.press("Enter")
            terminal.wait_for("Directory name:", present=False)
            created = left / LITERAL_DIRECTORY
            terminal.wait(lambda: created.is_dir(), "the literal Unicode directory name")
            assert os.fsencode(created.name) == LITERAL_DIRECTORY.encode()

            command = "CMD_é中_$[x]"
            terminal.type(command)
            terminal.wait_for(command)
            rows = capture(terminal, evidence, "command-input-excluded")
            command_cell = cell_at(rows, command)
            assert all((command_cell["foreground"], command_cell["background"]) != style[3:5]
                       for style in ROLES.values()), command_cell
            observations["command_input_excluded"] = command_cell
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
            assert terminal.stderr_file.read_bytes() == b"", terminal.stderr_file.read_text()
    assert document.read_bytes() == PAYLOAD and resource.read_bytes() == SKIN
    assert preferences.read_bytes() == PREFERENCES
    assert {str(path): digest(path) for path in (document, resource, preferences)} == originals
    assert not list(right.iterdir())
    assert sorted(path.name for path in left.iterdir()) == sorted((FILENAME, LITERAL_DIRECTORY))
    result.update({"resources_unchanged": True, "literal_file_bytes_unchanged": True,
                   "literal_directory_bytes_hex": os.fsencode(LITERAL_DIRECTORY).hex(),
                   "saved_preferences_unchanged": True, "launch_colors_saved": False})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--tmux", default="/usr/bin/tmux")
    arguments = parser.parse_args()
    if (os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file()):
        parser.error("requires UID1001 in the owned disposable review container")
    if not re.fullmatch("[0-9a-f]{64}", arguments.sha256):
        parser.error("--sha256 must be the parent's exact post-change binary digest")
    binary = arguments.binary.resolve(strict=True)
    binary_before = digest(binary)
    if (not os.access(binary, os.X_OK) or binary == COMMON.resolve() or
            binary_before in PRE_CHANGE_SHA256 or binary_before != arguments.sha256):
        parser.error("do not launch a preserved, pre-change or unmatched binary")
    common_before = digest(COMMON)
    if common_before != COMMON_SHA256:
        parser.error("the preserved common binary changed")
    os.umask(0o077)
    os.chdir("/review/cases")
    base = Path("input-styles/ui")
    private_directory(base)
    root = (base / ("run-" + str(os.getpid()))).absolute()
    root.mkdir()
    evidence = Path("/review/evidence/session/input-styles/ui")
    private_directory(evidence)
    evidence /= root.name
    evidence.mkdir()
    os.chdir(root)
    result = {"case": "fourth-input-style-actual-ui", "status": "failed", "root": str(root),
              "binary": str(binary), "sha256": binary_before, "native_rerun": False}
    try:
        exercise(binary, arguments.tmux, root, evidence, result)
        assert digest(binary) == binary_before and digest(COMMON) == common_before
        result["status"] = "passed"
    except BaseException as error:
        result["error"] = repr(error)
        raise
    finally:
        (evidence / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    print("input styles UI: passed; " + str(evidence / "result.json"))


if __name__ == "__main__":
    main()
