#!/usr/bin/env python3
"""One original syntax fixture for native MC or a hash-pinned mcpp release."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re

from terminal_session import TerminalSession


SYNTAX = r"""file . Original\sByte\sSyntax
caseinsensitive
context default lightgray black
 keyword ascii brightred
 keyword café brightgreen
 keyword \{é\}X yellow
 keyword R\[éλ\]Y brightcyan
context <é> </é> brightmagenta black
 spellcheck
"""

TEXT = "ASCII café CAFé CAFÉ éX ΩX RéλY RûΩY\n<é>inside</é> <É>outside</É>\n"
EXPECTED = {
    "ASCII": [9] * 5,
    "café": [10] * 4,
    "CAFé": [10] * 4,
    "CAFÉ": [7] * 4,
    "éX": [7, 11],
    "ΩX": [7, 11],
    "RéλY": [14] * 4,
    "RûΩY": [14] * 4,
    "<é>inside</é>": [13] * 13,
    "<É>outside</É>": [7] * 14,
}
SGR = re.compile(r"(\x1b\[[0-9;:]*m)")


def color_rows(ansi):
    foreground = None
    rows = []
    for line in ansi.splitlines():
        text = ""
        colors = []
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
                if value == 0 or value == 39:
                    foreground = None
                elif 30 <= value <= 37:
                    foreground = value - 30
                elif 90 <= value <= 97:
                    foreground = value - 90 + 8
                elif value in (38, 48):
                    mode = parameters[cursor]
                    cursor += 1
                    count = 3 if mode == 2 else 1
                    if value == 38:
                        foreground = parameters[cursor] if mode == 5 else None
                    cursor += count
        rows.append((text, colors))
    return rows


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
    base = Path("/review/cases") / ("native-syntax-byte-probe" if native else "mcpp-syntax-byte-comparison")
    base.mkdir(exist_ok=True)
    if base.is_symlink() or base.stat().st_uid != os.geteuid():
        raise SystemExit("fixture base must be owned and not a symlink")
    root = base / ("run-" + str(os.getpid()))
    root.mkdir()
    (root / "binary.json").write_text(json.dumps(
        {"binary": str(binary), "sha256": digest}, indent=2
    ) + "\n")
    fixture = root / "original.l22"
    fixture.write_text(TEXT)
    ui = root / "ui"
    config = ui / "config" / ("mc" if native else "mcpp")
    config.mkdir(parents=True)
    if native:
        definitions = ui / "data/mc/syntax"
        definitions.mkdir(parents=True)
        (definitions / "Syntax").write_text(SYNTAX)
        (config / "ini").write_text(
            "[Midnight-Commander]\nuse_subshell=0\nconfirm_exit=0\n"
            "editor_syntax_highlighting=true\n"
            "[Misc]\ndisplay_codepage=UTF-8\nsource_codepage=UTF-8\n"
        )
    else:
        (config / "Syntax").write_text(SYNTAX)
        (config / "config").write_text(
            "auto_save_setup=false\neditor_line_numbers=false\neditor_syntax=true\n"
        )
    os.environ["LD_LIBRARY_PATH"] = "/review/control/lib"
    command = ["-u", "-e", fixture] if native else ["--edit", fixture]
    with TerminalSession(binary, "/usr/bin/tmux", ui, command, record_exit=True) as terminal:
        terminal.wait_for("RûΩY")
        terminal.press("C-End")
        terminal.wait_idle()
        terminal.save("native-byte-syntax")
        rows = color_rows(terminal.text(ansi=True))
        assert any(TEXT.splitlines()[0] in row for row, _ in rows), (
            "syntax color boundaries changed displayed UTF-8 text"
        )
        evidence = {}
        for needle in EXPECTED:
            row, colors = next((row, colors) for row, colors in rows if needle in row)
            start = row.index(needle)
            evidence[needle] = colors[start:start + len(needle)]
        (root / "colors.json").write_text(json.dumps(
            {"binary": str(binary), "sha256": digest, "colors": evidence}, ensure_ascii=False, indent=2
        ) + "\n")
        print(json.dumps(evidence, ensure_ascii=False))
        terminal.press("F10")
        terminal.wait_file(terminal.exit_file, b"0\n")
    assert evidence == EXPECTED, f"MC-equivalent byte-syntax behavior changed: {evidence!r}"
    assert fixture.read_text() == TEXT, "syntax comparison changed fixture bytes"
    print(("PASS native MC 4.8.30" if native else "PASS mcpp " + digest) +
          ": ASCII folding, exact UTF-8 literals/contexts, byte pattern sets")
    print(root)


if __name__ == "__main__":
    main()
