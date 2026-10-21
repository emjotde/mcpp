#!/usr/bin/env python3
"""One original statusbar workflow, reserved for the parent's matching build."""

import argparse
import json
import os
from pathlib import Path
import re
import stat
from unittest.mock import patch

from terminal_session import SGR, TerminalSession
from test_viewer_help_styles_ui import capture, cells, check_style, color256, digest


PUBLISHED = Path(
    "/home/mcpp/src/build/local-parity/plural-commandmark-warning-next/mcpp"
)
PUBLISHED_SHA256 = "cdb1eb1455e4470221f440054745811ab93105dc7aacdc8dc1068a1c3a6bf65e"
PAYLOAD = "ORIGINAL_STATUSBAR Ω literal:17\nunchanged bytes\n".encode()
OTHER = "ORIGINAL_OTHER Ω literal:17\nunchanged bytes\n".encode()
SKIN = b"""[skin]
description=Original independent statusbar fixture
[core]
_default_=lightgray;black;bold+italic+underline+reverse+blink
statusbar=red;green;none
[menu]
_default_=cyan;blue;none
[viewer]
_default_=lightgray;black;bold+italic+underline+reverse+blink
viewselected=black;cyan;none
[editor]
_default_=lightgray;black;bold+italic+underline+reverse+blink
[diffviewer]
_default_=lightgray;black;bold+italic+underline+reverse+blink
[statusbar]
_default_=color35;color53;none
"""
SETTINGS = (
    "use_subshell=false\nauto_save_setup=false\neditor_check_newline=false\n"
    "editor_confirm_save=false\neditor_state_full_filename=false\n"
)


def private_directory(path):
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    state = path.lstat()
    assert stat.S_ISDIR(state.st_mode) and state.st_uid == 1001, path
    assert not state.st_mode & 0o077, path


def status_row(rows, row, attributes):
    return [
        check_style(cell, color256(35), color256(53), attributes)
        for cell in rows[row]
    ]


def has_statusbar(cell):
    return cell["foreground"] == color256(35) and cell["background"] == color256(53)


def editor_windowed_status(rows):
    row = next(row for row in rows if "L:[" in "".join(cell["text"] for cell in row))
    text = "".join(cell["text"] for cell in row)
    return [
        check_style(cell, color256(35), color256(53), 0)
        for cell in row[text.index("Ω:17.txt"):-1]
    ]


def configured_ui(root, name):
    ui = root / name
    config = ui / "config/mcpp"
    config.mkdir(parents=True)
    (config / "config").write_text(SETTINGS)
    return ui


def exercise(arguments, root, evidence, result):
    left, right = root / "left", root / "right"
    document, other = left / "Ω:17.txt", right / "other.txt"
    resource = root / "original-statusbar.ini"
    if not arguments.resume:
        left.mkdir()
        right.mkdir()
        document.write_bytes(PAYLOAD)
        other.write_bytes(OTHER)
        resource.write_bytes(SKIN)
    else:
        assert document.read_bytes() == PAYLOAD and other.read_bytes() == OTHER
        assert resource.read_bytes() == SKIN
    originals = {str(path): digest(path) for path in (document, other, resource)}
    if arguments.resume:
        assert result["resources_before"] == originals
    else:
        result["resources_before"] = originals
        result["observations"] = {}
    observations = result["observations"]
    common = ["-u", "--nokeymap", "-c", "-S", resource]

    if not arguments.resume:
        with TerminalSession(
            arguments.binary, arguments.tmux, configured_ui(root, "viewer"),
            [*common, "-v", document], width=100, height=30,
            record_exit=True, record_stderr=True,
        ) as terminal:
            terminal.wait_for("ORIGINAL_STATUSBAR")
            rows = capture(terminal, evidence, "viewer-text")
            observations["viewer_text"] = status_row(rows, 0, 0)
            assert not any(has_statusbar(cell) for row in rows[1:-1] for cell in row)
            terminal.press("F4")
            terminal.wait_for("Hex")
            rows = capture(terminal, evidence, "viewer-hex")
            observations["viewer_hex"] = status_row(rows, 0, 0)
            assert not any(has_statusbar(cell) for row in rows[1:-1] for cell in row)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
            assert terminal.stderr_file.read_text() == "", terminal.stderr_file.read_text()

    if "editor_windowed" not in observations:
        with TerminalSession(
            arguments.binary, arguments.tmux, configured_ui(root, "editor"),
            [*common, "-e", document], width=100, height=30,
            record_exit=True, record_stderr=True,
        ) as terminal:
            terminal.wait_for("ORIGINAL_STATUSBAR")
            terminal.wait_for("L:[")
            rows = capture(terminal, evidence, "editor-fullscreen")
            observations["editor_fullscreen"] = status_row(rows, 0, 0)
            assert "Ω:17.txt" in "".join(cell["text"] for cell in rows[0])
            terminal.press("F9")
            terminal.wait_for("Window")
            rows = capture(terminal, evidence, "editor-menu")
            assert not any(has_statusbar(cell) for cell in rows[0])
            terminal.press("w", "t")
            terminal.wait(
                lambda: any("L:[" in row for row in terminal.text().splitlines()[1:]),
                "windowed editor status row",
            )
            rows = capture(terminal, evidence, "editor-windowed")
            observations["editor_windowed"] = editor_windowed_status(rows)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
            assert terminal.stderr_file.read_text() == "", terminal.stderr_file.read_text()
            result["editor_exit"] = 0

    overrides = "statusbar=color35,color53,bold+italic+underline+reverse+blink"
    with TerminalSession(
        arguments.binary, arguments.tmux, configured_ui(root, "panels"),
        [*common, "-C", overrides, left, right], width=100, height=30,
        record_exit=True, record_stderr=True,
    ) as terminal:
        terminal.wait_for("Ω:17.txt")
        rows = capture(terminal, evidence, "panels")
        assert not any(has_statusbar(cell) for row in rows for cell in row)
        terminal.press("Home", "Down", "C-x", "q")
        terminal.wait_for("Quick view: Ω:17.txt")
        numeric = f"{len(PAYLOAD)}/{len(PAYLOAD)}  100%"
        terminal.wait_for(numeric)
        rows = capture(terminal, evidence, "quick-view")
        row = next(row for row in rows if numeric in "".join(cell["text"] for cell in row))
        text = "".join(cell["text"] for cell in row)
        column = text.index(numeric)
        observations["quick_numeric"] = [
            check_style(cell, color256(35), color256(53), 31)
            for cell in row[column:column + len(numeric)]
        ]
        title_row = next(row for row in rows if "Quick view:" in "".join(cell["text"] for cell in row))
        assert not any(has_statusbar(cell) for cell in title_row)
        terminal.press("Tab")
        terminal.wait_for("Quick view focused")
        rows = capture(terminal, evidence, "quick-view-focused")
        row = next(row for row in rows if numeric in "".join(cell["text"] for cell in row))
        column = "".join(cell["text"] for cell in row).index(numeric)
        for cell in row[column:column + len(numeric)]:
            check_style(cell, color256(35), color256(53), 31)
        terminal.press("Tab")
        terminal.wait_for("File listing focused")
        terminal.press("C-x", "q")
        terminal.wait_for("Quick view:", present=False)
        terminal.press("Tab", "Home", "Down", "Tab", "C-x", "C-d")
        terminal.wait_for("Dif 1")
        rows = capture(terminal, evidence, "diff")
        observations["diff_headers"] = status_row(rows, 0, 31)
        assert not any(has_statusbar(cell) for row in rows[1:-1] for cell in row)
        terminal.press("F10")
        terminal.wait_for("Dif 1", present=False)
        terminal.press("F10")
        terminal.wait_file(terminal.exit_file, b"0\n")
        assert terminal.stderr_file.read_text() == "", terminal.stderr_file.read_text()

    assert {str(path): digest(path) for path in (document, other, resource)} == originals
    result.update(resources_unchanged=True, literal_bytes_unchanged=True, native_runs=0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--tmux", default="/usr/bin/tmux")
    parser.add_argument("--resume", action="store_true",
                        help="continue only from a recorded controller-only checkpoint")
    arguments = parser.parse_args()
    if (os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file()):
        parser.error("requires UID1001 in the owned disposable review container")
    if not re.fullmatch("[0-9a-f]{64}", arguments.sha256):
        parser.error("requires the parent's matching-build SHA256")
    arguments.binary = arguments.binary.resolve(strict=True)
    if (digest(arguments.binary) != arguments.sha256 or
            arguments.binary == PUBLISHED.resolve() or arguments.sha256 == PUBLISHED_SHA256):
        parser.error("do not run a preserved or unmatched pre-change application")
    assert digest(PUBLISHED) == PUBLISHED_SHA256, "the published 4e65a5c application changed"
    os.umask(0o077)
    os.chdir("/review/cases")
    base = Path("statusbar-8789/ui").absolute()
    private_directory(base)
    root = base / arguments.sha256[:16]
    root.mkdir(exist_ok=arguments.resume)
    evidence = Path("/review/evidence/session/statusbar-8789/ui")
    private_directory(evidence)
    evidence = evidence / root.name
    evidence.mkdir(exist_ok=arguments.resume)
    if arguments.resume:
        original = (evidence / "result.json").read_bytes()
        result = json.loads(original)
        assert result["status"] == "failed" and result["sha256"] == arguments.sha256
        assert result["binary"] == str(arguments.binary)
        assert (root / "viewer/exit.status").read_bytes() == b"0\n"
        assert not (root / "panels").exists()
        if set(result["observations"]) == {"viewer_text", "viewer_hex"}:
            assert not (root / "editor").exists()
            warning = (root / "viewer/stderr.txt").read_text()
            assert warning.count("not supported by mcpp") == 2
            assert "editor_syntax_highlighting" in warning and "editor_line_state" in warning
            (evidence / "result-initial-fixture-warning.json").write_bytes(original)
            result["controller_correction"] = {
                "reason": "removed two native-INI spellings from the flat mcpp fixture config",
                "initial_stderr": warning, "viewer_repeated": False,
            }
        else:
            assert set(result["observations"]) == {
                "viewer_text", "viewer_hex", "editor_fullscreen",
            }
            assert result["controller_correction"]["viewer_repeated"] is False
            assert (root / "editor/stderr.txt").read_text() == ""
            ansi = (root / "editor/failure.ansi").read_text()
            rows = cells(ansi)
            assert any("L:[" in "".join(cell["text"] for cell in row) for row in rows[1:])
            assert "[*][X]" not in SGR.sub("", ansi)
            result["observations"]["editor_windowed"] = editor_windowed_status(rows)
            (evidence / "result-initial-window-controls-expectation.json").write_bytes(original)
            (evidence / "editor-windowed.ansi").write_text(ansi)
            (evidence / "editor-windowed.txt").write_text(SGR.sub("", ansi))
            result["controller_correction"]["windowed_editor"] = {
                "reason": "window buttons are disabled by default; check actual status content",
                "capture": str(root / "editor/failure.ansi"), "editor_repeated": False,
            }
            result["editor_exit"] = (
                "not asserted: controller cleanup followed its disabled-button expectation"
            )
        result["controller_correction"]["sha256"] = digest(Path(__file__))
    else:
        result = {
            "status": "failed", "group": "3/matching-build-statusbar-ui",
            "binary": str(arguments.binary), "sha256": arguments.sha256, "root": str(root),
        }
    environment = {
        "LD_LIBRARY_PATH": "/review/control/lib", "COLORTERM": "truecolor",
        "LANGUAGE": "C", "LC_MESSAGES": "C", "MC_COLOR_TABLE": "", "NO_COLOR": "",
    }
    try:
        with patch.dict(os.environ, environment):
            exercise(arguments, root, evidence, result)
        assert digest(PUBLISHED) == PUBLISHED_SHA256
        result["status"] = (
            "passed-with-recorded-controller-corrections" if arguments.resume else "passed"
        )
    finally:
        (evidence / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("statusbar-ui: passed; " + str(evidence / "result.json"))


if __name__ == "__main__":
    main()
