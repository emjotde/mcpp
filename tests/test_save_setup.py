from pathlib import Path
import shutil
import sys
import tempfile

from terminal_session import TerminalSession


def panel_text(session):
    return "\n".join(line[:session.width // 2]
                     for line in session.text().splitlines()[1:-3])


def quit_panels(session):
    session.press("F10")
    session.wait(
        lambda: session.run("display-message", "-p", "-t", "test:0.0",
                            "#{pane_dead}").strip() == "1",
        "File manager exits cleanly",
    )


def saved_hidden(config):
    fields = dict(line.split("=", 1) for line in config.read_text().splitlines()
                  if "=" in line)
    return fields["show_hidden"]


def check_restart(binary, tmux, root, fixture, saved, hidden, expected):
    config = root / "config" / "mcpp" / "config"
    config.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(saved, config)
    with TerminalSession(binary, tmux, root, [fixture, fixture]) as session:
        session.wait_for("visible.txt")
        session.wait(
            lambda: (hidden in panel_text(session)) == expected,
            f"Restart loads show_hidden={expected}",
        )
        session.save("restart")
        quit_panels(session)


def main():
    binary = Path(sys.argv[1]).resolve()
    tmux = sys.argv[2]
    root = Path(tempfile.mkdtemp(prefix="mcpp-save-setup-"))
    succeeded = False
    try:
        fixture = root / "files"
        fixture.mkdir()
        hidden = ".save-setup-hidden"
        (fixture / hidden).write_text("hidden\n")
        (fixture / "visible.txt").write_text("visible\n")
        config = root / "live" / "config" / "mcpp" / "config"
        config.parent.mkdir(parents=True)
        config.write_text("show_hidden=true\n")
        with TerminalSession(binary, tmux, root / "live",
                             [fixture, fixture]) as session:
            session.wait_for("visible.txt")
            session.wait(lambda: hidden in panel_text(session),
                         "Initial hidden file is shown")
            session.raw(b"\x1b.")
            session.wait(lambda: hidden not in panel_text(session),
                         "Alt-. hides the left-panel file")
            session.press("F9", "Right", "Right", "Right", "Down", "Up", "Enter")
            session.wait_for("Setup saved")
            session.save("setup-saved")
            if saved_hidden(config) != "false":
                raise AssertionError("Save setup wrote stale show_hidden")
            check_restart(binary, tmux, root / "saved-restart", fixture,
                          config, hidden, False)

            session.raw(b"\x1b.")
            session.wait(lambda: hidden in panel_text(session),
                         "Hidden file is shown before editor Save setup")
            session.press("Home", "End", "F4")
            session.press("F9", "Right", "Right", "Right", "Right",
                          "Right", "Right", "Down", "Up", "Enter")
            session.wait(lambda: saved_hidden(config) == "true",
                         "Editor Options Save setup writes current panel state")
            session.save("editor-setup-saved")
            session.press("F10")
            session.wait(lambda: hidden in panel_text(session),
                         "Editor closes back to the original panels")
            check_restart(binary, tmux, root / "editor-saved-restart", fixture,
                          config, hidden, True)

            session.raw(b"\x1b.")
            session.wait(lambda: hidden not in panel_text(session),
                         "Hidden file is hidden again before exit")
            quit_panels(session)
            if saved_hidden(config) != "false":
                raise AssertionError("Normal exit no longer persists panel state")
            check_restart(binary, tmux, root / "exit-restart", fixture,
                          config, hidden, False)
        print("Both Save setup routes and normal-exit panel persistence passed")
        succeeded = True
    finally:
        if succeeded:
            shutil.rmtree(root)
        else:
            print(f"Save setup artifacts retained at {root}", file=sys.stderr)


if __name__ == "__main__":
    main()
