from pathlib import Path
import json
import os
import sys

from terminal_session import TerminalSession


def numbers(root):
    left, right, ui = root / "left", root / "right", root / "ui"
    left.mkdir()
    right.mkdir()
    answer = root / "answer"
    (left / ".mc.menu").write_text(
        "n Hex numeric input\n"
        "    printf '%%s' $((%{Integer}+2)) > " + str(answer) + "\n"
    )
    config = ui / "config/mc"
    config.mkdir(parents=True)
    (config / "ini").write_text(
        "[Midnight-Commander]\npause_after_run=0\nconfirm_exit=0\n"
    )
    os.environ["LD_LIBRARY_PATH"] = "/review/control/lib"
    results = {}
    with TerminalSession("/review/control/mc", "/usr/bin/tmux", ui,
                         ["-u", left, right]) as terminal:
        terminal.record_output()
        terminal.wait_for("Name")
        for value, expected in (("0x2a", "44"), ("0Xf", "17")):
            terminal.press("F2")
            terminal.wait_for("Hex numeric input")
            terminal.press("n")
            terminal.wait_for("Integer")
            terminal.press("C-u")
            terminal.type(value)
            terminal.press("Enter")
            terminal.wait_file(answer, expected.encode())
            terminal.wait_idle()
            results[value] = answer.read_text()
        terminal.save("native-hexadecimal-arithmetic")
    (root / "observations.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(results, indent=2))


def cases(root):
    left, right, ui = root / "left", root / "right", root / "ui"
    left.mkdir()
    right.mkdir()
    (left / "chosen").write_text("case fixture\n")
    commands = {
        "basic": ("$(case %f in (chosen|other) printf basic;; "
                  "(*) printf miss;; esac)", b"basic\0"),
        "patterns": ("$(case ')' in [\\)]|\\)|'case in esac)') printf patterns;; "
                     "*) printf miss;; esac)", b"patterns\0"),
        "nested": ("$(case \"$(printf '%%s' %f)\" in chosen) "
                   "case x in (x) printf '%%s' case in esac "
                   "\"$(case inner in inner) printf nested;;esac)\";; "
                   "esac;; esac)", b"caseinesacnested\0"),
        "fallthrough": ("$(case %f in chosen) printf A ;& "
                        "ignored) printf B ;;& chosen) printf C ;; "
                        "*) printf miss ;; esac)", b"ABC\0"),
    }
    menu = "c Record command substitution cases\n    /bin/bash <<'L20_CASE_BASH'\n"
    for name, (command, _) in commands.items():
        menu += "    printf '%%s\\0' \"" + command + "\" > " + str(root / (name + ".bin")) + "\n"
    menu += "    printf done > " + str(root / "done") + "\n    L20_CASE_BASH\n"
    (left / ".mc.menu").write_text(menu)
    config = ui / "config/mc"
    config.mkdir(parents=True)
    (config / "ini").write_text(
        "[Midnight-Commander]\npause_after_run=0\nconfirm_exit=0\n"
        "show_dot_files=0\n"
    )
    os.environ["LD_LIBRARY_PATH"] = "/review/control/lib"
    with TerminalSession("/review/control/mc", "/usr/bin/tmux", ui,
                         ["-u", left, right]) as terminal:
        terminal.record_output()
        terminal.wait_for("chosen")
        terminal.press("End", "F2")
        terminal.wait_for("Record command substitution cases")
        terminal.press("c")
        terminal.wait_file(root / "done", b"done")
        for name, (_, expected) in commands.items():
            terminal.wait_file(root / (name + ".bin"), expected)
        terminal.wait_idle()
        terminal.save("native-case-substitutions")
    results = {
        name: (root / (name + ".bin")).read_bytes().decode()
        for name in commands
    }
    (root / "observations.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(results, indent=2))


def extglobs(root):
    left, right, ui = root / "left", root / "right", root / "ui"
    left.mkdir()
    right.mkdir()
    (left / "selected").write_text("extended case fixture\n")
    runner = root / "bash-extglob"
    runner.write_text('#!/bin/sh\nexec /bin/bash -O extglob "$@"\n')
    runner.chmod(0o700)
    answer = "literal ' \" @(?|*) ) | \\ $(printf DATA)"
    command = (
        "printf '%%s\\0' \"$(case abbz-tail in "
        "(@(%f|[()]|literal\\)|'quoted|)'|?(@(a|x))*(b)+(z|q)!(blocked))) "
        "printf '%%s' %{Extglob bytes};; *) printf miss;; esac)\" "
        "%f %0var{L20_EXTGLOB_WORDS:raw words} '@(?|*)'"
    )
    malformed = "printf '%%s' \"$(case x in @(?(@(x|y))) printf bad;; esac)\""
    (left / ".mc.menu").write_text(
        "g Record extended case patterns\n"
        "    " + str(runner) + " <<'L20_EXTGLOB_BASH'\n"
        "    shopt -p extglob > " + str(root / "extglob-state") + "\n"
        "    " + command + " > " + str(root / "nested.bin") + "\n"
        "    L20_EXTGLOB_BASH\n"
        "    " + str(runner) + " -n <<'L20_EXTGLOB_INVALID' 2> " +
        str(root / "invalid.stderr") + "\n"
        "    " + malformed + "\n"
        "    L20_EXTGLOB_INVALID\n"
        "    printf '%%s' \"$?\" > " + str(root / "invalid.status") + "\n"
        "    printf done > " + str(root / "done") + "\n"
    )
    config = ui / "config/mc"
    config.mkdir(parents=True)
    (config / "ini").write_text(
        "[Midnight-Commander]\npause_after_run=0\nconfirm_exit=0\n"
        "show_dot_files=0\n"
    )
    os.environ["LD_LIBRARY_PATH"] = "/review/control/lib"
    # MC 4.8.30 leaves this raw environment form literal; do not mistake it for a pattern error.
    expected = b"".join(value.encode() + b"\0" for value in (
        answer, "selected", "%var{L20_EXTGLOB_WORDS:raw", "words}", "@(?|*)",
    ))
    with TerminalSession("/review/control/mc", "/usr/bin/tmux", ui,
                         ["-u", left, right]) as terminal:
        terminal.record_output()
        terminal.wait_for("selected")
        terminal.press("End", "F2")
        terminal.wait_for("Record extended case patterns")
        terminal.press("g")
        terminal.wait_for("Extglob bytes")
        terminal.type(answer)
        terminal.press("Enter")
        terminal.wait_file(root / "done", b"done")
        terminal.wait_file(root / "nested.bin", expected)
        terminal.wait_file(root / "extglob-state", b"shopt -s extglob\n")
        terminal.wait_file(root / "invalid.status", b"2")
        terminal.wait_idle()
        terminal.save("native-extended-case-patterns")
    results = {
        "runner": runner.read_text(),
        "nested_hex": (root / "nested.bin").read_bytes().hex(),
        "extglob_state": (root / "extglob-state").read_text(),
        "invalid_status": (root / "invalid.status").read_text(),
        "invalid_stderr": (root / "invalid.stderr").read_text(),
    }
    (root / "observations.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(results, indent=2))


def main():
    if (os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file()):
        raise SystemExit("Menu reference requires uid1001 in the disposable Docker container")
    root = Path(sys.argv[1]).resolve()
    if not str(root).startswith("/review/cases/menu-complete-"):
        raise SystemExit("Menu reference fixtures must use /review/cases/menu-complete-*")
    root.mkdir(parents=True)
    if len(sys.argv) > 2:
        if sys.argv[2] == "numbers":
            numbers(root)
        elif sys.argv[2] == "cases":
            cases(root)
        elif sys.argv[2] == "extglobs":
            extglobs(root)
        else:
            raise SystemExit("unknown native menu reference selector")
        return
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
