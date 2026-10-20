#!/usr/bin/env python3
"""One original native control or the reserved matching-build widget-style case."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
from unittest.mock import patch

from terminal_session import SGR, TerminalSession, style_sgr


COMMON = Path("/home/mcpp/src/build/local-parity/mcpp")
COMMON_SHA256 = "507f0060e933236faa8f81224637bfe59d463bae665637ac49c57b069b82eae3"
PREVIOUS_SHA256 = "9b26bfcf9ea036b5b83c0b9622d699a2e5d7ddc97fae86d99f71b759421bccd1"
PAYLOAD = b"ORIGINAL_WIDGET PLAIN B\bB _\bU _\bC\bC\n"
SKIN = b"""[skin]
description=Original viewer and help role fixture
[core]
_default_=lightgray;black;none
selected=black;cyan;none
[viewer]
_default_=lightgray;black;none
viewbold=green;magenta;none
viewunderline=cyan;blue;none
viewselected=black;cyan;none
[help]
_default_=lightgray;black;none
helpbold=green;magenta;none
helpitalic=cyan;red;none
helplink=green;black;none
helpslink=black;cyan;none
"""
COLORS16 = (
    "000000", "800000", "008000", "808000", "000080", "800080", "008080", "c0c0c0",
    "808080", "ff0000", "00ff00", "ffff00", "0000ff", "ff00ff", "00ffff", "ffffff",
)
COLORS = tuple(tuple(bytes.fromhex(value)) for value in COLORS16)
UI_ROLES = {
    "viewbold": ("brightcyan", "magenta", "none", COLORS[14], COLORS[5], 0),
    "viewunderline": ("brightgreen", "blue", "bold+italic+underline", COLORS[10], COLORS[4], 7),
    "helpbold": ("yellow", "magenta", "none", COLORS[11], COLORS[5], 0),
    "helpitalic": ("brightcyan", "red", "none", COLORS[14], COLORS[1], 0),
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def color256(index):
    if index < 16:
        return COLORS[index]
    if index >= 232:
        return (8 + 10 * (index - 232),) * 3
    index -= 16
    levels = (0, 95, 135, 175, 215, 255)
    return levels[index // 36], levels[index // 6 % 6], levels[index % 6]


def cells(ansi):
    foreground = background = None
    active = {name: False for name in ("bold", "italic", "underline", "reverse", "blink", "dim")}
    rows = []
    for line in ansi.splitlines():
        row = []
        for token in SGR.split(line):
            if not token.startswith("\x1b["):
                row.extend({
                    "text": character, "foreground": foreground, "background": background,
                    **active,
                } for character in token)
                continue
            for name, enabled, disabled in (
                    ("bold", 1, 22), ("italic", 3, 23), ("underline", 4, 24),
                    ("reverse", 7, 27), ("blink", 5, 25), ("dim", 2, 22)):
                active[name] = style_sgr(active[name], token, enabled, disabled)
            parameters = token[2:-1].replace(":", ";").split(";")
            index = 0
            while index < len(parameters):
                code = int(parameters[index] or "0")
                index += 1
                if code == 0:
                    foreground = background = None
                elif code in (38, 48):
                    mode = int(parameters[index])
                    index += 1
                    if mode == 5:
                        value = color256(int(parameters[index]))
                        index += 1
                    elif mode == 2:
                        if parameters[index] == "":
                            index += 1
                        value = tuple(map(int, parameters[index:index + 3]))
                        index += 3
                    else:
                        raise AssertionError(("unexpected color mode", mode))
                    if code == 38:
                        foreground = value
                    else:
                        background = value
                elif 30 <= code <= 37:
                    foreground = COLORS[code - 30]
                elif 40 <= code <= 47:
                    background = COLORS[code - 40]
                elif 90 <= code <= 97:
                    foreground = COLORS[code - 90 + 8]
                elif 100 <= code <= 107:
                    background = COLORS[code - 100 + 8]
                elif code == 39:
                    foreground = None
                elif code == 49:
                    background = None
        rows.append(row)
    return rows


def cell_at(rows, needle, offset=0):
    for row in rows:
        column = "".join(cell["text"] for cell in row).find(needle)
        if column >= 0:
            return row[column + offset]
    raise AssertionError(("original text is not visible", needle))


def check_style(cell, foreground, background, attributes):
    assert cell["foreground"] == foreground and cell["background"] == background, cell
    for name, bit in (("bold", 1), ("italic", 2), ("underline", 4), ("reverse", 8), ("blink", 16)):
        assert cell[name] == bool(attributes & bit), (name, cell, attributes)
    assert not cell["dim"], cell
    return cell


def capture(terminal, evidence, name):
    terminal.wait_idle()
    ansi = terminal.text(ansi=True)
    (evidence / (name + ".ansi")).write_text(ansi)
    (evidence / (name + ".txt")).write_text(SGR.sub("", ansi))
    return cells(ansi)


def private_directory(path):
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    state = path.lstat()
    assert stat.S_ISDIR(state.st_mode) and state.st_uid == 1001 and not state.st_mode & 0o077, path


def exercise(arguments, root, evidence, result):
    document = root / "original.nroff"
    resource = root / "original.ini"
    document.write_bytes(PAYLOAD)
    resource.write_bytes(SKIN)
    originals = {str(path): digest(path) for path in (document, resource)}
    native = arguments.selector == "native"
    binary = Path("/review/control/mc") if native else arguments.binary.resolve(strict=True)
    environment = {
        "LD_LIBRARY_PATH": "/review/control/lib", "LANGUAGE": "C", "LC_MESSAGES": "C",
        "COLORTERM": "truecolor",
    }
    command = ["-u", "-c", "-S", resource]
    if not native:
        command += ["--nokeymap", "-C", ":".join(
            role + "=" + ",".join(style[:3]) for role, style in UI_ROLES.items())]
    command += ["-v", document]
    result.update({"arguments": list(map(str, command)), "resources_before": originals})
    observations = {}
    result["observations"] = observations
    with patch.dict(os.environ, environment):
        if native:
            version = subprocess.run([binary, "--version"], cwd=root, check=True,
                                     capture_output=True, text=True, timeout=10).stdout
            assert version.startswith("GNU Midnight Commander 4.8.30\n"), version
            help_color = subprocess.run([binary, "--help-color"], cwd=root, check=True,
                                        capture_output=True, text=True, timeout=10).stdout
            assert all(role in help_color for role in UI_ROLES), help_color
            assert "KEYWORD={FORE},{BACK},{ATTR}" in help_color
            (evidence / "native-help-color.txt").write_text(help_color)
            result["version"] = version.splitlines()[0]
        with TerminalSession(binary, arguments.tmux, root / "ui", command,
                             width=100, height=30, record_exit=True, record_stderr=True) as terminal:
            terminal.wait_for("ORIGINAL_WIDGET")
            if "Unform" not in terminal.text().splitlines()[-1]:
                terminal.press("F9")
            terminal.wait_for("ORIGINAL_WIDGET PLAIN B U C")
            rows = capture(terminal, evidence, "formatted-viewer")
            phrase = "PLAIN B U C"
            for role, offset in (("viewbold", phrase.index("B")), ("viewunderline", phrase.index("U"))):
                style = (COLORS[2], COLORS[5], 0) if role == "viewbold" else (COLORS[6], COLORS[4], 0)
                if not native:
                    style = UI_ROLES[role][3:]
                observations[role] = check_style(cell_at(rows, phrase, offset), *style)
            observations["combined_nroff"] = cell_at(rows, phrase, phrase.index("C"))
            if not native:
                terminal.press("F1")
                terminal.wait_for("mcpp help:")
                terminal.press("F1")
                terminal.wait_for("Moving around")
                rows = capture(terminal, evidence, "original-help")
                for role, text in (("helpbold", "Moving around"), ("helpitalic", " Lines ")):
                    observations[role] = check_style(cell_at(rows, text), *UI_ROLES[role][3:])
                terminal.press("F2")
                terminal.wait_for("Using this help browser")
                rows = capture(terminal, evidence, "selected-help-link")
                observations["selected_help_link"] = check_style(
                    cell_at(rows, "Using this help browser"), COLORS[0], COLORS[6], 4)
                terminal.press("F10")
                terminal.wait_for("mcpp help:", present=False)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
            result["stderr"] = terminal.stderr_file.read_text()
            if not native:
                assert result["stderr"] == "", result["stderr"]
    assert document.read_bytes() == PAYLOAD and resource.read_bytes() == SKIN
    assert {str(path): digest(path) for path in (document, resource)} == originals
    result.update({"resources_unchanged": True, "literal_bytes_unchanged": True,
                   "literal_bytes_hex": PAYLOAD.hex()})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("selector", choices=("native", "ui"))
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--sha256")
    parser.add_argument("--tmux", default="/usr/bin/tmux")
    arguments = parser.parse_args()
    if (os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file()):
        parser.error("requires UID1001 in the owned disposable review container")
    if arguments.selector == "ui":
        if arguments.binary is None or not re.fullmatch("[0-9a-f]{64}", arguments.sha256 or ""):
            parser.error("the reserved UI case requires the parent's matching binary and SHA256")
        arguments.binary = arguments.binary.resolve(strict=True)
        if (arguments.binary == COMMON.resolve() or digest(arguments.binary) in
                (COMMON_SHA256, PREVIOUS_SHA256) or digest(arguments.binary) != arguments.sha256):
            parser.error("do not launch a preserved or unmatched pre-change binary")
    common_before = digest(COMMON)
    assert common_before == COMMON_SHA256, "the preserved common binary changed"
    os.umask(0o077)
    os.chdir("/review/cases")
    base = Path("viewer-help-colors") / arguments.selector
    private_directory(base)
    root = (base / ("run-" + str(os.getpid()))).absolute()
    root.mkdir()
    evidence = Path("/review/evidence/session/viewer-help-colors") / arguments.selector
    private_directory(evidence)
    evidence = evidence / root.name
    evidence.mkdir()
    os.chdir(root)
    result = {"selector": arguments.selector, "root": str(root), "status": "failed",
              "binary": "/review/control/mc" if arguments.selector == "native" else str(arguments.binary),
              "sha256": None if arguments.selector == "native" else arguments.sha256}
    try:
        exercise(arguments, root, evidence, result)
        assert digest(COMMON) == common_before, "the preserved common binary changed"
        result["status"] = "passed-with-native-warning" if result.get("stderr") else "passed"
    finally:
        (evidence / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(arguments.selector + ": " + result["status"] + "; " + str(evidence / "result.json"))


if __name__ == "__main__":
    main()
