#!/usr/bin/env python3
"""One hash-pinned L26 UI case: Polish labels and C-locale English fallback."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat

from terminal_session import SGR, TerminalSession


COMMON = Path("/home/mcpp/src/build/local-parity/mcpp")
COMMON_SHA256 = "507f0060e933236faa8f81224637bfe59d463bae665637ac49c57b069b82eae3"
CATALOG_ROOT = Path("/review/cases/l26-translation/fixtures-frozen/locale")
CATALOG = CATALOG_ROOT / "pl/LC_MESSAGES/mcpp.mo"
CATALOG_SHA256 = "f77758776c78960f873e65bb60467f30041660d6aaa5f5a299d77d134b384465"
FILENAME = "File-café-┌│.txt"
PAYLOAD = "File\nCancel\nQuit\nZażółć café ┌│\n".encode()
INPUT_LITERAL = "File/Cancel/café-Żółć"
MENU = b"a Cancel\n    : %{File}\n"
QUIT_MESSAGE = "Do you really want to quit?"
PHASES = (
    ("translated", {
        "menus": ["Lewy", "Plik", "Polecenie", "Opcje", "Prawy"],
        "ok": "OK", "cancel": "Anuluj", "quit": "Wyjdź", "yes": "Tak", "no": "Nie",
    }),
    ("c-english-fallback", {
        "menus": ["Left", "File", "Command", "Options", "Right"],
        "ok": "OK", "cancel": "Cancel", "quit": "Quit", "yes": "Yes", "no": "No",
    }),
)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(65536), b""):
            result.update(block)
    return result.hexdigest()


def private_directory(path):
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    state = path.lstat()
    require(stat.S_ISDIR(state.st_mode) and state.st_uid == 1001 and
            not state.st_mode & 0o077, f"not an owned private directory: {path}")


def capture(terminal, evidence, name):
    terminal.wait_idle()
    ansi = terminal.text(ansi=True)
    text = SGR.sub("", ansi)
    (evidence / (name + ".ansi")).write_text(ansi)
    (evidence / (name + ".txt")).write_text(text)
    return text


def menu_line(text, captions):
    pattern = r"\s*" + r"\s+".join(map(re.escape, captions)) + r"\s*"
    return next((line for line in text.splitlines()[:2] if re.fullmatch(pattern, line)), None)


def standalone(text, value):
    pattern = r"(?m)^\s*(?:[│║]\s*)?" + re.escape(value) + r"\s*(?:[│║]\s*)?$"
    return re.search(pattern, text) is not None


def exercise(binary, expected_hash, tmux, root, evidence, result):
    left, right = root / "left", root / "right"
    left.mkdir()
    right.mkdir()
    document = left / FILENAME
    document.write_bytes(PAYLOAD)
    resources = {str(document): digest(document), str(CATALOG): digest(CATALOG)}
    result["resource_sha256_before"] = resources
    result["phases"] = {}

    for name, labels in PHASES:
        require(digest(binary) == expected_hash, "private binary changed before a UI launch")
        ui = root / name
        preferences = ui / "config/mcpp"
        preferences.mkdir(parents=True)
        (preferences / "config").write_text(
            "theme=gray\nuse_subshell=false\nconfirm_exit=true\n"
            "menubar_visible=true\nuse_internal_view=true\n"
        )
        menu = preferences / "menu"
        menu.write_bytes(MENU)
        resources[str(menu)] = digest(menu)

        # The shared harness sets LC_ALL=C.UTF-8. Unset it only for the child,
        # retaining UTF-8 LC_CTYPE while selecting the requested message locale.
        message_locale = "pl_PL.UTF-8" if name == "translated" else "C"
        command = [
            "-u", "LC_ALL", "-u", "MCPP_LOCALEDIR",
            "LC_CTYPE=C.UTF-8", "LC_MESSAGES=" + message_locale, "LANGUAGE=pl",
            "MCPP_LOCALEDIR=" + str(CATALOG_ROOT),
        ]
        command += [str(binary), "-u", "--nokeymap", "--", str(left), str(right)]
        phase = {"expected_labels": labels, "catalog_override": True,
                 "message_locale": message_locale, "language": "pl"}
        result["phases"][name] = phase
        with TerminalSession("/usr/bin/env", tmux, ui, command, width=120, height=32,
                             record_exit=True, record_stderr=True) as terminal:
            terminal.wait_for(FILENAME)
            terminal.wait(lambda: menu_line(terminal.text(), labels["menus"]) is not None,
                          f"{name}: all five main-menu captions")
            screen = capture(terminal, evidence, name + "-menus")
            phase["observed_menu_line"] = menu_line(screen, labels["menus"])

            terminal.press("F9", "Down")
            terminal.wait_for("Listing format")
            capture(terminal, evidence, name + "-f9")
            terminal.press("Escape")
            terminal.wait_for("Listing format", present=False)

            terminal.press("F2")
            terminal.wait_for("User menu")
            screen = capture(terminal, evidence, name + "-user-menu")
            require("Cancel" in screen, "literal user-menu entry was translated")
            terminal.press("Enter")
            primary = "[< " + labels["ok"] + " >]"
            cancel = "[ " + labels["cancel"] + " ]"
            terminal.wait(lambda: primary in terminal.text() and cancel in terminal.text(),
                          f"{name}: native-input buttons")
            terminal.type(INPUT_LITERAL)
            terminal.wait_for(INPUT_LITERAL)
            screen = capture(terminal, evidence, name + "-input")
            require(primary in screen and cancel in screen and INPUT_LITERAL in screen,
                    "native-input labels or literal Unicode input differ")
            require(re.search(r"\sFile\s", screen) is not None,
                    "literal user-menu prompt text was translated")
            terminal.press("Tab", "Tab", "Enter")
            terminal.wait_for(primary, present=False)
            terminal.press("Escape")
            terminal.wait_for("User menu", present=False)

            terminal.press("Home", "Down", "F3")
            terminal.wait_for("Zażółć café ┌│")
            screen = capture(terminal, evidence, name + "-literal-viewer")
            require(all(standalone(screen, value) for value in ("File", "Cancel", "Quit")),
                    "file-content words matching UI labels were translated")
            require(FILENAME in screen and "Zażółć café ┌│" in screen,
                    "literal Unicode filename or file content was modified")
            terminal.press("F10")
            terminal.wait_for("Zażółć café ┌│", present=False)
            terminal.wait_for(FILENAME)

            terminal.press("F10")
            terminal.wait_for(QUIT_MESSAGE)
            screen = capture(terminal, evidence, name + "-quit")
            require(labels["quit"] in screen and
                    "[< " + labels["yes"] + " >]" in screen and
                    "[ " + labels["no"] + " ]" in screen,
                    "quit title/choices differ from the selected label catalog")
            terminal.press("Tab", "Enter")
            terminal.wait_for(QUIT_MESSAGE, present=False)
            terminal.wait(lambda: menu_line(terminal.text(), labels["menus"]) is not None,
                          "declining quit returns to the manager")
            terminal.press("F10")
            terminal.wait_for(QUIT_MESSAGE)
            terminal.press("Enter")
            terminal.wait_file(terminal.exit_file, b"0\n")
            stderr = terminal.stderr_file.read_bytes()
            (evidence / (name + "-stderr.txt")).write_bytes(stderr)
            require(stderr == b"", f"unexpected translation/startup diagnostic: {stderr!r}")

        require(document.read_bytes() == PAYLOAD, "literal file bytes changed")
        require(menu.read_bytes() == MENU, "literal user-menu resource changed")
        require([path.name for path in left.iterdir()] == [FILENAME] and
                list(right.iterdir()) == [], "literal filenames or pane contents changed")
        phase.update({"passed": True, "input_buttons": [labels["ok"], labels["cancel"]],
                      "literal_bytes_unchanged": True, "exit_status": 0, "stderr_empty": True})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="parent-published private translation mcpp")
    parser.add_argument("--sha256", required=True, help="parent-published exact binary SHA-256")
    parser.add_argument("--tmux", nargs="?", const="/usr/bin/tmux", default="/usr/bin/tmux")
    arguments = parser.parse_args()
    if (os.getuid() != 1001 or os.geteuid() != 1001 or
            not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file()):
        parser.error("requires UID 1001 in the approved disposable container")
    if re.fullmatch("[0-9a-f]{64}", arguments.sha256) is None:
        parser.error("--sha256 must be the parent's exact lowercase SHA-256")
    binary = arguments.binary.resolve(strict=True)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error("private translation binary must be a regular executable")
    binary_before = digest(binary)
    if binary == COMMON.resolve() or binary_before == COMMON_SHA256:
        parser.error("the pinned common/Cfg binary must not be run by this controller")
    if binary_before != arguments.sha256:
        parser.error("private binary does not match the parent-published SHA-256")
    common_before = digest(COMMON)
    if common_before != COMMON_SHA256:
        parser.error("common/Cfg binary changed; stop before any UI launch")
    if not CATALOG_ROOT.is_absolute() or digest(CATALOG) != CATALOG_SHA256:
        parser.error("the original three-case Polish catalog is missing or changed")

    os.umask(0o077)
    os.chdir("/review/cases")
    base = Path("l26-translation-ui")
    private_directory(base)
    root = (base / ("run-" + str(os.getpid()))).absolute()
    root.mkdir()
    evidence = Path("/review/evidence/session/l26-translation-ui")
    private_directory(evidence)
    evidence = evidence / root.name
    evidence.mkdir()
    os.chdir(root)
    result = {
        "case": "fourth-single-L26-actual-UI", "root": str(root),
        "binary": str(binary), "binary_sha256_before": binary_before,
        "common_binary_sha256_before": common_before,
        "catalog_root": str(CATALOG_ROOT), "catalog_sha256": CATALOG_SHA256,
        "native_rerun": False, "model_rerun": False, "passed": False,
    }
    try:
        exercise(binary, arguments.sha256, arguments.tmux, root, evidence, result)
        result["passed"] = True
    except BaseException as error:
        result["error"] = repr(error)
        raise
    finally:
        result["binary_sha256_after"] = digest(binary)
        result["common_binary_sha256_after"] = digest(COMMON)
        result["binaries_unchanged"] = (
            result["binary_sha256_after"] == binary_before and
            result["common_binary_sha256_after"] == common_before
        )
        resources = result.get("resource_sha256_before", {})
        result["resource_sha256_after"] = {
            name: digest(Path(name)) if Path(name).is_file() else None for name in resources
        }
        result["resources_unchanged"] = result["resource_sha256_after"] == resources
        result["passed"] = result["passed"] and result["binaries_unchanged"] and result["resources_unchanged"]
        encoded = json.dumps(result, indent=2, ensure_ascii=False) + "\n"
        Path("result.json").write_text(encoded)
        (evidence / "result.json").write_text(encoded)
    require(result["passed"], "binary or original user/catalog resources changed")
    print("PASS fourth L26 UI: menus, input/quit buttons, literal Unicode data, English fallback")
    print(evidence / "result.json")


if __name__ == "__main__":
    main()
