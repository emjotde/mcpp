#!/usr/bin/env python3
"""One reserved, hash-pinned F2 workflow for extended case patterns."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import stat

from terminal_session import SGR, TerminalSession


STALE_SHA256 = {
    "507f0060e933236faa8f81224637bfe59d463bae665637ac49c57b069b82eae3",
    "9b26bfcf9ea036b5b83c0b9622d699a2e5d7ddc97fae86d99f71b759421bccd1",
    "ee2d01f8d7d3b63e079ecad769cf48cd59b637d2eb2433c887da0e4dab28bc35",
}
FILENAME = "zz-extglob-data café ' \" @(?|*) ) | $(printf FILE_LITERAL)\\tail\n*.case"
PAYLOAD = b"UNCHANGED EXTGLOB MENU SOURCE\n"
ANSWER = "prompt café ' \" @(?|*) ) | $(printf PROMPT_LITERAL) \\tail *"
RUNNER = '#!/bin/sh\nexec /bin/bash -O extglob "$@"\n'


def require(value, message):
    if not value:
        raise AssertionError(message)


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(65536), b""):
            value.update(block)
    return value.hexdigest()


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


def exercise(binary, expected_hash, tmux, root, evidence, result):
    left, right = root / "left", root / "right"
    left.mkdir()
    right.mkdir()
    document = left / FILENAME
    document.write_bytes(PAYLOAD)
    runner = root / "bash-extglob"
    runner.write_text(RUNNER)
    runner.chmod(0o700)
    ui = root / "ui"
    config = ui / "config/mcpp"
    config.mkdir(parents=True)
    settings = config / "config"
    settings.write_text(
        "pause_after_run=0\nuse_subshell=0\nconfirm_execute=0\nconfirm_exit=0\n"
    )
    output, options = root / "menu.bin", root / "bash.options"
    command = (
        "shopt -p extglob > " + shlex.quote(str(options)) + "\n"
        "printf '%%s\\0' \"$(case abbz-tail in "
        "(@(%f|?(@(a|x))*(b)+(z|q)!(blocked))) "
        "case %f in @('%f'|never)) printf '%%s' \"%{Extglob prompt bytes}\";; "
        "*) printf 'file mismatch';; esac;; *) printf 'pattern mismatch';; esac)\" "
        "\"%f\" '@(?|*)' > " + shlex.quote(str(output))
    )
    menu = left / ".mc.menu"
    menu.write_text("g Extended case literal bytes\n    " + command.replace("\n", "\n    ") + "\n")
    resources = (document, menu, runner)
    result["resource_sha256_before"] = {str(path): digest(path) for path in resources}
    require(digest(binary) == expected_hash, "private binary changed before UI launch")
    arguments = [
        "LANGUAGE=C", "LC_MESSAGES=C", "BASH_ENV=/dev/null", "ENV=/dev/null",
        "SHELL=" + str(runner), str(binary), "-u", "--nokeymap",
        "--", str(left), str(right),
    ]
    result["arguments"] = arguments
    result["runner"] = RUNNER
    with TerminalSession("/usr/bin/env", tmux, ui, arguments, width=120, height=32,
                         record_exit=True, record_stderr=True) as terminal:
        terminal.record_output()
        terminal.wait_for("zz-extglob-data")
        terminal.press("End", "F2")
        terminal.wait_for("Extended case literal bytes")
        capture(terminal, evidence, "extended-case-menu")
        terminal.press("g")
        terminal.wait_for("Extglob prompt bytes")
        terminal.press("C-u")
        terminal.type(ANSWER)
        capture(terminal, evidence, "extended-case-prompt")
        terminal.press("Enter")
        expected = b"".join(value.encode() + b"\0" for value in (ANSWER, FILENAME, "@(?|*)"))
        terminal.wait_file(output, expected)
        terminal.wait_file(options, b"shopt -s extglob\n")
        terminal.wait_for("Extglob prompt bytes", present=False)
        terminal.wait_for("Name")
        capture(terminal, evidence, "extended-case-complete")
        terminal.press("F10")
        terminal.wait_file(terminal.exit_file, b"0\n")
        result["stderr"] = terminal.stderr_file.read_text()
        require(result["stderr"] == "", "unexpected mcpp stderr: " + result["stderr"])
    result["output_hex"] = output.read_bytes().hex()
    result["extglob_state"] = options.read_text()
    result["literal_file_and_prompt_bytes"] = True
    require(document.read_bytes() == PAYLOAD, "the selected source file changed")
    require({path.name for path in left.iterdir()} == {FILENAME, ".mc.menu"} and
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
    if binary_before in STALE_SHA256 or binary_before != arguments.sha256:
        parser.error("do not launch a preserved or unmatched pre-extglob binary")
    os.umask(0o077)
    os.chdir("/review/cases")
    base = Path("l20-menu-extglob-ui")
    private_directory(base)
    root = (base / ("run-" + str(os.getpid()))).absolute()
    root.mkdir()
    evidence = Path("/review/evidence/session/l20-menu-extglob-ui")
    private_directory(evidence)
    evidence = evidence / root.name
    evidence.mkdir()
    os.chdir(root)
    result = {
        "case": "one-extended-case-f2-workflow", "root": str(root),
        "binary": str(binary), "binary_sha256_before": binary_before,
        "native_rerun": False, "model_rerun": False, "passed": False,
    }
    try:
        exercise(binary, arguments.sha256, arguments.tmux, root, evidence, result)
        result["passed"] = True
    except BaseException as error:
        result["error"] = repr(error)
        raise
    finally:
        result["binary_sha256_after"] = digest(binary)
        result["binary_unchanged"] = result["binary_sha256_after"] == binary_before
        before = result.get("resource_sha256_before", {})
        result["resource_sha256_after"] = {
            name: digest(Path(name)) if Path(name).is_file() else None for name in before
        }
        result["resources_unchanged"] = bool(before) and result["resource_sha256_after"] == before
        result["passed"] = result["passed"] and result["binary_unchanged"] and result["resources_unchanged"]
        encoded = json.dumps(result, indent=2) + "\n"
        Path("result.json").write_text(encoded)
        (evidence / "result.json").write_text(encoded)
    require(result["passed"], "the matching binary or literal fixture resources changed")
    print("PASS one extglob F2 workflow: literal prompt/file bytes and enabled Bash")
    print(evidence / "result.json")


if __name__ == "__main__":
    main()
