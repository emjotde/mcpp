from pathlib import Path
import os
import sys

from terminal_session import TerminalSession


def run(binary, tmux, root):
    if (os.getuid() != 1001 or os.geteuid() != 1001 or
            not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file() or
            not root.is_relative_to("/review/cases/recursive-delete")):
        raise RuntimeError("Run only as UID1001 in the approved recursive-delete Docker directory")
    if root.exists():
        raise RuntimeError("Choose a fresh recursive-delete UI fixture directory")
    for choice, count in (("all", 2), ("none", 4)):
        case = root / choice
        left, right = case / "left", case / "right"
        for name in ("alpha", "beta"):
            (left / name).mkdir(parents=True)
            (left / name / "file").write_text("keep\n")
        right.mkdir()
        if choice == "none":
            (left / "empty").mkdir()
            (left / "plain").write_text("plain\n")
            (left / "alpha").chmod(0)
        config = case / "ui/config/mcpp"
        config.mkdir(parents=True)
        (config / "config").write_text(
            "safe_delete=1\nconfirm_delete=1\nverbose_operation=1\n"
            "compute_totals=1\nuse_subshell=0\n"
        )
        try:
            with TerminalSession(binary, tmux, case / "ui", [left, right],
                                 width=100, height=30, record_exit=True) as session:
                session.wait_for("alpha")
                session.press("Down", "C-a", "F8")
                session.wait_for(f"Delete {count} selected item(s)?")
                assert any("No" in text for text in session.focused_text()), "Safe delete lost initial No default"
                session.press("y")
                session.wait_for("Delete it recursively?")
                for label in ("Yes", "No", "All", "None", "Abort"):
                    session.wait_for(f"[ {label} ]")
                assert any("No" in text for text in session.focused_text()), "Safe delete lost recursive No default"
                session.save("recursive-prompt")
                session.press("l" if choice == "all" else "e")
                session.wait_for("Delete it recursively?", present=False)
                session.wait_for("Delete complete" if choice == "all"
                                 else "Delete incomplete: 2 skipped")
                if choice == "all":
                    assert not (left / "alpha").exists() and not (left / "beta").exists()
                else:
                    (left / "alpha").chmod(0o700)
                    assert (left / "alpha/file").read_text() == "keep\n"
                    assert (left / "beta/file").read_text() == "keep\n"
                    assert not (left / "empty").exists() and not (left / "plain").exists()
                    session.wait_for("2 selected,")
                session.save("completed")
        finally:
            if (left / "alpha").is_dir():
                (left / "alpha").chmod(0o700)
        print(f"PASS recursive delete UI: {choice}")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()), sys.argv[2], Path(sys.argv[3]).resolve())
