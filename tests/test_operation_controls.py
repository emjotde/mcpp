from pathlib import Path
import os
import shutil
import sys
import tempfile

from terminal_session import TerminalSession


def run(binary, tmux):
    if not Path("/.dockerenv").exists() or not Path(
        "/review/.operation-regressions-disposable"
    ).exists() or os.geteuid() == 0:
        raise RuntimeError("Operation controls require the non-root disposable Docker image")
    root = Path(tempfile.mkdtemp(prefix="mcpp-operation-controls-"))
    passed = False
    try:
        for case in (
            "collision", "all", "none", "older", "retry", "ignore",
            "move-retry", "delete-retry", "older-directory",
        ):
            left, right = root / case / "left", root / case / "right"
            left.mkdir(parents=True)
            right.mkdir()
            (left / "a.txt").write_bytes(b"NEW A\n")
            if case == "older-directory":
                (left / "tree").mkdir()
                (right / "tree").mkdir()
                (left / "tree/a.txt").write_bytes(b"OLDER SOURCE\n")
                (right / "tree/a.txt").write_bytes(b"NEWER TARGET\n")
                os.utime(left / "tree/a.txt", (1000, 1000))
                os.utime(right / "tree/a.txt", (2000, 2000))
            multiple = case in ("collision", "all", "none", "older", "ignore")
            if multiple:
                (left / "b.txt").write_bytes(b"NEW B\n")
            if case in ("collision", "all", "none", "older"):
                (right / "a.txt").write_bytes(b"OLD A\n")
                (right / "b.txt").write_bytes(b"OLD B\n")
            if case == "older":
                for path, modified in (
                    (left / "a.txt", 2000), (right / "a.txt", 1000),
                    (left / "b.txt", 1000), (right / "b.txt", 2000),
                ):
                    os.utime(path, (modified, modified))
            if case == "retry":
                (right / "a.txt").write_bytes(b"OLD A\n")
                right.chmod(0o500)
            if case == "ignore":
                (left / "a.txt").chmod(0)
                (left / "b.txt").chmod(0)
            if case in ("move-retry", "delete-retry"):
                left.chmod(0o500)
            try:
                with TerminalSession(
                    binary, tmux, root / case / "ui", [left, right]
                ) as session:
                    session.wait_for("a.txt")
                    session.press("Down")
                    if multiple:
                        session.press("C-a")
                    key = {"move-retry": "F6", "delete-retry": "F8",
                           "older-directory": "F6"}.get(case, "F5")
                    session.press(key)
                    session.wait_for(
                        "Delete 1 selected item(s)?" if case == "delete-retry"
                        else "Source mask:"
                    )
                    session.press("Enter")
                    if case == "older-directory":
                        session.wait_for("Destination exists")
                        session.type("o")
                        session.wait_for("Move incomplete: 1 skipped")
                        session.wait_file(left / "tree/a.txt", b"OLDER SOURCE\n")
                        session.wait_file(right / "tree/a.txt", b"NEWER TARGET\n")
                    elif case == "collision":
                        session.wait_for("Destination exists")
                        session.wait_for("Source: " + str(left / "a.txt"))
                        session.type("n")
                        session.wait_for("Source: " + str(left / "b.txt"))
                        session.type("y")
                        session.wait_for("Copy incomplete: 1 skipped")
                        session.wait_file(right / "a.txt", b"OLD A\n")
                        session.wait_file(right / "b.txt", b"NEW B\n")
                        session.wait_for("1 selected,")
                        session.press("C-x")
                        session.type("j")
                        session.wait_for("Background jobs")
                        session.wait_for("Copy incomplete:")
                        session.save("job-history")
                        session.press("Escape")
                        session.wait_for("Background jobs", present=False)
                    elif case in ("all", "none", "older"):
                        session.wait_for("Destination exists")
                        session.type({"all": "a", "none": "s", "older": "o"}[case])
                        skipped = {"all": 0, "none": 2, "older": 1}[case]
                        session.wait_for(
                            f"Copy incomplete: {skipped} skipped" if skipped
                            else "Copy complete"
                        )
                        session.wait_file(
                            right / "a.txt", b"OLD A\n" if case == "none" else b"NEW A\n"
                        )
                        session.wait_file(
                            right / "b.txt", b"NEW B\n" if case == "all" else b"OLD B\n"
                        )
                    elif case == "retry":
                        session.wait_for("Destination exists")
                        session.type("a")
                        session.wait_for("File operation error")
                        session.wait_file(right / "a.txt", b"OLD A\n")
                        right.chmod(0o700)
                        session.type("r")
                        session.wait_file(right / "a.txt", b"NEW A\n")
                        session.wait_for("Copy complete")
                    elif case in ("move-retry", "delete-retry"):
                        session.wait_for("File operation error")
                        session.wait_file(left / "a.txt", b"NEW A\n")
                        left.chmod(0o700)
                        session.type("r")
                        if case == "move-retry":
                            session.wait_file(right / "a.txt", b"NEW A\n")
                        session.wait_for(
                            "Move complete" if case == "move-retry" else "Delete complete"
                        )
                        assert not (left / "a.txt").exists()
                    else:
                        session.wait_for("File operation error")
                        session.type("i")
                        session.wait_for("Copy incomplete: 2 skipped")
                        assert not (right / "a.txt").exists()
                        assert not (right / "b.txt").exists()
                        session.wait_for("2 selected,")
                    session.save("completed")
            finally:
                left.chmod(0o700)
                right.chmod(0o700)
                for path in left.iterdir():
                    path.chmod(0o700 if path.is_dir() else 0o600)
        passed = True
    finally:
        if passed:
            shutil.rmtree(root)
        else:
            print(f"Operation-control failure artifacts: {root}", file=sys.stderr)


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()), sys.argv[2])
