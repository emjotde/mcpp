from pathlib import Path
import shutil
import sys
import tempfile

from terminal_session import TerminalSession
from test_editor_mouse import click, mouse, profile
from test_editor_workflows import menu, open_file


def frame(session, row, top, cursor_y, column=None):
    expected = {"row": row}
    if column is not None:
        expected["column"] = column
    session.wait_editor(**expected)
    session.wait(
        lambda: session.text().splitlines()[1].startswith(f"LINE_{top:03d}"),
        f"Top visible line is {top}",
    )
    session.wait(
        lambda: session.run("display-message", "-p", "-t", "test:0.0",
                            "#{cursor_y}").strip() == str(cursor_y),
        f"Cursor screen row is {cursor_y}",
    )


def navigation(binary, tmux, root):
    path = root / "navigation.txt"
    contents = "".join(f"LINE_{i:03d} abcdefghijklmnopqrstuvwxyz\n" for i in range(1, 61))
    path.write_text(contents)
    with TerminalSession(binary, tmux, profile(root / "navigation"), ["--edit", path]) as session:
        frame(session, 1, 1, 1)
        for keys, row, top, y in (
            (["PageDown"], 28, 28, 1),
            (["PageDown"], 55, 34, 22),
            (["PageDown"], 61, 34, 28),
            (["Down"] * 4, 61, 34, 28),
            (["Up"] * 3, 58, 34, 25),
            (["PageUp"], 31, 7, 25),
            (["C-Home"], 1, 1, 1),
            (["Down"] * 29, 30, 3, 28),
            (["C-End"], 61, 34, 28),
            (["PageUp"], 34, 7, 28),
            (["C-PgUp"], 7, 7, 1),
            (["C-PgDn"], 34, 7, 28),
            (["C-Down"], 35, 8, 28),
            (["C-Up"], 34, 7, 28),
            (["C-Home", "Right", "Right", "C-End"], 61, 34, 28),
        ):
            session.press(*keys)
            frame(session, row, top, y)
        session.press("C-Home")
        frame(session, 1, 1, 1, column=3)
        session.press("End")
        session.wait_for("0010 U+000A")
        session.press("Home", "Right", "Right")
        session.press(*["Down"] * 15)
        frame(session, 16, 1, 16)
        for button, row, top, y in (
            (65, 18, 3, 16), (64, 16, 1, 16), (64, 14, 1, 14),
        ):
            mouse(session, 20, 10, button)
            frame(session, row, top, y)
        session.press("C-End", *["Up"] * 15)
        frame(session, 46, 34, 13)
        mouse(session, 20, 10, 65)
        frame(session, 48, 34, 15)
        session.save("navigation")
        session.press("C-End")
        frame(session, 61, 34, 28)
        session.wait_for("<EOF>")
        session.resize(80, 20)
        frame(session, 61, 44, 18)
        session.resize(100, 30)
        frame(session, 61, 34, 28)
        session.press("F7")
        session.wait_for("Search for:")
        session.type("LINE_04")
        session.press("Enter")
        frame(session, 40, 26, 15, column=1)
        session.raw(b"\x1b[31~")
        frame(session, 41, 27, 15, column=1)
        session.press("C-End")
        frame(session, 61, 34, 28)
        other = root / "other.txt"
        other.write_text("other buffer\n")
        open_file(session, other)
        session.press("Down")
        session.wait_editor(row=2)
        menu(session, 5, "Next")
        frame(session, 61, 34, 28)
        menu(session, 5, "Next")
        session.wait_editor(row=2)
    path.write_text(contents.rstrip("\n"))
    with TerminalSession(binary, tmux, profile(root / "no-newline"), ["--edit", path]) as session:
        session.wait_editor(row=1)
        session.press("Right", "Right", "C-End")
        frame(session, 60, 33, 28, column=3)
        session.press("C-Home", "PageDown", "PageDown", "PageDown")
        frame(session, 60, 33, 28, column=3)
    long_line = "".join(f"{i:03d}|" for i in range(70))
    path.write_text(long_line + "\nshort\n" + long_line + "\n")
    with TerminalSession(binary, tmux, profile(root / "horizontal"), ["--edit", path]) as session:
        session.wait_editor(row=1)
        session.press("End")
        session.wait_editor(column=281)
        first = session.text().splitlines()[1]
        session.press(*["Left"] * 5)
        session.wait_editor(column=276)
        assert session.text().splitlines()[1] == first, "Left within the viewport must not scroll"
        session.press("Down")
        session.wait_editor(row=2, column=6)
        session.wait(lambda: session.text().splitlines()[1].startswith("01|002|"),
                     "Short-line cursor is revealed by horizontal scrolling")
        session.save("horizontal")


def deletion(binary, tmux, root):
    path = root / "delete.txt"
    original = b"first line\nsecond line\nthird line\n"
    path.write_bytes(original)
    with TerminalSession(binary, tmux, profile(root / "delete"), ["--edit", path]) as session:
        session.wait_editor(row=1)
        session.press("Down", "Right", "Right", "F8")
        session.wait_editor(row=2, column=1, lines=3, dirty=True)
        session.save_editor()
        session.wait_file(path, b"first line\nthird line\n")
        session.press("C-u")
        session.save_editor()
        session.wait_file(path, original)
        session.press("C-Home", "F3", "F8")
        session.wait_editor(row=1, dirty=False)
        session.press("Right", "Right", "F8")
        session.save_editor()
        session.wait_file(path, b"second line\nthird line\n")
        session.press("C-u", "Down", "Home", "F3", "Right", "Right", "Right", "F3", "F8")
        session.save_editor()
        session.wait_file(path, b"first line\nond line\nthird line\n")
        session.press("C-u")
        session.save_editor()
        session.wait_file(path, original)
        session.press("F9")
        session.wait_for("Options")
        click(session, "Edit", header=True)
        session.wait_for("Delete  F8")
        click(session, "Delete  F8")
        session.wait_editor(lines=4, column=1)
        session.save_editor()
        session.wait_file(path, b"first line\nond line\nthird line\n")
        session.save("f8-and-menu")


def main():
    binary, tmux = str(Path(sys.argv[1]).resolve()), sys.argv[2]
    root = Path(tempfile.mkdtemp(prefix="mcpp-editor-parity-"))
    passed = False
    try:
        navigation(binary, tmux, root)
        deletion(binary, tmux, root)
        passed = True
    finally:
        if passed:
            shutil.rmtree(root)
        else:
            print(f"Editor parity failure artifacts: {root}", file=sys.stderr)


if __name__ == "__main__":
    main()
