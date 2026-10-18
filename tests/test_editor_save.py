from pathlib import Path
import argparse
import os
import shutil
import tempfile

from terminal_session import TerminalSession
from test_editor_mouse import click, profile
from test_editor_workflows import menu, prompt


def request_save(session):
    session.press("F2")
    session.wait_for("Save file")
    session.wait(
        lambda: "[ Save ]" in session.focused_text(),
        "Save is the default confirmation button",
    )


def normal_saves(binary, tmux, root):
    path = root / "original.txt"
    original = b"alpha\nbeta\n"
    path.write_bytes(original)
    with TerminalSession(binary, tmux, profile(root / "normal"), ["--edit", path]) as session:
        session.wait_editor(row=1)
        session.type("X")
        session.press("F3", "Right", "Right", "Right", "F3")
        before = session.wait_editor(row=1, column=5, selection=3, dirty=True)
        for cancel in (
            ("Escape",), ("F10",), ("Tab", "Space"), ("BTab", "Enter"),
            ("Left", "Enter"), ("Right", "Enter"), ("c",),
        ):
            request_save(session)
            session.wait_file(path, original)
            session.press(*cancel)
            session.wait_for("Save file", present=False)
            session.wait_editor(**before)
            session.wait_file(path, original)
        menu(session, 0, "Save  F2")
        session.wait_for("Save file")
        click(session, "[ Cancel ]")
        session.wait_for("Save file", present=False)
        session.wait_editor(**before)
        request_save(session)
        session.type("s")
        session.wait_editor(dirty=False, column=5, selection=3)
        session.wait_file(path, b"Xalpha\nbeta\n")
        request_save(session)
        session.confirm_editor_save()
        session.wait_file(path, b"Xalpha\nbeta\n")
        menu(session, 0, "Save  F2")
        session.wait_for("Save file")
        click(session, "[ Save ]")
        session.wait_for("Save file", present=False)
        session.wait_editor(dirty=False)
        session.save("save-and-cancel")
        new = root / "new.txt"
        session.press("C-n")
        prompt(session, new)
        session.wait_editor(row=1, column=1, dirty=False)
        session.type("NEW")
        request_save(session)
        assert not new.exists(), "Opening Save confirmation must not create the file"
        session.confirm_editor_save()
        session.wait_file(new, b"NEW")


def failed_saves(binary, tmux, root):
    if not Path("/.dockerenv").exists() or not Path(
        "/review/.operation-regressions-disposable"
    ).exists() or os.geteuid() == 0:
        raise RuntimeError("Save failures require the non-root disposable Docker image")
    directory = root / "read-only"
    directory.mkdir()
    path = directory / "original.txt"
    original = b"original\n"
    path.write_bytes(original)
    try:
        with TerminalSession(binary, tmux, profile(root / "failure"), ["--edit", path]) as session:
            session.wait_editor(row=1)
            session.type("X")
            session.wait_editor(dirty=True)
            directory.chmod(0o500)
            request_save(session)
            session.press("Enter")
            session.wait_for("Editor error")
            session.wait_file(path, original)
            assert not list(directory.glob(".mcpp-*")), "Failed save leaves no staging files"
            session.press("Enter")
            session.wait_editor(dirty=True, column=2)
            directory.chmod(0o700)
            session.save_editor()
            session.wait_file(path, b"Xoriginal\n")
            session.type("Y")
            request_save(session)
            path.write_bytes(b"external change\n")
            session.press("Enter")
            session.wait_for("Editor error")
            session.wait_file(path, b"external change\n")
            session.press("Enter")
            session.wait_editor(dirty=True)
            session.save("failed-save-preserves-draft")
    finally:
        directory.chmod(0o700)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("tmux")
    parser.add_argument("--failures", action="store_true")
    arguments = parser.parse_args()
    root = Path(tempfile.mkdtemp(prefix="mcpp-editor-save-"))
    passed = False
    try:
        run = failed_saves if arguments.failures else normal_saves
        run(str(Path(arguments.binary).resolve()), arguments.tmux, root)
        passed = True
    finally:
        if passed:
            shutil.rmtree(root)
        else:
            print(f"Editor save failure artifacts: {root}", flush=True)


if __name__ == "__main__":
    main()
