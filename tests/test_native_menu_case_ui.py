#!/usr/bin/env python3
"""One hash-pinned mcpp UI workflow: literal case data through F2 and extensions."""

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
    "2b9cd9fd731e39315ce462319ee833854aac436fc5760d44c4147d6682c1afb9",
    "9b26bfcf9ea036b5b83c0b9622d699a2e5d7ddc97fae86d99f71b759421bccd1",
}
FILENAME = "zz-case-data café ' \" ) ;; ;& ;;& esac $(printf FILE_DATA) `printf FILE_TICK`\\tail\n*.case"
PAYLOAD = b"UNCHANGED L20 CASE UI SOURCE\n"
MENU_ANSWER = "menu ' \" ) ;; ;& ;;& esac $(printf MENU_DATA) `printf MENU_TICK`\\tail *"
EXTENSION_ANSWER = "extension ' \" ) ;; ;& ;;& esac $(printf EXT_DATA) `printf EXT_TICK`\\tail *"


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
    ui = root / "ui"
    config = ui / "config/mcpp"
    config.mkdir(parents=True)
    (config / "config").write_text(
        "pause_after_run=0\nuse_subshell=0\nconfirm_execute=0\nconfirm_exit=0\n"
    )
    menu_output, extension_output = root / "menu.bin", root / "extension.bin"
    menu_command = (
        "printf '%%s\\0' \"$(case %f in (%f) printf '%%s' \"%{Case menu input}\";; "
        "*) printf 'pattern mismatch';; esac)\" \"%f\" > " + shlex.quote(str(menu_output))
    )
    extension_command = (
        "printf '%%s\\0' \"$(case %f in %f) case literal in (literal) "
        "printf '%%s' '%{Case extension input}';; esac;; "
        "*) printf 'pattern mismatch';; esac)\" \"%f\" > " + shlex.quote(str(extension_output))
    )
    menu = left / ".mc.menu"
    menu.write_text("c Case menu literal bytes\n    " + menu_command + "\n")
    extension = config / "mc.ext.ini"
    extension.write_text(
        "[mc.ext.ini]\nVersion=4.0\n[Literal case]\nShell=.case\nOpen=" +
        extension_command.replace("\\", "\\\\") + "\n"
    )
    result["resource_sha256_before"] = {
        str(path): digest(path) for path in (document, menu, extension)
    }
    result["observations"] = {}
    require(digest(binary) == expected_hash, "private binary changed before UI launch")
    arguments = ["LANGUAGE=C", "LC_MESSAGES=C", str(binary), "-u", "--nokeymap",
                 "--", str(left), str(right)]
    result["arguments"] = arguments
    with TerminalSession("/usr/bin/env", tmux, ui, arguments, width=120, height=32,
                         record_exit=True, record_stderr=True) as terminal:
        terminal.record_output()
        terminal.wait_for("zz-case-data")
        for phase, key, prompt, answer, output in (
                ("menu", "F2", "Case menu input", MENU_ANSWER, menu_output),
                ("extension", "Enter", "Case extension input", EXTENSION_ANSWER, extension_output)):
            terminal.press("End", key)
            if phase == "menu":
                terminal.wait_for("Case menu literal bytes")
                terminal.press("c")
            terminal.wait_for(prompt)
            terminal.press("C-u")
            terminal.type(answer)
            capture(terminal, evidence, phase + "-literal-input")
            terminal.press("Enter")
            expected = b"".join(value.encode() + b"\0" for value in (answer, FILENAME))
            terminal.wait_file(output, expected)
            terminal.wait_for(prompt, present=False)
            terminal.wait_for("Name")
            result["observations"][phase] = {
                "prompt": prompt, "answer": answer, "output_hex": output.read_bytes().hex(),
                "literal_file_and_prompt_bytes": True,
            }
        capture(terminal, evidence, "case-actions-complete")
        terminal.press("F10")
        terminal.wait_file(terminal.exit_file, b"0\n")
        result["stderr"] = terminal.stderr_file.read_text()
        require(result["stderr"] == "", "unexpected mcpp stderr: " + result["stderr"])
    require(document.read_bytes() == PAYLOAD, "the selected source file changed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="parent-approved new mcpp binary")
    parser.add_argument("--tmux", nargs="?", const="/usr/bin/tmux", default="/usr/bin/tmux")
    parser.add_argument("--sha256", required=True, help="parent-provided exact matching binary SHA256")
    arguments = parser.parse_args()
    if (os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file()):
        parser.error("requires UID1001 in the owned disposable review container")
    if not re.fullmatch("[0-9a-f]{64}", arguments.sha256):
        parser.error("--sha256 must be the parent's exact lowercase SHA256")
    binary = arguments.binary.resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error("the supplied binary must be a regular executable")
    binary_before = digest(binary)
    if binary_before in STALE_SHA256 or binary_before != arguments.sha256:
        parser.error("do not launch a preserved or unmatched pre-case binary")
    os.umask(0o077)
    os.chdir("/review/cases")
    base = Path("l20-menu-case-ui")
    private_directory(base)
    root = (base / ("run-" + str(os.getpid()))).absolute()
    root.mkdir()
    evidence = Path("/review/evidence/session/l20-menu-case-ui")
    private_directory(evidence)
    evidence = evidence / root.name
    evidence.mkdir()
    os.chdir(root)
    result = {
        "case": "one-modern-case-menu-and-extension-ui", "root": str(root),
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
    print("PASS one case UI workflow: F2 and extension preserve literal file/prompt bytes")
    print(evidence / "result.json")


if __name__ == "__main__":
    main()
