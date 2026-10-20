#!/usr/bin/env python3
"""An original native syntax-format control, without installed rule fixtures."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re

from terminal_session import TerminalSession


REGISTRY = "file . Original\\sNative\\sFormat\ninclude original-format.syntax\n"
DEFINITION = r"""define accent brightgreen black
context default red black
 keyword whole whole\[\] accent
 keyword plain
context [ ] brightmagenta black
 keyword reset
"""
TEXT = "whole plain [reset] wholeword\n"
EXPECTED = [10] * 5 + [1] * 7 + [13] * 7 + [1] * 10
SGR = re.compile(r"(\x1b\[[0-9;:]*m)")


def colors_for(ansi, needle):
    foreground = None
    for line in ansi.splitlines():
        text, colors = "", []
        for token in SGR.split(line):
            if not token.startswith("\x1b["):
                text += token
                colors.extend([foreground] * len(token))
                continue
            parameters = [int(value or "0") for value in token[2:-1].split(";")]
            cursor = 0
            while cursor < len(parameters):
                value = parameters[cursor]
                cursor += 1
                if value in (0, 39):
                    foreground = None
                elif 30 <= value <= 37:
                    foreground = value - 30
                elif 90 <= value <= 97:
                    foreground = value - 90 + 8
                elif value in (38, 48):
                    mode = parameters[cursor]
                    cursor += 1
                    if value == 38:
                        foreground = parameters[cursor] if mode == 5 else None
                    cursor += 3 if mode == 2 else 1
        if needle in text:
            start = text.index(needle)
            return colors[start:start + len(needle)]
    raise AssertionError(f"missing syntax-format fixture: {needle!r}")


def main():
    if (os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file()):
        raise SystemExit("requires uid1001 in the owned disposable container")
    parser = argparse.ArgumentParser()
    parser.add_argument("--mcpp", type=Path)
    parser.add_argument("--sha256")
    arguments = parser.parse_args()
    native = arguments.mcpp is None
    if not native and not arguments.sha256:
        parser.error("--mcpp requires --sha256")
    binary = Path("/review/control/mc") if native else arguments.mcpp
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    if arguments.sha256 and digest != arguments.sha256:
        raise SystemExit(f"binary hash differs: {digest}")
    base = Path("/review/cases") / ("native-syntax-format" if native else "mcpp-syntax-format")
    base.mkdir(exist_ok=True)
    if base.is_symlink() or base.stat().st_uid != os.geteuid():
        raise SystemExit("fixture base must be owned and not a symlink")
    root = base / ("run-" + str(os.getpid()))
    root.mkdir()
    fixture = root / "original.fmt"
    fixture.write_text(TEXT)
    ui = root / "ui"
    config = ui / "config" / ("mc" if native else "mcpp")
    config.mkdir(parents=True)
    if native:
        definitions = ui / "data/mc/syntax"
        definitions.mkdir(parents=True)
        (config / "ini").write_text(
            "[Midnight-Commander]\nuse_subshell=0\nconfirm_exit=0\n"
            "editor_syntax_highlighting=true\n"
        )
    else:
        definitions = config
        (config / "config").write_text(
            "auto_save_setup=false\neditor_line_numbers=false\neditor_syntax=true\n"
        )
    (definitions / "Syntax").write_text(REGISTRY)
    (definitions / "original-format.syntax").write_text(DEFINITION)
    os.environ["LD_LIBRARY_PATH"] = "/review/control/lib"
    command = ["-u", "-e", fixture] if native else ["--edit", fixture]
    with TerminalSession(binary, "/usr/bin/tmux", ui, command, record_exit=True) as terminal:
        terminal.wait_for("wholeword")
        terminal.press("C-End")
        terminal.wait_idle()
        terminal.save("native-format")
        colors = colors_for(terminal.text(ansi=True), TEXT.rstrip("\n"))
        (root / "colors.json").write_text(json.dumps(
            {"binary": str(binary), "sha256": digest, "colors": colors}, indent=2
        ) + "\n")
        print(json.dumps(colors))
        terminal.press("F10")
        terminal.wait_file(terminal.exit_file, b"0\n")
    assert fixture.read_text() == TEXT, "syntax control changed fixture bytes"
    assert colors == EXPECTED, f"native syntax-format colors changed: {colors!r}"
    print(("PASS native MC 4.8.30" if native else "PASS mcpp " + digest) +
          ": empty repeated sets and colorless keywords")
    print(root)


if __name__ == "__main__":
    main()
