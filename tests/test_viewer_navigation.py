from pathlib import Path
import shutil
import sys
import tempfile

from terminal_session import TerminalSession


def listing_target(session):
    info = session.text().splitlines()[-5][:session.width // 2]
    return info.lstrip("│ ").split()[0]


def main():
    binary = Path(sys.argv[1]).resolve()
    tmux = sys.argv[2]
    root = Path(tempfile.mkdtemp(prefix="mcpp-view-nav-"))
    succeeded = False
    try:
        fixture = root / "files"
        fixture.mkdir()
        (fixture / "a-before.txt").write_text("BEFORE_OTHER\n")
        (fixture / "b-target.txt").write_text(
            "BEGIN_TARGET\n"
            + "".join(f"target row {row:04d}\n" for row in range(100))
            + "END_TARGET\n"
        )
        (fixture / "z-after.txt").write_text("AFTER_OTHER\n")
        with TerminalSession(binary, tmux, root / "terminal",
                             [fixture, fixture]) as session:
            session.wait_for("b-target.txt")
            session.press("Home", "Down", "Down", "C-x", "q")
            session.wait_for("Quick view: b-target.txt")
            session.press("Tab")
            session.wait_for("Quick view focused")
            session.wait_for("BEGIN_TARGET")
            selected = listing_target(session)
            if selected != "b-target.txt":
                raise AssertionError(f"Unexpected initial listing target: {selected!r}")
            session.press("End")
            session.wait_for("END_TARGET")
            if "Quick view: b-target.txt" not in session.text():
                raise AssertionError("Quick-view End changed the listing target")
            if listing_target(session) != selected:
                raise AssertionError("Quick-view End moved the listing cursor")
            session.save("quick-end")
            session.press("Home")
            session.wait_for("BEGIN_TARGET")
            if "Quick view: b-target.txt" not in session.text():
                raise AssertionError("Quick-view Home changed the listing target")
            if listing_target(session) != selected:
                raise AssertionError("Quick-view Home moved the listing cursor")
            session.save("quick-home")

            session.press("Tab")
            session.wait_for("File listing focused")
            session.press("F3")
            session.wait_for("BEGIN_TARGET")
            session.press("End")
            session.wait_for("END_TARGET")
            session.save("viewer-end")
            session.press("Home")
            session.wait_for("BEGIN_TARGET")
            session.save("viewer-home")
            session.press("F3")
            session.wait_for("Quick view: b-target.txt")
            session.press("F10")
            session.wait(
                lambda: session.run("display-message", "-p", "-t", "test:0.0",
                                    "#{pane_dead}").strip() == "1",
                "File manager exits cleanly",
            )
        print("Viewer and focused quick-view Home/End passed")
        succeeded = True
    finally:
        if succeeded:
            shutil.rmtree(root)
        else:
            print(f"Viewer navigation artifacts retained at {root}", file=sys.stderr)


if __name__ == "__main__":
    main()
