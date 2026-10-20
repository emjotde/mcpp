from pathlib import Path
import os
import shutil
import sys

from terminal_session import TerminalSession


def run(binary, tmux):
    if (os.geteuid() != 1001 or not Path("/.dockerenv").exists()
            or not Path("/review/.operation-regressions-disposable").exists()
            or Path.cwd() != Path("/review/cases/copy-masks")):
        raise RuntimeError("Run only as UID1001 in the copy-masks Docker fixture directory")
    root = Path("ui-integration").resolve()
    if root.exists():
        shutil.rmtree(root)
    left, right = root / "left", root / "right"
    (left / "tree").mkdir(parents=True)
    (right / "Ree-copy").mkdir(parents=True)
    (left / "tree/leaf").write_bytes(b"leaf\n")
    (left / "external").write_bytes(b"target\n")
    (left / "skip.txt").write_bytes(b"skip\n")
    (left / "tree/link").symlink_to("../external")
    (right / "Ree-copy/old").write_bytes(b"old\n")
    with TerminalSession(binary, tmux, root / "terminal", [left, right],
                         width=110, height=32, record_exit=True) as session:
        session.wait_for("skip.txt")
        session.press("Down", "C-a", "F5")
        session.wait_for("Source mask:")
        for label in ("Using shell patterns", "Follow links", "Preserve attributes",
                      "Dive into subdir if exists", "Stable symlinks", "[ Background ]"):
            session.wait_for(label)
        session.press("C-u")
        session.type(str(right / r"\u*-copy"))
        session.press("BTab", "BTab", "C-u")
        session.type("t*")
        session.press("Tab", "Tab", "Tab", "Tab", "Tab", "Space", "Tab", "Space")
        session.wait_for("[x] Dive into subdir if exists")
        session.wait_for("[x] Stable symlinks")
        session.save("options")
        session.press("Enter")
        session.wait_for("Source mask:", present=False)
        session.wait_for("Copy complete")
        session.wait_file(right / "Ree-copy/tree/leaf", b"leaf\n")
        session.wait_file(right / "Ree-copy/tree/link", b"target\n")
        assert os.readlink(right / "Ree-copy/tree/link") == str(left / "tree/../external")
        assert (right / "Ree-copy/old").read_bytes() == b"old\n"
        assert not (right / "skip.txt").exists()
        assert not (right / "external").exists()
        assert (left / "tree/leaf").read_bytes() == b"leaf\n"
        session.save("completed")
    print("PASS copy-masks integrated dialog")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()), sys.argv[2])
