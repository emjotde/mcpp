#!/usr/bin/env python3
"""One hash-pinned editor display workflow; the native controls stay cached."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import unicodedata

from terminal_session import TerminalSession


BASE = Path("/review/cases/mc-config/editor-display-500191ad")
EVIDENCE = Path("/review/evidence/session")
OLD_BINARIES = {
    "03687bc3f3b9eca7209546264abdfa7f7bbc9e703783fbc54a2556316ec466f8",
    "e886331864b03066cf2eac1343b2835c7ec8b92a264bfab4982378bb3aa12b5c",
}
TEXT = "0123456789ABCDEFGHIJKLMNOPQ\nx\n\tA界éZ\n"
MARGIN = ((173, 93, 23), (20, 41, 58))
MARKED = ((5, 26, 42), (179, 212, 223))
SGR = re.compile(r"(\x1b\[[0-9;:]*m)")
SKIN = """[skin]
description=Original editor display fixture
[core]
_default_=#cfd6dd;#13294d;none
[editor]
_default_=#cfd6dd;#13294d;none
editrightmargin=#ad5d17;#14293a;none
editmarked=#051a2a;#b3d4df;none
editlinestate=#7fb49a;#13294d;none
"""


def cells(ansi):
    foreground, background, bold = None, None, False
    rows = []
    for line in ansi.splitlines():
        row = []
        for token in SGR.split(line):
            if not token.startswith("\x1b["):
                for glyph in token:
                    if unicodedata.combining(glyph):
                        continue
                    width = 2 if unicodedata.east_asian_width(glyph) in ("W", "F") else 1
                    row.extend([(foreground, background, bold)] * width)
                continue
            parameters = [int(value or "0") for value in token[2:-1].split(";")]
            cursor = 0
            while cursor < len(parameters):
                value = parameters[cursor]
                cursor += 1
                if value == 0:
                    foreground, background, bold = None, None, False
                elif value in (1, 22):
                    bold = value == 1
                elif value in (39, 49):
                    if value == 39:
                        foreground = None
                    else:
                        background = None
                elif 30 <= value <= 37 or 90 <= value <= 97:
                    foreground = value - (30 if value < 90 else 82)
                elif 40 <= value <= 47 or 100 <= value <= 107:
                    background = value - (40 if value < 100 else 92)
                elif value in (38, 48):
                    mode = parameters[cursor]
                    cursor += 1
                    count = 3 if mode == 2 else 1
                    color = tuple(parameters[cursor:cursor + count])
                    cursor += count
                    if value == 38:
                        foreground = color
                    else:
                        background = color
        rows.append(row)
    return rows


def main():
    if (os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file()):
        raise SystemExit("requires UID1001 in the owned disposable container")
    parser = argparse.ArgumentParser()
    parser.add_argument("--mcpp", required=True, type=Path)
    parser.add_argument("--sha256", required=True)
    arguments = parser.parse_args()
    binary = arguments.mcpp.resolve()
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    if digest != arguments.sha256 or digest in OLD_BINARIES:
        raise SystemExit("requires the parent's new matching display-preferences binary and hash")
    if BASE.is_symlink() or BASE.stat().st_uid != os.geteuid():
        raise SystemExit("fixture base must be owned and not a symlink")
    cached_native = EVIDENCE / "editor-display-500191ad-native.json"
    native = json.loads(cached_native.read_text())
    assert native["correct_native_wrap_key"] == "editor_word_wrap_line_length"
    assert native["configured_margin_control"]["wrap_column"] == 12
    result_path = EVIDENCE / f"editor-display-500191ad-ui-{digest[:16]}.json"
    if result_path.is_file():
        previous = json.loads(result_path.read_text())
        if previous.get("status") == "PASS":
            assert previous["sha256"] == digest
            print(f"Using cached matching display workflow: {result_path}")
            return
        raise SystemExit("an incomplete workflow exists; review it before allocating another case")
    root = BASE / ("ui-" + digest[:16])
    root.mkdir()
    assert not root.is_symlink() and root.stat().st_uid == os.geteuid()
    fixture = root / "original.txt"
    fixture.write_text(TEXT)
    ui = root / "ui"
    config = ui / "config/mcpp"
    config.mkdir(parents=True)
    resource = config / "original-display.ini"
    resource.write_text(SKIN)
    (config / "Syntax").write_text(
        "file . Original\\sEditor\\sDisplay\ninclude original-display.syntax\n"
    )
    (config / "original-display.syntax").write_text(
        "context default white blue\n keyword 0123456789AB brightgreen blue\n"
    )
    settings = (
        "[Midnight-Commander]\nauto_save_setup=false\neditor_line_state=true\n"
        "editor_syntax_highlighting=true\neditor_word_wrap_line_length=12\n"
        f"skin={resource}\n"
    )
    ini = config / "ini"
    ini.write_text(settings)
    os.environ["LD_LIBRARY_PATH"] = "/review/control/lib"
    os.environ["COLORTERM"] = "truecolor"
    result = {
        "status": "running", "binary": str(binary), "sha256": digest,
        "cached_native": str(cached_native), "native_runs": 0,
        "fixture_sha256": hashlib.sha256(fixture.read_bytes()).hexdigest(), "phases": [],
    }
    result_path.write_text(json.dumps(result, indent=2) + "\n")
    command = [
        "--foreground", "--signal=TERM", "--kill-after=1", "20",
        str(binary), "--edit", str(fixture),
    ]
    with TerminalSession("/usr/bin/timeout", "/usr/bin/tmux", ui, command,
                         width=100, height=14, record_exit=True, record_stderr=True) as terminal:
        terminal.wait_for("0123456789ABCDEFGHIJKLMNOPQ", timeout=4)
        terminal.wait_idle()
        terminal.save("legacy")
        baseline = terminal.text().splitlines()
        baseline_colors = cells(terminal.text(ansi=True))
        assert "[----]" in baseline[0] and " L:[" in baseline[0]
        gutter = baseline[1].index("0123456789ABCDEFGHIJKLMNOPQ")
        assert gutter == 6
        assert baseline_colors[1][gutter + 12][:2] != MARGIN
        terminal.press("F10")
        terminal.wait_file(terminal.exit_file, b"0\n")
    assert fixture.read_text() == TEXT
    assert "editor_simple_statusbar" not in ini.read_text()
    assert "editor_show_right_margin" not in ini.read_text()
    result["phases"].append({"name": "absent-retains-legacy", "exit": 0})
    result_path.write_text(json.dumps(result, indent=2) + "\n")
    ini.write_text(settings + "editor_simple_statusbar=true\neditor_show_right_margin=true\n")
    enabled_settings = ini.read_bytes()
    with TerminalSession("/usr/bin/timeout", "/usr/bin/tmux", ui, command,
                         width=100, height=14, record_exit=True, record_stderr=True) as terminal:
        terminal.wait_for("0123456789ABCDEFGHIJKLMNOPQ", timeout=4)
        terminal.wait_idle()
        terminal.save("simple-margin")
        enabled = terminal.text().splitlines()
        styled = cells(terminal.text(ansi=True))
        assert "----" in enabled[0] and " L:[" not in enabled[0] and "U+" not in enabled[0]
        assert re.search(r"\b0\s+1/4\s+0/40\s+0048\s+0%", enabled[0]), enabled[0]
        assert enabled[1:5] == baseline[1:5], "display options changed body glyphs or line wrapping"
        assert styled[1][gutter + 11] == baseline_colors[1][gutter + 11]
        assert styled[1][gutter + 12][:2] == MARGIN
        assert styled[1][gutter + 12][2] is False
        assert styled[2][gutter + 12][:2] == MARGIN, "margin missing beyond EOL"
        assert styled[7][gutter + 12][:2] == MARGIN, "margin missing below EOF"
        assert styled[1][gutter - 1] == baseline_colors[1][gutter - 1], "margin changed gutter"
        terminal.press("End")
        terminal.wait_idle()
        terminal.save("cursor-in-margin")
        styled = cells(terminal.text(ansi=True))
        assert styled[1][gutter + 27][:2] == MARKED, "margin replaced the EOL cursor style"
        terminal.press("Home", "F3", *(["Right"] * 16))
        terminal.wait_idle()
        terminal.save("selection-in-margin")
        selected = terminal.text().splitlines()
        styled = cells(terminal.text(ansi=True))
        assert "B---" in selected[0]
        assert re.search(r"\b16\s+1/4\s+16/40", selected[0]), selected[0]
        assert selected[1:5] == baseline[1:5], "selecting across margin changed displayed glyphs"
        assert all(styled[1][gutter + column][:2] == MARKED for column in range(12, 17))
        assert styled[1][gutter + 17][:2] == MARGIN
        terminal.press("F3", "End")
        terminal.resize(44, 14)
        terminal.wait_idle()
        terminal.save("narrow-simple")
        assert " L:[" not in terminal.text().splitlines()[0]
        assert "0123456789ABCDEFGHIJKLMNOPQ" in terminal.text().splitlines()[1]
        terminal.resize(100, 14)
        terminal.press("C-End")
        terminal.wait_idle()
        terminal.save("simple-eof")
        assert "<EOF>" in terminal.text().splitlines()[0]
        assert "100%" in terminal.text().splitlines()[0]
        terminal.press("F10")
        terminal.wait_file(terminal.exit_file, b"0\n")
    assert fixture.read_text() == TEXT, "rendering or selection changed file bytes"
    assert ini.read_bytes() == enabled_settings and resource.read_text() == SKIN
    result["phases"].append({
        "name": "enabled-after-restart", "exit": 0,
        "body_glyphs_unchanged": True, "syntax_before_margin_unchanged": True,
        "cursor_and_selection_preserved": True, "gutter_unchanged": True,
        "blank_area_margin": True, "file_bytes_unchanged": True,
    })
    result["status"] = "PASS"
    result_path.write_text(json.dumps(result, indent=2) + "\n")
    print(f"PASS matching editor display workflow: {result_path}")


if __name__ == "__main__":
    main()
