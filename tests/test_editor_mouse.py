from pathlib import Path
import shutil
import sys
import tempfile

from terminal_session import TerminalSession


def mouse(session, column, row, button=0, motion="M"):
    session.raw(f"\x1b[<{button};{column};{row}{motion}".encode())


def click(session, label, header=False):
    lines = session.text().splitlines()
    candidates = enumerate(lines[:1] if header else lines[1:], 0 if header else 1)
    row, line = next((row, line) for row, line in candidates if label in line)
    column = line.index(label) + 2
    mouse(session, column, row + 1)
    mouse(session, column, row + 1, motion="m")


def profile(root):
    config = root / "config" / "mcpp"
    config.mkdir(parents=True)
    (config / "config").write_text("editor_line_numbers=false\n")
    return root


def main():
    binary, tmux = str(Path(sys.argv[1]).resolve()), sys.argv[2]
    root = Path(tempfile.mkdtemp(prefix="mcpp-editor-mouse-"))
    passed = False
    try:
        path = root / "mouse.txt"
        expected = b"alpha  gamma\nsecond line\n"
        path.write_bytes(b"alpha beta gamma\nsecond line\n")
        with TerminalSession(binary, tmux, profile(root / "linear"), ["--edit", path]) as session:
            session.wait_editor(row=1, column=1)
            mouse(session, 7, 2)
            mouse(session, 11, 2, 32)
            mouse(session, 11, 2, motion="m")
            session.wait_editor(selection=4, column=11)
            session.press("F8")
            session.save_editor()
            session.wait_file(path, expected)
            session.press("Home")
            session.type("X")
            session.wait_editor(dirty=True)
            mouse(session, 2, 1)
            session.wait_for("Open file")
            click(session, "Save")
            session.confirm_editor_save()
            session.wait_editor(dirty=False)
            session.wait_file(path, b"X" + expected)
            mouse(session, 2, 1)
            session.wait_for("Open file")
            click(session, "Edit", header=True)
            session.wait_for("Undo")
            click(session, "Undo")
            session.wait_editor(dirty=True)
            session.save_editor()
            session.wait_file(path, expected)
            session.press("F7")
            session.wait_for("Search for:")
            session.type("gmma")
            session.press("Home", "Right")
            session.type("a")
            session.wait_for("> gamma")
            click(session, "OK")
            session.wait_editor(selection=5, column=8)
            session.save("linear-drag-and-menus")
        path.write_bytes(b"abcdef\nABCDEF\nabcdef\n")
        with TerminalSession(binary, tmux, profile(root / "columns"), ["--edit", path]) as session:
            session.wait_editor(row=1, column=1)
            mouse(session, 2, 2, 8)
            mouse(session, 4, 4, 40)
            mouse(session, 4, 4, 8, "m")
            session.wait_editor(selection=8, mark="C")
            session.press("F8")
            session.save_editor()
            session.wait_file(path, b"adef\nADEF\nadef\n")
            session.save("column-drag")
        path.write_bytes(b"a\tb\n")
        with TerminalSession(binary, tmux, profile(root / "options"), ["--edit", path]) as session:
            session.wait_editor(row=1, column=1)
            mouse(session, 2, 1)
            session.wait_for("Open file")
            click(session, "Options", header=True)
            session.wait_for("General...")
            click(session, "General...")
            session.wait_for("Tab spacing:")
            session.type("12")
            session.press("Left", "BSpace")
            session.type("3")
            session.press("Tab")
            session.type("2")
            click(session, "OK")
            session.wait_for("Tab spacing:", present=False)
            session.press("End")
            session.wait_editor(column=34)
            session.save("editable-options")
        passed = True
    finally:
        if passed:
            shutil.rmtree(root)
        else:
            print(f"Editor mouse failure artifacts: {root}", file=sys.stderr)


if __name__ == "__main__":
    main()
