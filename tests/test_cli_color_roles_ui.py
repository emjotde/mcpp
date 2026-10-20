#!/usr/bin/env python3
"""One launch-only color-role UI case against a parent-approved private binary."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
from unittest.mock import patch

from terminal_session import SGR, TerminalSession
from test_skins_ui import cells


COMMON = Path("/home/mcpp/src/build/local-parity/mcpp")
COMMON_SHA256 = "507f0060e933236faa8f81224637bfe59d463bae665637ac49c57b069b82eae3"
PAYLOAD = "COLOR_LITERAL café ┌│\nsecond literal +notes:12\n".encode()
FILENAME = "literal-┌│-café.txt"
MENU = "a Selected original entry\n    :\nb Literal café ┌│ entry\n    :\n".encode()
SKIN = b"""[skin]
description=Original CLI role UI fixture
[core]
_default_=lightgray;#101820;none
selected=black;cyan;none
[menu]
_default_=white;#304050;none
menusel=black;cyan;none
menuhot=yellow;;none
[popupmenu]
_default_=white;#203040;none
menusel=yellow;#502030;none
menutitle=brightcyan;#203040;none
[dialog]
_default_=white;#405060;none
dfocus=black;cyan;none
[editor]
_default_=lightgray;#101820;none
editlinestate=white;blue;none
"""
PREFERENCES = {"theme": "gray", "use_subshell": "true", "editor_line_numbers": "false"}
STYLES = {
    "pmenunormal": ("cfe3f5", "132b44", "underline"),
    "pmenutitle": ("ffe188", "3a174f", "bold+underline"),
    "pmenusel": ("cde2f7", "294862", "none"),
    "editlinestate": ("ffcb70", "4f2d50", "bold+underline"),
    "dfocus": ("ffffff", "285c88", "bold+underline"),
}


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(65536), b""):
            result.update(block)
    return result.hexdigest()


class Capture:
    def __init__(self, ansi):
        self.ansi = ansi

    def text(self, ansi=False):
        return self.ansi if ansi else SGR.sub("", self.ansi)


def capture(terminal, name, evidence):
    terminal.wait_idle()
    ansi = terminal.text(ansi=True)
    screen = Capture(ansi)
    (evidence / (name + ".ansi")).write_text(ansi)
    (evidence / (name + ".txt")).write_text(screen.text())
    return cells(screen, include_style=True)


def text_cell(rows, needle, after=None):
    start = 0
    if after is not None:
        start = next(index + 1 for index, row in enumerate(rows)
                     if after in "".join(cell[0] for cell in row))
    for row_number, row in enumerate(rows[start:], start):
        column = "".join(cell[0] for cell in row).find(needle)
        if column >= 0:
            return row_number, column, row[column]
    raise AssertionError(f"Literal text not visible: {needle!r}")


def check_style(cell, role):
    foreground, background, attributes = STYLES[role]
    expected = (tuple(bytes.fromhex(foreground)), tuple(bytes.fromhex(background)))
    assert cell[1:3] == expected, (role, cell, expected)
    expected_bold = "bold" in attributes.split("+")
    expected_underline = "underline" in attributes.split("+")
    assert cell[3] == {
        "dim": False, "bold": expected_bold, "underlined": expected_underline,
    }, (role, cell)
    return {"foreground": cell[1], "background": cell[2], "attributes": cell[3]}


def saved_preferences(path):
    values = {}
    for line in path.read_text().splitlines():
        key, separator, value = line.partition("=")
        if separator and key.strip() in PREFERENCES:
            values[key.strip()] = value.strip()
    return values


def exercise(binary, tmux, root, evidence, result):
    left, right, ui = root / "left", root / "right", root / "ui"
    document = left / FILENAME
    skin = root / "original.ini"
    config = ui / "config/mcpp"
    left.mkdir()
    right.mkdir()
    config.mkdir(parents=True)
    document.write_bytes(PAYLOAD)
    skin.write_bytes(SKIN)
    menu = config / "menu"
    menu.write_bytes(MENU)
    preferences = config / "config"
    preferences.write_text("".join(f"{key}={value}\n" for key, value in PREFERENCES.items()))
    originals = {str(path): digest(path) for path in (document, skin, menu)}
    preference_hash_before = digest(preferences)
    colors = ":".join(f"{role}=#{foreground},#{background},{attributes}"
                      for role, (foreground, background, attributes) in STYLES.items())
    arguments = ["-u", "-c", "--nokeymap", "-S", skin, "-C", colors, "--", left, right]
    observations = {}
    result.update({
        "observed_styles": observations,
        "cli_arguments": list(map(str, arguments)),
        "literal_bytes_hex": PAYLOAD.hex(),
        "resource_sha256_before": originals,
        "saved_preferences_before": PREFERENCES,
        "preference_file_sha256_before": preference_hash_before,
    })
    with patch.dict(os.environ, {"COLORTERM": "truecolor", "LANGUAGE": "C", "LC_MESSAGES": "C"}):
        with TerminalSession(binary, tmux, ui, arguments, width=110, height=30,
                             record_exit=True, record_stderr=True) as terminal:
            terminal.wait_for(FILENAME)
            terminal.press("F2")
            terminal.wait_for("Literal café ┌│ entry")
            rows = capture(terminal, "f2-popup", evidence)
            for role, label in (
                ("pmenunormal", "Literal café ┌│ entry"),
                ("pmenutitle", "User menu"),
                ("pmenusel", "Selected original entry"),
            ):
                observations[role] = check_style(text_cell(rows, label)[2], role)
            terminal.press("Escape")
            terminal.wait_for("User menu", present=False)

            terminal.press("F7")
            terminal.wait_for("Directory name:")
            rows = capture(terminal, "dialog-focus", evidence)
            observations["dfocus"] = check_style(
                text_cell(rows, "> ", after="Directory name:")[2], "dfocus")
            terminal.press("Escape")
            terminal.wait_for("Directory name:", present=False)

            terminal.press("Home", "Down", "F4")
            terminal.wait_editor(dirty=False)
            terminal.wait_for(PAYLOAD.decode().splitlines()[0])
            terminal.press("M-n")
            terminal.wait(lambda: text_cell(cells(terminal, include_style=True), "COLOR_LITERAL")[1] >= 6,
                          "the editor line-number gutter is visible")
            rows = capture(terminal, "editor-gutter", evidence)
            row_number, column, _ = text_cell(rows, "COLOR_LITERAL")
            gutter = "".join(cell[0] for cell in rows[row_number][:column])
            digit = gutter.find("1")
            assert digit >= 0, ("line number 1 is missing", gutter)
            observations["editlinestate"] = check_style(rows[row_number][digit], "editlinestate")
            assert PAYLOAD.decode().splitlines()[0] in "".join(cell[0] for cell in rows[row_number])
            terminal.press("M-n")
            terminal.wait(lambda: text_cell(cells(terminal, include_style=True), "COLOR_LITERAL")[1] < 6,
                          "line numbers return to the saved disabled setting")
            terminal.press("F10")
            terminal.wait_for("COLOR_LITERAL", present=False)
            terminal.wait_for(FILENAME)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
            assert terminal.stderr_file.read_bytes() == b"", terminal.stderr_file.read_text()
    assert document.read_bytes() == PAYLOAD, "the literal document bytes changed"
    assert skin.read_bytes() == SKIN and menu.read_bytes() == MENU, "an original color/menu resource changed"
    assert {str(path): digest(path) for path in (document, skin, menu)} == originals
    assert list(right.iterdir()) == [] and [path.name for path in left.iterdir()] == [FILENAME]
    persisted = saved_preferences(preferences)
    assert persisted == PREFERENCES, (persisted, PREFERENCES)
    generated = {}
    for name in ("config", "ini"):
        path = config / name
        if path.is_file():
            contents = path.read_text()
            assert str(skin) not in contents, "the launch-only skin path was saved"
            assert all("#" + color not in contents for style in STYLES.values() for color in style[:2])
            assert all(role + "=" not in contents for role in STYLES), "a launch color role was saved"
            generated[name] = digest(path)
    return {
        "observed_styles": observations,
        "cli_arguments": list(map(str, arguments)),
        "literal_bytes_hex": PAYLOAD.hex(),
        "literal_bytes_unchanged": True,
        "resource_sha256": originals,
        "resources_unchanged": True,
        "saved_preferences_before": PREFERENCES,
        "saved_preferences_after": persisted,
        "saved_settings_unchanged": True,
        "preference_file_sha256_before": preference_hash_before,
        "preference_file_sha256_after": digest(preferences),
        "generated_configuration_sha256": generated,
        "saved_color_overrides": False,
    }


def private_directory(path):
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    state = path.lstat()
    assert stat.S_ISDIR(state.st_mode) and state.st_uid == 1001 and not state.st_mode & 0o077, path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="parent-approved private mcpp binary")
    parser.add_argument("--tmux", nargs="?", const="/usr/bin/tmux", default="/usr/bin/tmux")
    parser.add_argument("--sha256", required=True, help="parent-provided exact private binary SHA256")
    arguments = parser.parse_args()
    if (os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file()):
        parser.error("requires uid1001 in the owned disposable container")
    if not re.fullmatch("[0-9a-f]{64}", arguments.sha256):
        parser.error("--sha256 must be the parent's exact lowercase SHA256")
    binary = arguments.binary.resolve(strict=True)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error("the private binary must be a regular executable")
    binary_before = digest(binary)
    if binary == COMMON.resolve() or binary_before == COMMON_SHA256:
        parser.error("do not run this case on the pinned common binary")
    if binary_before != arguments.sha256:
        parser.error("the private binary does not match the parent's approved SHA256")
    common_before = digest(COMMON)
    if common_before != COMMON_SHA256:
        parser.error("the pinned common binary changed; stop before launching any UI")
    os.umask(0o077)
    os.chdir("/review/cases")
    base = Path("cli-color-roles/ui")
    private_directory(base)
    root = (base / ("run-" + str(os.getpid()))).absolute()
    root.mkdir()
    evidence = Path("/review/evidence/session/cli-color-roles/ui")
    private_directory(evidence)
    evidence = evidence / root.name
    evidence.mkdir()
    os.chdir(root)
    result = {
        "case": "fourth-single-cli-color-role-actual-ui",
        "binary": str(binary), "binary_sha256_before": binary_before,
        "common_binary_sha256_before": common_before, "root": str(root),
        "native_rerun": False, "model_rerun": False, "passed": False,
    }
    try:
        result.update(exercise(binary, arguments.tmux, root, evidence, result))
        result["passed"] = True
    except BaseException as error:
        result["error"] = repr(error)
        raise
    finally:
        result["binary_sha256_after"] = digest(binary)
        result["common_binary_sha256_after"] = digest(COMMON)
        result["binaries_unchanged"] = (
            result["binary_sha256_after"] == binary_before and
            result["common_binary_sha256_after"] == common_before
        )
        if "resource_sha256_before" in result:
            result["resource_sha256_after"] = {
                name: digest(Path(name)) if Path(name).is_file() else None
                for name in result["resource_sha256_before"]
            }
            result["resources_unchanged"] = (
                result["resource_sha256_after"] == result["resource_sha256_before"]
            )
            document = root / "left" / FILENAME
            result["literal_bytes_unchanged"] = document.is_file() and document.read_bytes() == PAYLOAD
            preferences = root / "ui/config/mcpp/config"
            result["saved_preferences_after"] = saved_preferences(preferences) if preferences.is_file() else {}
            result["saved_settings_unchanged"] = result["saved_preferences_after"] == PREFERENCES
            result["passed"] = result["passed"] and all(result[key] for key in (
                "resources_unchanged", "literal_bytes_unchanged", "saved_settings_unchanged",
            ))
        result["passed"] = result["passed"] and result["binaries_unchanged"]
        encoded = json.dumps(result, indent=2) + "\n"
        Path("result.json").write_text(encoded)
        (evidence / "result.json").write_text(encoded)
    assert result["passed"], "the supplied private or pinned common binary changed"
    print("PASS single CLI color UI: F2 normal/title/selection, dialog focus, editor gutter, literal bytes")
    print(evidence / "result.json")


if __name__ == "__main__":
    main()
