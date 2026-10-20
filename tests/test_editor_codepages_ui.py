from pathlib import Path
import os
import sys

from terminal_session import TerminalSession
from test_editor_mouse import profile
from test_editor_workflows import menu, search


def codepage(session, hotkey, text, **expected):
    session.raw(b"\x1be")
    session.wait_for("Choose codepage")
    session.type(hotkey)
    session.wait_for("Choose codepage", present=False)
    session.wait_editor(**expected)
    session.wait_for(text)


def main():
    binary, tmux, directory = sys.argv[1:]
    root = Path(directory).resolve()
    assert os.getuid() == 1001 and Path("/review/.operation-regressions-disposable").is_file()
    assert root.is_relative_to(Path("/review/cases/editor-codepages"))
    root.mkdir(parents=True, exist_ok=True)
    path = root / "draft"
    original = b"caf\xe9 caf\xe9\r\n"
    path.write_bytes(original)
    with TerminalSession(binary, tmux, profile(root / "terminal"), ["--edit", path]) as session:
        session.wait_editor(row=1, bytes=11, dirty=False)
        codepage(session, "1", "café café", dirty=False)
        session.type("X")
        session.wait_editor(bytes=12, dirty=True)
        session.press("Home", "F3", *["Right"] * 5, "F3")
        session.wait_editor(selection=5, offset=5)
        codepage(session, "7", "XcafÚ cafÚ", selection=5, offset=5, bytes=12, dirty=True)
        session.press("C-u")
        session.wait_editor(bytes=11, selection=0, dirty=True)
        codepage(session, "1", "café café", bytes=11, dirty=True)
        menu(session, 1, "Redo")
        session.wait_editor(bytes=12, dirty=True)
        session.wait_for("Xcafé café")
        session.raw(b"\x1be")
        session.wait_for("Choose codepage")
        session.type("0")
        session.wait_for("Choose codepage", present=False)
        session.wait_for("Editor error")
        session.wait_for("convert text from ASCII")
        assert path.read_bytes() == original
        session.press("Enter")
        session.wait_for("Editor error", present=False)
        session.wait_editor(bytes=12, dirty=True)
        session.press("Home")
        session.wait_editor(offset=0)
        search(session, "E9", mode=3)
        session.wait_editor(offset=4, column=5, bytes=12)
        session.raw(b"\x1b[31~")
        session.wait_editor(offset=9, column=10, bytes=12)
        session.save_editor()
        session.wait_file(path, b"X" + original)
        session.press("C-u")
        session.wait_editor(bytes=11, dirty=True)
        session.save_editor()
        session.wait_file(path, original)
        session.save("codepages-and-raw-search")
    print("PASS editor controls: dirty codepages, marks, undo/redo, conversion error, raw search and exact saves")


if __name__ == "__main__":
    main()
