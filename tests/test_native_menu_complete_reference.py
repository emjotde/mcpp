from pathlib import Path
import json
import os
import sys

from terminal_session import TerminalSession


def main():
    if (os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file()):
        raise SystemExit("Menu reference requires uid1001 in the disposable Docker container")
    root = Path(sys.argv[1]).resolve()
    if not str(root).startswith("/review/cases/menu-complete-"):
        raise SystemExit("Menu reference fixtures must use /review/cases/menu-complete-*")
    root.mkdir(parents=True)
    left, right = root / "left", root / "right"
    left.mkdir()
    right.mkdir()
    search = root / "search"
    (search / "sought").mkdir(parents=True)
    for name in ("7", "a space.txt", "b-two.txt"):
        (left / name).write_text("menu reference\n")
    (left / ".mc.menu").write_text(
        "r Record native macro words\n"
        "    printf '%%s\\0' %f %0f \"%0f\" %0var{L20_UNSET:raw words} "
        "%t \"%t\" '%t' \"pre%t/post\" "
        "> " + str(root / "words.bin") + "\n"
        "    printf '%%s\\0' \"`printf '%%s' %f`\" $'%f' "
        "$((%f+2)) > " + str(root / "contexts.bin") + "\n"
        "    cat <<'L20_LITERAL' > " + str(root / "heredoc.bin") + "\n"
        "    %f\n"
        "    %0f\n"
        "    %t\n"
        "    L20_LITERAL\n"
        "    printf done > " + str(root / "done") + "\n"
    )
    config = root / "ui/config/mc"
    config.mkdir(parents=True)
    (config / "ini").write_text("[Midnight-Commander]\npause_after_run=0\n")
    os.environ["LD_LIBRARY_PATH"] = "/review/control/lib"
    os.environ["CDPATH"] = str(search)
    with TerminalSession("/review/control/mc", "/usr/bin/tmux", root / "ui",
                         ["-u", left, right]) as terminal:
        terminal.record_output()
        terminal.wait_for("7")
        terminal.press("Home", "Down", "Down", "Down", "Insert", "Insert",
                       "Home", "Down", "Down")
        terminal.press("F2")
        terminal.wait_for("Record native macro words")
        terminal.press("r")
        terminal.wait_file(root / "done", b"done")
        terminal.wait_idle()
        terminal.type("cd sought")
        terminal.press("Enter")
        terminal.wait_for("/search/sought")
        terminal.save("cdpath")
        terminal.type("cd ~mcpp")
        terminal.press("Enter")
        terminal.wait_for("/home/mcpp")
        terminal.save("named-home")
        result = {
            "words": (root / "words.bin").read_bytes().decode().split("\0"),
            "contexts": (root / "contexts.bin").read_bytes().decode().split("\0"),
            "heredoc": (root / "heredoc.bin").read_text(),
            "cdpath": str(search / "sought"),
            "named_home": "/home/mcpp",
        }
        (root / "observations.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
