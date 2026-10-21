#!/usr/bin/env python3
"""One hash-pinned F2 workflow for constant ANSI-C heredoc delimiters."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat

from terminal_session import SGR, TerminalSession


PRE_ANSI_SHA256 = "270425ee58de7cfddd2884ea6a36f1512e0e01dae9fca8def6fa809d0f2dfe3b"
FILENAME = "zz-ansi-data café ' \" $(printf DATA) `printf DATA`\\tail\n* %f"
TAGS = (
    "a-ansi-tag ' \" $(printf DATA)\\tail",
    "b-ansi-tag café `printf DATA`\n? %f",
)
ANSWER = "Original prompt café ' \" $(printf DATA) `printf DATA`\\tail * %f"
PAYLOAD = b"Original\n"


def require(value, message):
    if not value:
        raise AssertionError(message)


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def private_directory(path):
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    state = path.lstat()
    require(stat.S_ISDIR(state.st_mode) and state.st_uid == 1001 and
            not state.st_mode & 0o077, f"not an owned private directory: {path}")


def capture(terminal, evidence, name):
    terminal.wait_idle()
    ansi = terminal.text(ansi=True)
    (evidence / (name + ".ansi")).write_text(ansi)
    (evidence / (name + ".txt")).write_text(SGR.sub("", ansi))


def exercise(binary, expected_hash, tmux, evidence, result):
    left, right = Path("left"), Path("right")
    left.mkdir()
    right.mkdir()
    documents = tuple(left / name for name in (FILENAME, *TAGS))
    for document in documents:
        document.write_bytes(PAYLOAD)
    config = Path("ui/config/mcpp")
    config.mkdir(parents=True)
    (config / "config").write_text(
        "pause_after_run=0\nuse_subshell=0\nconfirm_execute=0\nconfirm_exit=0\n"
    )
    valid = (
        "a ANSI delimiter literal bytes\n"
        r"    cat <<$'L20_\x41NSI' > ../menu.bin" "\n"
        "    %f\n    %{ANSI prompt bytes}\n    %t\n    %0f\n    L20_ANSI\n"
        r"""    cat <<"pre"$'\105OF\\\'\"\?'"post" >> ../menu.bin""" "\n"
        "    Original concatenated delimiter\n"
        """    preEOF\\'"?post\n"""
        "    cat <<-$'L20_TABS' >> ../menu.bin\n"
        "    \tOriginal tabs\n    \tL20_TABS\n"
    )
    invalid_line = len(valid.splitlines()) + 1
    menu = left / ".mc.menu"
    menu.write_text(
        valid + "b Invalid ANSI delimiter\n"
        r"    cat <<$'L20\q' > ../invalid.bin" "\n"
        "    Original\n"
        r"    L20\q" "\n"
    )
    resources = (*documents, menu)
    result["resource_sha256_before"] = {str(path): digest(path) for path in resources}
    result["menu_source"] = menu.read_text()
    require(digest(binary) == expected_hash, "private binary changed before UI launch")
    arguments = [
        "LANGUAGE=C", "LC_MESSAGES=C", "BASH_ENV=/dev/null", "ENV=/dev/null",
        "LD_LIBRARY_PATH=/review/control/lib", "SHELL=/bin/bash",
        str(binary), "-u", "--nokeymap", "--", str(left.resolve()), str(right.resolve()),
    ]
    result["arguments"] = arguments
    joined = " ".join(TAGS) + " "
    expected = (
        FILENAME + "\n" + ANSWER + "\n" + joined + "\n" + FILENAME + "\n" +
        "Original concatenated delimiter\nOriginal tabs\n"
    ).encode()
    with TerminalSession("/usr/bin/env", tmux, "ui", arguments, width=120, height=32,
                         record_exit=True, record_stderr=True) as terminal:
        terminal.record_output()
        terminal.wait_for("zz-ansi-data")
        terminal.press("End", "Up", "Up", "Insert", "End", "Up", "Insert", "End", "F2")
        terminal.wait_for("ANSI delimiter literal bytes")
        terminal.press("a")
        terminal.wait_for("ANSI prompt bytes")
        terminal.type(ANSWER)
        capture(terminal, evidence, "ansi-cancel-prompt")
        terminal.press("Escape")
        terminal.wait_for("ANSI prompt bytes", present=False)
        terminal.wait_for("ANSI delimiter literal bytes")
        require(not Path("menu.bin").exists(), "cancelled ANSI menu command ran")
        result["cancelled_without_execution"] = True
        terminal.press("a")
        terminal.wait_for("ANSI prompt bytes")
        terminal.press("C-u")
        terminal.type(ANSWER)
        terminal.press("Enter")
        terminal.wait_file("menu.bin", expected)
        terminal.wait_for("ANSI prompt bytes", present=False)
        terminal.wait_for("Name")
        capture(terminal, evidence, "ansi-complete")
        terminal.press("F2")
        terminal.wait_for("Invalid ANSI delimiter")
        terminal.press("b")
        terminal.wait_for("unsupported escape in ANSI-C")
        terminal.wait_for(".mc.menu:" + str(invalid_line))
        require(not Path("invalid.bin").exists(), "malformed ANSI delimiter executed")
        capture(terminal, evidence, "ansi-parser-source-error")
        result["malformed_delimiter_rejected"] = True
        result["parser_error_source"] = str(menu.resolve()) + ":" + str(invalid_line)
        terminal.press("Escape")
        terminal.wait_for("Invalid ANSI delimiter", present=False)
        terminal.press("F10")
        terminal.wait_file(terminal.exit_file, b"0\n")
        result["stderr"] = terminal.stderr_file.read_text()
        require(result["stderr"] == "", "unexpected mcpp stderr: " + result["stderr"])
    result["output_hex"] = Path("menu.bin").read_bytes().hex()
    result["literal_filename_prompt_tag_and_raw_bytes"] = True
    require({path.name for path in left.iterdir()} == {FILENAME, *TAGS, ".mc.menu"} and
            not list(right.iterdir()), "the literal pane resources changed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="parent-approved new private mcpp binary")
    parser.add_argument("--tmux", default="/usr/bin/tmux")
    parser.add_argument("--sha256", required=True, help="parent-provided exact binary SHA256")
    arguments = parser.parse_args()
    if (os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file()):
        parser.error("requires UID1001 in the owned disposable review container")
    if not re.fullmatch("[0-9a-f]{64}", arguments.sha256):
        parser.error("--sha256 must be the parent's exact lowercase SHA256")
    binary = arguments.binary.resolve()
    if (not binary.is_relative_to("/home/mcpp/src/build/local-parity") or
            not binary.is_file() or not os.access(binary, os.X_OK)):
        parser.error("the binary must be a private executable under the approved snapshot")
    binary_before = digest(binary)
    if binary_before == PRE_ANSI_SHA256 or binary_before != arguments.sha256:
        parser.error("do not launch the frozen pre-ANSI or an unmatched binary")
    os.umask(0o077)
    os.chdir("/review/cases")
    base = Path("l20-aui")
    private_directory(base)
    root = base / ("r" + str(os.getpid()))
    root.mkdir()
    root = root.resolve()
    evidence = Path("../evidence/session/l20-menu-ansi-ui")
    private_directory(evidence)
    evidence = evidence / root.name
    evidence.mkdir()
    evidence = Path(os.path.relpath(evidence.resolve(), root))
    os.chdir(root)
    result = {
        "case": "one-ansi-heredoc-f2-workflow", "root": str(root),
        "binary": str(binary), "binary_sha256_before": binary_before,
        "runner": "/bin/bash", "native_rerun": False, "model_rerun": False, "passed": False,
    }
    try:
        exercise(binary, arguments.sha256, arguments.tmux, evidence, result)
        result["passed"] = True
    except BaseException as error:
        result["error"] = repr(error)
        raise
    finally:
        result["binary_sha256_after"] = digest(binary)
        before = result.get("resource_sha256_before", {})
        after = {name: digest(Path(name)) if Path(name).is_file() else None for name in before}
        result["resource_sha256_after"] = after
        result["binary_unchanged"] = result["binary_sha256_after"] == binary_before
        result["resources_unchanged"] = bool(before) and before == after
        result["passed"] = result["passed"] and result["binary_unchanged"] and result["resources_unchanged"]
        encoded = json.dumps(result, indent=2) + "\n"
        Path("result.json").write_text(encoded)
        (evidence / "result.json").write_text(encoded)
    require(result["passed"], "the matching binary or literal resources changed")
    print("PASS one ANSI heredoc F2 workflow: cancellation, literal bytes, <<-, and sourced error")
    print(evidence.resolve() / "result.json")


if __name__ == "__main__":
    main()
