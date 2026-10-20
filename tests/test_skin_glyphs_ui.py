from pathlib import Path
import json
import os
import sys

from terminal_session import TerminalSession
from test_audit_ui import click


SKIN = """[skin]
description=Independent glyph fixture
[core]
_default_=white;blue;none
selected=black;cyan;none
[Lines]
horiz=-
vert=!
lefttop=a
righttop=b
leftbottom=c
rightbottom=d
topmiddle=t
bottommiddle=u
leftmiddle=l
rightmiddle=r
cross=+
dhoriz==
dvert=:
dlefttop=A
drighttop=B
dleftbottom=C
drightbottom=D
dtopmiddle=T
dbottommiddle=U
dleftmiddle=L
drightmiddle=R
[widget-panel]
sort-up-char=U
sort-down-char=D
[widget-editor]
window-state-char=S
window-close-char=Q
"""


def choose(terminal, category, item):
    click(terminal, category, row=0)
    terminal.wait_for(item)
    click(terminal, item)


def editor_window(terminal):
    terminal.press("F9")
    terminal.wait_for("Window")
    choose(terminal, "Window", "Toggle fullscreen")
    terminal.wait_for("[S][Q]")


def acceptance(manager, tmux, root):
    left, right = root / "left-┌│─↑↓", root / "right"
    left.mkdir(parents=True)
    right.mkdir()
    filename = "f-┌│─↑↓.txt"
    content = "┌┐└┘│─╔╗╚╝═║↑↓ café\n"
    path = left / filename
    path.write_text(content)
    config = root / "ui/config/mcpp"
    (config / "skins").mkdir(parents=True)
    skin = config / "skins/glyph.ini"
    skin.write_text(SKIN)
    (config / "config").write_text(
        f"theme={skin}\nuse_subshell=false\nescape_timeout=50000\n"
        "editor_check_newline=false\neditor_confirm_save=false\n"
    )
    with TerminalSession(manager, tmux, root / "ui", [left, right],
                         record_exit=True, record_stderr=True) as terminal:
        terminal.wait_for(filename)
        terminal.wait_for("U Name")
        assert terminal.text().splitlines()[1].startswith("a"), terminal.text()
        assert "left-┌│─↑↓" in terminal.text(), terminal.text()
        choose(terminal, "Left", "Sort order...")
        terminal.wait_for("Reverse")
        assert any("A=" in line and "Sort order" in line and "=B" in line
                   for line in terminal.text().splitlines()), terminal.text()
        click(terminal, "Reverse")
        click(terminal, "[< OK >]")
        terminal.wait_for("D Name")
        choose(terminal, "Options", "Appearance...")
        terminal.wait_for("Skin:")
        terminal.press("Enter")
        terminal.wait_for("Skins")
        terminal.press("Home", "Enter")
        terminal.wait_for("Skin:")
        terminal.wait(lambda: terminal.text().splitlines()[1].startswith("┌"),
                      "builtin preview restores its own border glyphs")
        click(terminal, "[ Cancel ]")
        terminal.wait_for("Skin:", present=False)
        terminal.wait(lambda: terminal.text().splitlines()[1].startswith("a"),
                      "Appearance Cancel restores the original glyph palette")
        terminal.step("End", "F4")
        terminal.wait_for(content.strip())
        editor_window(terminal)
        terminal.wait_for(content.strip())
        terminal.save("glyph-controls")
        click(terminal, "[S]")
        terminal.wait_for("[S][Q]", present=False)
        terminal.wait_for(content.strip())
        editor_window(terminal)
        click(terminal, "[Q]")
        terminal.wait_for("D Name")
        terminal.press("F10")
        terminal.wait_file(terminal.exit_file, b"0\n")
    with TerminalSession(manager, tmux, root / "stick",
                         ["-u", "-S", skin, "--stickchars", left, right],
                         record_exit=True, record_stderr=True) as terminal:
        terminal.wait_for(filename)
        terminal.wait_for("^ Name")
        assert terminal.text().splitlines()[1].startswith("+"), terminal.text()
        assert "left-┌│─↑↓" in terminal.text(), terminal.text()
        terminal.save("stickchars")
        terminal.press("F10")
        terminal.wait_file(terminal.exit_file, b"0\n")
    assert path.read_text() == content
    (root / "result.json").write_text(json.dumps({
        "custom_lines_and_sort": True,
        "appearance_cancel": True,
        "editor_state_and_close": True,
        "unicode_text_unchanged": True,
        "stickchars_precedence": True,
    }, indent=2) + "\n")


if __name__ == "__main__":
    if os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or not Path(
        "/review/.operation-regressions-disposable"
    ).is_file():
        raise SystemExit("Glyph acceptance requires the existing UID1001 isolated container")
    manager, tmux = str(Path(sys.argv[1]).resolve()), sys.argv[2]
    root = Path(sys.argv[3]).resolve()
    if not str(root).startswith("/review/cases/native-skin-glyphs-"):
        raise SystemExit("Use an owned native-skin-glyphs case directory")
    acceptance(manager, tmux, root)
    print("native glyph App acceptance: passed")
