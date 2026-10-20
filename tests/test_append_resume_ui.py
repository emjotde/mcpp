from pathlib import Path
import os
import sys

from terminal_session import TerminalSession


def run(binary, tmux):
    if (os.geteuid() != 1001 or not Path("/.dockerenv").exists()
            or not Path("/review/.operation-regressions-disposable").exists()
            or Path.cwd() != Path("/review/cases/append-resume")):
        raise RuntimeError("Run only as UID1001 in the append-resume Docker fixture directory")
    root = Path("ui").resolve()
    left, right = root / "left", root / "right"
    left.mkdir(parents=True, exist_ok=True)
    right.mkdir(parents=True, exist_ok=True)
    (left / "a.txt").write_bytes(b"new-a\n")
    (right / "a.txt").write_bytes(b"old-a\n")
    (left / "b.txt").write_bytes(b"prefix-tail\n")
    (right / "b.txt").write_bytes(b"prefix")
    with TerminalSession(binary, tmux, root / "terminal", [left, right],
                         width=110, height=32) as session:
        session.wait_for("a.txt")
        session.press("Down", "C-a", "F5")
        session.wait_for("Source mask:")
        session.press("Enter")
        session.wait_for("Destination exists")
        session.wait_for("Source: " + str(left / "a.txt"))
        for label in ("Yes", "No", "Append", "All", "Older", "None", "Abort"):
            session.wait_for("[ " + label + " ]")
        assert "[ Reget ]" not in session.text(), "Reget offered for an equal-sized target"
        session.save("append-collision")
        session.type("p")
        session.wait_for("Source: " + str(left / "b.txt"))
        session.wait_for("[ Reget ]")
        session.save("reget-collision")
        session.type("r")
        session.wait_for("Copy complete")
        session.wait_file(right / "a.txt", b"old-a\nnew-a\n")
        session.wait_file(right / "b.txt", b"prefix-tail\n")
        assert (left / "a.txt").read_bytes() == b"new-a\n"
        assert (left / "b.txt").read_bytes() == b"prefix-tail\n"
        session.save("completed")
    print("PASS append-reget collision UI")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()), sys.argv[2])
