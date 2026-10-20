from pathlib import Path
import os
import re
import shlex
import sys
import tempfile

from terminal_session import TerminalSession


def run(binary, tmux):
    if (os.getuid() != 1001 or os.geteuid() != 1001
            or not Path("/.dockerenv").is_file()
            or not Path("/review/.operation-regressions-disposable").is_file()
            or Path.cwd() != Path("/review/cases/concurrent-jobs")):
        raise RuntimeError("Run only as UID1001 in the concurrent-jobs Docker fixture directory")
    root = Path(tempfile.mkdtemp(prefix="ui-", dir=Path.cwd()))
    left, right = root / "left", root / "right"
    left.mkdir(parents=True)
    right.mkdir()
    for name in ("a", "b", "c"):
        (left / f"{name}.txt").write_bytes(f"new-{name}\n".encode())
    for name in ("a", "b"):
        (right / f"{name}.txt").write_bytes(f"old-{name}\n".encode())
    config = root / "terminal/config/mcpp"
    config.mkdir(parents=True)
    (config / "config").write_text(
        "use_subshell=1\npause_after_run=0\nconfirm_exit=0\n"
        "confirm_overwrite=1\nverbose_operation=1\nsafe_overwrite=0\n"
    )
    with TerminalSession(binary, tmux, root / "terminal", [left, right],
                         width=110, height=32, record_exit=True) as session:
        def background_copy():
            session.press("F5")
            session.wait_for("Source mask:")
            session.press("M-b")
            session.wait_for("Source mask:", present=False)

        def jobs():
            session.press("C-x")
            session.type("j")
            session.wait_for("Background jobs")

        def state(job, text):
            session.wait(
                lambda: re.search(rf"\b{job}  Copy  {re.escape(text)}", session.text()),
                f"Job {job} should be {text}",
            )

        session.wait_for("a.txt")
        session.press("Down")
        background_copy()
        session.wait_for("waiting for a decision")
        session.press("Down")
        background_copy()
        jobs()
        session.wait(
            lambda: session.text().count("Waiting for decision") == 2,
            "Two independent workers should both be waiting for their own collision",
        )
        session.save("two-waiting")
        session.press("Up", "F2")
        state(1, "Paused")
        state(2, "Waiting for decision")
        session.press("Escape")
        session.wait_for("Background jobs", present=False)

        session.press("Down")
        background_copy()
        session.wait_for("Copy complete")
        session.wait_file(right / "c.txt", b"new-c\n")
        command = "printf '%s' shell-ok > " + shlex.quote(str(root / "shell.ok"))
        session.type(command)
        session.press("Enter")
        session.wait_file(root / "shell.ok", b"shell-ok")
        session.wait_for("Command exited 0")
        session.press("F4")
        session.wait_editor(dirty=False)
        assert "Destination exists" not in session.text(), "Background question replaced the editor"
        session.save("editor-with-waiting-jobs")
        session.press("F10")
        session.wait(lambda: "L:[" not in session.text(), "The editor should return to panels")

        jobs()
        session.press("Up", "Up", "F8")
        state(1, "Operation cancelled")
        state(2, "Waiting for decision")
        session.wait_file(right / "a.txt", b"old-a\n")
        session.press("Down", "F3")
        session.wait_for("Destination exists")
        session.wait_for("Source: " + str(left / "b.txt"))
        session.save("second-job-question")
        session.type("y")
        session.wait_for("Destination exists", present=False)
        session.wait_file(right / "b.txt", b"new-b\n")
        session.wait_for("Background jobs")
        state(2, "Copy complete")
        session.wait_file(right / "a.txt", b"old-a\n")
        session.save("independent-completions")
        session.press("Escape")
        session.wait_for("Background jobs", present=False)

        background_copy()
        jobs()
        state(4, "Waiting for decision")
        session.press("F2")
        state(4, "Paused")
        session.press("Escape")
        session.wait_for("Background jobs", present=False)
        session.press("F10")
        session.wait(
            lambda: session.exit_file.is_file() and session.exit_file.read_text().strip() == "0",
            "Quit should cancel and join a paused job waiting for an answer",
        )
        assert (left / "a.txt").read_bytes() == b"new-a\n"
        assert (left / "b.txt").read_bytes() == b"new-b\n"
        assert (left / "c.txt").read_bytes() == b"new-c\n"
        assert (right / "a.txt").read_bytes() == b"old-a\n"
        assert (right / "b.txt").read_bytes() == b"new-b\n"
        assert (right / "c.txt").read_bytes() == b"new-c\n"
    print("PASS concurrent jobs UI: workers, controls, prompts, shell, editor, quit")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()), sys.argv[2])
