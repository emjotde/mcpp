#!/usr/bin/env python3
"""One reserved EOF UI group; requires the parent's matching private build."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat

from terminal_session import TerminalSession


STALE_SHA256 = "790bf9f8b11e7b8aea40f7708d668b7c88cab12da3412a3d0736fabbc64d0549"
PAYLOAD = b"alpha\n"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def private_directory(path):
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    state = path.lstat()
    assert stat.S_ISDIR(state.st_mode) and state.st_uid == 1001, path


def exercise(binary, root, value, expected):
    listing = root / "listing"
    other = root / "other"
    terminal_root = root / "terminal"
    for path in (listing, other, terminal_root / "config/mcpp"):
        private_directory(path)
    document = listing / "tiny.txt"
    document.write_bytes(PAYLOAD)
    original = digest(document)
    (terminal_root / "config/mcpp/ini").write_text(
        "[Midnight-Commander]\nmcview_eof=" + value +
        "\nconfirm_exit=0\nauto_save_setup=0\nuse_file_to_guess_type=0\n"
    )
    observations = {}
    with TerminalSession(
            binary, "tmux", terminal_root,
            ["-u", "--nokeymap", listing, other], width=72, height=14,
            record_exit=True, record_stderr=True) as terminal:
        terminal.wait_for("tiny.txt")
        terminal.press("End")
        terminal.press("F3")
        terminal.wait_for("alpha")
        full = terminal.wait_idle()
        observations["full"] = full
        rows = full.splitlines()
        assert rows[1].strip() == "alpha", full
        assert len(rows) == 14 and all(row.strip() == expected for row in rows[2:-1]), full
        assert "6/6" in rows[0], full
        terminal.press("F4")
        terminal.wait_for("00000000")
        hexadecimal = terminal.wait_idle()
        observations["hex"] = hexadecimal
        rows = hexadecimal.splitlines()
        assert all(not row.strip() for row in rows[2:-1]), hexadecimal
        terminal.press("F4")
        terminal.wait(lambda: expected in terminal.text().splitlines()[2],
                      "restore the literal text EOF preference")
        terminal.press("F10")
        terminal.wait_for("tiny.txt")
        terminal.press("C-x")
        terminal.press("q")
        terminal.wait_for("Quick view:")
        quick = terminal.wait_idle()
        observations["quick"] = quick
        assert sum(row[37:].startswith(expected) for row in quick.splitlines()) >= 3, quick
        terminal.press("F10")
        terminal.wait(
            lambda: terminal.exit_file.is_file() and terminal.exit_file.read_text().strip() == "0",
            "exit the bounded EOF workflow with status zero")
        stderr = terminal.stderr_file.read_text()
        assert "mcview_eof" not in stderr, stderr
        observations["stderr"] = stderr
    assert digest(document) == original and document.read_bytes() == PAYLOAD
    observations["fixture_unchanged"] = True
    return observations


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("sha256")
    arguments = parser.parse_args()
    assert os.geteuid() == 1001 and Path("/.dockerenv").is_file()
    assert Path("/review/.operation-regressions-disposable").is_file()
    binary = arguments.binary.resolve(strict=True)
    assert str(binary).startswith("/home/mcpp/src/build/local-parity/"), binary
    assert digest(binary) == arguments.sha256 and arguments.sha256 != STALE_SHA256
    root = Path("/review/cases/viewer-eof/ui") / arguments.sha256[:16]
    private_directory(root)
    evidence = Path("/review/evidence/session/viewer-eof-ae129337-ui.json")
    result = {
        "group": "reserved-matching-app-eof",
        "binary": str(binary),
        "sha256": arguments.sha256,
        "uid": os.geteuid(),
        "root": str(root),
        "observations": {},
    }
    try:
        for label, value, expected in (
                ("zero", "0", "0"),
                ("escaped-data", "RAW\\n\\t\\r\x1b[2J", r"RAW\n\t\r\x1B[2J")):
            result["observations"][label] = exercise(binary, root / label, value, expected)
        assert digest(binary) == arguments.sha256
        result["status"] = "passed"
        print("EOF UI group passed: full/quick text, literal zero, escaped user data, hex unchanged")
    except Exception as error:
        result["status"] = "failed"
        result["error"] = repr(error)
        raise
    finally:
        evidence.write_text(json.dumps(result, indent=2) + "\n")
        print("evidence:", evidence)


if __name__ == "__main__":
    main()
