#!/usr/bin/env python3
"""One reserved command-selection case, requiring the parent's matching UI build."""

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
PRESERVED = Path("/home/mcpp/src/build/local-parity/view-checkout-tree-eof-next/mcpp")
PRESERVED_SHA256 = "ffd3e925c7dcc3d8f920206026038b84fbfe54b1ab5ce18344743dfaefbbc94c"
FILENAME = "ORIGINAL_é中_$[x].txt"
PAYLOAD = "UNCHANGED_COMMAND_MARK α中 $$ [] ;\n".encode()
COMMAND = "M_é中_$[x] 'literal draft'"
HISTORY = "HISTORY_é中_$[x] 'not executed'"
PREFERENCES = (
    "[Midnight-Commander]\nauto_save_setup=0\nconfirm_exit=0\n"
    "[Layout]\ncommand_prompt=1\n"
    "[Colors]\nbase_color=commandlinemark=brightgreen,red,bold+underline\n"
    "xterm-256color=commandlinemark=brightcyan,,none\n"
    "[mcpp]\ncommand_history=" + HISTORY + "\n"
).encode()
SKIN = b"""[skin]
description=Original command line selection fixture
[core]
_default_=lightgray;black;none
selected=black;cyan;none
commandlinemark=black;blue;italic
input=lightgray;red;italic
inputmark=yellow;green;underline
inputunchanged=cyan;blue;bold
[dialog]
_default_=lightgray;blue;bold+underline
dfocus=black;lightgray;bold+italic+underline
"""


def style(cell):
    return {name: value for name, value in cell.items() if name != "text"}


def cursor(terminal):
    return terminal.run("display-message", "-p", "-t", "test:0.0",
                        "#{cursor_x},#{cursor_y},#{cursor_flag}").strip()


def marked(rows):
    return {
        character: check_style(cell_at(rows, COMMAND, offset), COLORS[14], COLORS[5], 0)
        for character, offset in (("é", 2), ("中", 3))
    }


def exercise(binary, tmux, root, evidence, result):
    left, right, profile = root / "left", root / "right", root / "ui"
    left.mkdir()
    right.mkdir()
    preferences = profile / "config/mcpp/ini"
    preferences.parent.mkdir(parents=True)
    preferences.write_bytes(PREFERENCES)
    document, resource = left / FILENAME, root / "original-commandline.ini"
    document.write_bytes(PAYLOAD)
    resource.write_bytes(SKIN)
    originals = {str(path): digest(path) for path in (document, resource, preferences)}
    arguments = ["-u", "-c", "--nokeymap", "-S", resource,
                 "-C", "commandlinemark=,green", "--colors=commandlinemark=,magenta",
                 "--", left, right]
    observations = {}
    result.update({"arguments": list(map(str, arguments)), "resources_before": originals,
                   "observations": observations, "command_executed": False})
    environment = {"COLORTERM": "truecolor", "LANGUAGE": "C", "LC_MESSAGES": "C",
                   "LD_LIBRARY_PATH": "/review/control/lib", "MC_COLOR_TABLE": ""}
    with patch.dict(os.environ, environment):
        with TerminalSession(binary, tmux, profile, arguments, width=110, height=30,
                             record_exit=True, record_stderr=True) as terminal:
            terminal.wait_for(FILENAME)
            terminal.type(COMMAND)
            terminal.wait_for(COMMAND)
            rows = capture(terminal, evidence, "normal-literal-draft")
            normal = style(cell_at(rows, COMMAND))
            assert (normal["foreground"], normal["background"]) != (COLORS[14], COLORS[5])
            observations["normal"] = cell_at(rows, COMMAND)
            terminal.press("C-a", "Right", "Right", "S-Right", "S-Right")
            rows = capture(terminal, evidence, "selected-unicode-draft")
            observations["selected"] = marked(rows)
            for offset in (0, 1, 4, 7, len(COMMAND) - 1):
                assert style(cell_at(rows, COMMAND, offset)) == normal
            selected_cursor = cursor(terminal)
            terminal.press("F9")
            terminal.wait_idle()
            if "Listing format" not in terminal.text():
                terminal.press("Down")
            terminal.wait_for("Listing format")
            rows = capture(terminal, evidence, "selected-draft-under-f9-popup")
            observations["blurred"] = marked(rows)
            assert style(cell_at(rows, COMMAND)) == normal
            terminal.press("Escape")
            terminal.wait_for("Listing format", present=False)
            terminal.wait_idle()
            assert cursor(terminal) == selected_cursor, "closing F9 changed the command selection cursor"
            rows = capture(terminal, evidence, "selected-draft-refocused")
            marked(rows)

            terminal.press("M-p")
            terminal.wait_for(HISTORY)
            rows = capture(terminal, evidence, "history-without-selection")
            assert style(cell_at(rows, HISTORY)) == normal
            terminal.press("M-n")
            terminal.wait_for(COMMAND)
            rows = capture(terminal, evidence, "draft-selection-restored-from-history")
            observations["restored"] = marked(rows)
            assert cursor(terminal) == selected_cursor, "history navigation lost the draft cursor or anchor"
            terminal.type("é中")
            terminal.wait_for(COMMAND)
            rows = capture(terminal, evidence, "identical-literal-replacement")
            for offset in (0, 2, 3, 4, len(COMMAND) - 1):
                assert style(cell_at(rows, COMMAND, offset)) == normal
            observations["literal_command_unchanged"] = COMMAND
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
            assert terminal.stderr_file.read_bytes() == b"", terminal.stderr_file.read_text()
    assert document.read_bytes() == PAYLOAD and resource.read_bytes() == SKIN
    assert preferences.read_bytes() == PREFERENCES
    assert {str(path): digest(path) for path in (document, resource, preferences)} == originals
    assert not list(right.iterdir()) and [path.name for path in left.iterdir()] == [FILENAME]
    result.update({"resources_unchanged": True, "literal_file_bytes_unchanged": True,
                   "saved_colors_and_history_unchanged": True, "selected_cursor": selected_cursor,
                   "launch_colors_saved": False, "native_rerun": False})


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
    if (not os.access(binary, os.X_OK) or binary in (COMMON.resolve(), PRESERVED.resolve()) or
            binary_before in (COMMON_SHA256, PRESERVED_SHA256) or binary_before != arguments.sha256):
        parser.error("do not launch a preserved, pre-change or unmatched binary")
    preserved_before = {str(path): digest(path) for path in (COMMON, PRESERVED)}
    if preserved_before != {str(COMMON): COMMON_SHA256, str(PRESERVED): PRESERVED_SHA256}:
        parser.error("a preserved binary changed")
    os.umask(0o077)
    os.chdir("/review/cases")
    base = Path("commandline-mark/ui")
    private_directory(base)
    root = (base / ("run-" + str(os.getpid()))).absolute()
    root.mkdir()
    evidence = Path("../evidence/session/commandline-mark/ui")
    private_directory(evidence)
    evidence = (evidence / root.name).absolute()
    evidence.mkdir()
    os.chdir(root)
    result = {"case": "reserved-commandline-mark-actual-ui", "status": "failed", "root": str(root),
              "binary": str(binary), "sha256": binary_before}
    try:
        exercise(binary, arguments.tmux, root, evidence, result)
        assert digest(binary) == binary_before
        assert {str(path): digest(path) for path in (COMMON, PRESERVED)} == preserved_before
        result["status"] = "passed"
    except BaseException as error:
        result["error"] = repr(error)
        raise
    finally:
        (evidence / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    print("commandline mark UI: passed; " + str(evidence / "result.json"))


if __name__ == "__main__":
    main()
