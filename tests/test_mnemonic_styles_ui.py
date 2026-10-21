#!/usr/bin/env python3
"""One compact mnemonic-role UI case; requires the parent's matching build."""

import argparse
import json
import os
from pathlib import Path
import re
from unittest.mock import patch

from terminal_session import TerminalSession
from test_audit_ui import click
from test_viewer_help_styles_ui import (
    capture, cell_at, check_style, digest, private_directory,
)


PUBLISHED = Path("/home/mcpp/src/build/local-parity/menu-frame-status-next/mcpp")
PUBLISHED_SHA256 = "270425ee58de7cfddd2884ea6a36f1512e0e01dae9fca8def6fa809d0f2dfe3b"
FILENAME = "literal-é中_$[x].txt"
PAYLOAD = "ORIGINAL_LITERAL é中 $$ [] ;\n".encode()
PREFERENCES = (
    b"auto_save_setup=false\nuse_subshell=false\nconfirm_exit=false\nescape_timeout=50000\n"
)
SKIN = b"""[skin]
description=Original mnemonic role UI sample
truecolors=true
[core]
_default_=lightgray;black;none
selected=black;cyan;none
input=white;#183248;none
inputunchanged=white;#183248;none
[dialog]
_default_=white;#183248;none
dfocus=black;#d0c090;none
dhotnormal=#ffddaa;#234567;none
dhotfocus=#46dbfe;#4a1970;none
[error]
_default_=white;red;none
errdfocus=black;lightgray;none
errdhotnormal=#09ae65;#481a2b;none
errdhotfocus=yellow;magenta;bold
[menu]
_default_=white;#183248;none
menusel=black;#b0c090;none
menuhot=#aabbcc;#345678;none
menuhotsel=#f0e060;#24486c;none
"""
OVERRIDES = (
    "dhotnormal=#f4a836,,none:dhotfocus=#46dbfe,#4a1970,none:"
    "errdhotnormal=#ff9900,#3d203d,none:menuhotsel=,#0f4359,none"
)
STYLES = {
    "dhotnormal": ((244, 168, 54), (35, 69, 103), 0),
    "dhotfocus": ((70, 219, 254), (74, 25, 112), 0),
    "errdhotnormal": ((255, 153, 0), (61, 32, 61), 0),
    "menuhot": ((170, 187, 204), (52, 86, 120), 0),
    "menuhotsel": ((240, 224, 96), (15, 67, 89), 0),
}
BODY = ((255, 255, 255), (24, 50, 72), 0)


def hot(rows, needle, role, offset=0):
    return check_style(cell_at(rows, needle, offset), *STYLES[role])


def cursor(terminal):
    return terminal.run("display-message", "-p", "-t", "test:0.0",
                        "#{cursor_x},#{cursor_y},#{cursor_flag}").strip()


def exercise(binary, tmux, root, evidence, result):
    left, right, profile = root / "left", root / "right", root / "ui"
    left.mkdir()
    right.mkdir()
    document, resource = left / FILENAME, root / "original-mnemonics.ini"
    preferences = profile / "config/mcpp/config"
    preferences.parent.mkdir(parents=True)
    document.write_bytes(PAYLOAD)
    resource.write_bytes(SKIN)
    preferences.write_bytes(PREFERENCES)
    originals = {str(path): digest(path) for path in (document, resource, preferences)}
    command = ["-u", "-c", "--nokeymap", "-S", resource, "-C", OVERRIDES, "--", left, right]
    observations = {}
    result.update({"arguments": list(map(str, command)), "resources_before": originals,
                   "observations": observations})
    environment = {
        "COLORTERM": "truecolor", "LANGUAGE": "C", "LC_MESSAGES": "C",
        "LD_LIBRARY_PATH": "/review/control/lib",
    }
    try:
        with patch.dict(os.environ, environment):
            with TerminalSession(binary, tmux, profile, command, width=110, height=30,
                                 record_exit=True, record_stderr=True) as terminal:
                terminal.wait_for(FILENAME)
                click(terminal, "Options", row=0)
                terminal.wait_for("Configuration...")
                rows = capture(terminal, evidence, "selected-menu")
                observations["selected_menu_title"] = hot(rows, "Options", "menuhotsel")
                observations["selected_menu_row"] = hot(rows, "Configuration...", "menuhotsel")
                observations["normal_menu_row"] = hot(rows, "Layout...", "menuhot")
                check_style(cell_at(rows, "Layout...", 1), *BODY)
                click(terminal, "Configuration...")
                terminal.wait_for("Verbose operation")
                rows = capture(terminal, evidence, "focused-dialog-mnemonic")
                observations["focused_dialog"] = hot(rows, "Verbose operation", "dhotfocus")
                observations["normal_dialog"] = hot(rows, "Compute totals", "dhotnormal", 12)
                check_style(cell_at(rows, "Compute totals", 0), *BODY)
                hot(rows, "OK", "dhotnormal")
                hot(rows, "Cancel", "dhotnormal")
                original_cursor = cursor(terminal)
                terminal.press("Tab")
                rows = capture(terminal, evidence, "dialog-focus-moved")
                hot(rows, "Verbose operation", "dhotnormal")
                hot(rows, "Compute totals", "dhotfocus", 12)
                terminal.press("BTab")
                terminal.wait_idle()
                assert cursor(terminal) == original_cursor, "focus round trip changed the original cursor"
                terminal.press("BTab")
                rows = capture(terminal, evidence, "focused-cancel-glyph")
                observations["focused_cancel"] = hot(rows, "Cancel", "dhotfocus")
                hot(rows, "OK", "dhotnormal")
                terminal.press("BTab")
                rows = capture(terminal, evidence, "focused-ok-glyph")
                observations["focused_ok"] = hot(rows, "OK", "dhotfocus")
                hot(rows, "Cancel", "dhotnormal")
                click(terminal, "[ Cancel ]")
                terminal.wait_for("Verbose operation", present=False)

                right.chmod(0o500)
                terminal.press("Home", "Down", "F5")
                terminal.wait_for("Source mask:")
                terminal.press("Enter")
                terminal.wait_for("File operation error")
                rows = capture(terminal, evidence, "unfocused-error-mnemonic")
                observations["normal_error"] = hot(rows, "Skip", "errdhotnormal")
                check_style(cell_at(rows, "Skip", 1), *BODY)
                terminal.press("Tab")
                rows = capture(terminal, evidence, "error-focus-moved")
                hot(rows, "Retry", "errdhotnormal")
                terminal.press("Escape")
                terminal.wait_for("File operation error", present=False)
                terminal.wait_for("Operation cancelled")
                terminal.wait_idle(stable=0.2)
                terminal.press("F10")
                terminal.wait_file(terminal.exit_file, b"0\n")
                assert terminal.stderr_file.read_bytes() == b"", terminal.stderr_file.read_text()
    finally:
        right.chmod(0o700)
    assert document.read_bytes() == PAYLOAD and resource.read_bytes() == SKIN
    assert preferences.read_bytes() == PREFERENCES
    assert {str(path): digest(path) for path in (document, resource, preferences)} == originals
    assert not list(right.iterdir()) and sorted(path.name for path in left.iterdir()) == [FILENAME]
    result.update({"resources_unchanged": True, "literal_file_bytes_unchanged": True,
                   "saved_preferences_unchanged": True, "launch_colors_saved": False,
                   "independent_error_focus_mnemonic": "deferred; legacy whole-focus alias retained"})


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
    before = digest(binary)
    if (not os.access(binary, os.X_OK) or binary == PUBLISHED.resolve() or
            before == PUBLISHED_SHA256 or before != arguments.sha256):
        parser.error("do not launch a preserved, pre-change or unmatched binary")
    if digest(PUBLISHED) != PUBLISHED_SHA256:
        parser.error("the immutable published binary changed")
    os.umask(0o077)
    os.chdir("/review/cases/mnemonic-roles-96b2")
    base = Path("ui")
    private_directory(base)
    root = (base / ("run-" + str(os.getpid()))).absolute()
    root.mkdir()
    evidence = Path("/review/evidence/session/mnemonic-roles-96b2/ui")
    private_directory(evidence)
    evidence /= root.name
    evidence.mkdir()
    result = {"binary": str(binary), "sha256": before, "case_directory": str(root)}
    try:
        exercise(binary, arguments.tmux, root, evidence, result)
        result["status"] = "passed"
    except Exception as error:
        result.update({"status": "failed", "error": repr(error)})
        raise
    finally:
        assert digest(binary) == before, "the matching binary changed during the UI check"
        assert digest(PUBLISHED) == PUBLISHED_SHA256, "the published binary changed"
        (evidence / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    print("mnemonic actual UI: passed; evidence:", evidence)


if __name__ == "__main__":
    main()
