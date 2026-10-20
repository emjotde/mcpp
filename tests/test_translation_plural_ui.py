#!/usr/bin/env python3
"""One reserved UI case: contextual delete caption and three count forms, without deleting."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat

from terminal_session import TerminalSession


OLD_BINARY_SHA256 = "ffd3e925c7dcc3d8f920206026038b84fbfe54b1ab5ce18344743dfaefbbc94c"
LOCALE = "three-le"
FILENAMES = ("Delete", "Delete {count} selected item?", "Delete-café-Żółć.txt",
             "item_04", "item_05")
PAYLOAD = "Delete\nDelete {count} selected item?\nDelete {count} selected items?\ncafé Żółć\n".encode()
QUESTIONS = {1: "Usunąć 1 element?", 2: "Usunąć 2 elementy?", 5: "Usunąć 5 elementów?"}


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


def file_hashes(root):
    return {str(path.relative_to(root)): digest(path) for path in sorted(root.rglob("*"))
            if path.is_file() and not path.is_symlink()}


def standalone(screen, value):
    return re.search(r"[│║][ \t]*" + re.escape(value) + r"[ \t]*[│║]", screen) is not None


def capture(terminal, evidence, name):
    terminal.wait_idle()
    screen = terminal.text()
    (evidence / (name + ".txt")).write_text(screen, encoding="utf-8")
    (evidence / (name + ".ansi")).write_text(terminal.text(ansi=True), encoding="utf-8")
    return screen


def exercise(arguments, root, evidence, result):
    left, right = root / "left", root / "right"
    left.mkdir()
    right.mkdir()
    for name in FILENAMES:
        (left / name).write_bytes(PAYLOAD)
    result["resource_sha256_before"] = {str(left / name): digest(left / name) for name in FILENAMES}
    result["phases"] = {}
    for name, locale, caption in (
            ("translated", LOCALE, "Usuwanie próbne"), ("c-legacy-fallback", "C", "Delete")):
        require(digest(arguments.binary) == arguments.sha256, "private binary changed before launch")
        ui = root / name
        preferences = ui / "config/mcpp"
        preferences.mkdir(parents=True)
        (preferences / "config").write_text(
            "theme=gray\nuse_subshell=false\nconfirm_exit=false\nconfirm_delete=true\nsafe_delete=true\n"
            "menubar_visible=true\nshow_mini_status=true\n", encoding="utf-8")
        command = [
            "-u", "LC_ALL", "-u", "MCPP_LOCALEDIR", "LC_CTYPE=C.UTF-8",
            "LC_MESSAGES=" + locale, "LANGUAGE=" + LOCALE,
            "MCPP_LOCALEDIR=" + str(arguments.catalog_root),
            str(arguments.binary), "-u", "--nokeymap", "--", str(left), str(right),
        ]
        phase = {"message_locale": locale, "caption": caption, "counts": {}, "passed": False}
        phase["cache_files_before"] = file_hashes(ui / "cache")
        result["phases"][name] = phase
        with TerminalSession("/usr/bin/env", arguments.tmux, ui, command, width=120, height=30,
                             record_exit=True, record_stderr=True) as terminal:
            terminal.wait_for("Delete-café-Żółć.txt")

            def select_file(index):
                terminal.press("Home", *(["Down"] * (index + 1)))
                terminal.wait_idle()

            def toggle_file(index):
                select_file(index)
                terminal.press("Insert")
                terminal.wait_idle()

            def check_dialog(count):
                expected = (QUESTIONS[count] if name == "translated" else
                            f"Delete {count} selected item(s)?")
                terminal.press("F8")
                terminal.wait_for(expected)
                screen = capture(terminal, evidence, f"{name}-count-{count}")
                rows = screen.splitlines()
                question_row = next((index for index, row in enumerate(rows)
                                     if standalone(row, expected)), None)
                require(question_row is not None and question_row >= 2,
                        f"{name}/{count}: exact question row differs")
                require(standalone(rows[question_row - 2], caption),
                        f"{name}/{count}: contextual caption differs")
                require("[ Yes ]" in screen or "[< Yes >]" in screen, "literal Yes button changed")
                require("[ No ]" in screen or "[< No >]" in screen, "literal No button changed")
                if count == 1:
                    require(standalone(rows[question_row + 1], "Delete"),
                            "literal selected filename was translated")
                if name != "translated":
                    require("Usunąć" not in screen and "Usuwanie próbne" not in screen,
                            "C locale did not suppress the contextual/plural catalog")
                phase["counts"][str(count)] = {"question": expected, "passed": True}
                terminal.press("Escape")
                terminal.wait_for(expected, present=False)
                terminal.wait_for("Delete cancelled")

            select_file(0)
            check_dialog(1)
            toggle_file(0)
            toggle_file(1)
            check_dialog(2)
            toggle_file(0)
            toggle_file(1)
            for index in range(len(FILENAMES)):
                toggle_file(index)
            check_dialog(5)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
            stderr = terminal.stderr_file.read_bytes()
            (evidence / (name + "-stderr.txt")).write_bytes(stderr)
            require(stderr == b"", f"unexpected UI diagnostics: {stderr!r}")
        require(sorted(path.name for path in left.iterdir()) == sorted(FILENAMES) and
                list(right.iterdir()) == [], "opening/cancelling delete changed filenames")
        require(all((left / filename).read_bytes() == PAYLOAD for filename in FILENAMES),
                "opening/cancelling delete changed literal file contents")
        phase["cache_files_after"] = file_hashes(ui / "cache")
        phase["cache_files_unchanged"] = phase["cache_files_after"] == phase["cache_files_before"]
        phase["existing_cache_files_unchanged"] = all(
            phase["cache_files_after"].get(path) == value
            for path, value in phase["cache_files_before"].items())
        require(phase["existing_cache_files_unchanged"], "delete-dialog checks changed existing cache files")
        phase.update({"passed": True, "stderr_empty": True, "exit_status": 0})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--catalog-root", required=True, type=Path)
    parser.add_argument("--catalog-sha256", required=True)
    parser.add_argument("--tmux", default="/usr/bin/tmux")
    arguments = parser.parse_args()
    require(os.getuid() == 1001 and os.geteuid() == 1001 and
            Path("/.dockerenv").is_file() and
            Path("/review/.operation-regressions-disposable").is_file(),
            "requires the approved UID-1001 disposable Docker container")
    require(all(re.fullmatch("[0-9a-f]{64}", value) for value in
                (arguments.sha256, arguments.catalog_sha256)), "parent must pin both exact SHA-256 values")
    arguments.binary = arguments.binary.resolve(strict=True)
    require(arguments.binary.is_file() and os.access(arguments.binary, os.X_OK) and
            arguments.binary.parent != Path("/home/mcpp/src/build/local-parity") and
            arguments.sha256 != OLD_BINARY_SHA256 and
            digest(arguments.binary) == arguments.sha256,
            "requires a newly published private binary with matching App/core/header objects")
    arguments.catalog_root = arguments.catalog_root.resolve(strict=True)
    catalog = arguments.catalog_root / LOCALE / "LC_MESSAGES/mcpp.mo"
    require(arguments.catalog_root.is_relative_to("/review/cases") and
            digest(catalog) == arguments.catalog_sha256,
            "requires the parent's frozen original three-form model fixture")
    os.umask(0o077)
    os.chdir("/review/cases")
    root = Path("l26-next-ui-" + str(os.getpid())).absolute()
    require(not root.exists(), "UI root must be fresh")
    private_directory(root)
    evidence = Path("/review/evidence/session") / root.name
    require(not evidence.exists(), "UI evidence directory must be fresh")
    private_directory(evidence)
    os.chdir(root)
    result = {
        "case": "nextL26-one-reserved-delete-context-count-UI", "root": str(root),
        "binary": str(arguments.binary), "binary_sha256_before": arguments.sha256,
        "catalog": str(catalog), "catalog_sha256_before": arguments.catalog_sha256,
        "native_rerun": False, "model_rerun": False, "remaining_UI_English": True,
        "passed": False,
    }
    private_artifacts = [arguments.binary.parent / "libmcpp_core.a",
                         arguments.binary.parent / "libmcpp_ui.a"]
    private_artifacts += sorted((arguments.binary.parent / "include").rglob("*.hpp"))
    require(all(path.is_file() for path in private_artifacts) and len(private_artifacts) > 2,
            "matching private libraries and public headers must accompany the binary")
    result["private_artifact_sha256_before"] = {str(path): digest(path) for path in private_artifacts}
    try:
        exercise(arguments, root, evidence, result)
        result["passed"] = True
    except BaseException as error:
        result["error"] = repr(error)
        raise
    finally:
        result["binary_sha256_after"] = digest(arguments.binary)
        result["catalog_sha256_after"] = digest(catalog)
        resources = result.get("resource_sha256_before", {})
        result["resource_sha256_after"] = {
            name: digest(Path(name)) if Path(name).is_file() else None for name in resources
        }
        result["private_artifact_sha256_after"] = {str(path): digest(path) for path in private_artifacts}
        result["passed"] = (
            result["passed"] and result["binary_sha256_after"] == arguments.sha256 and
            result["catalog_sha256_after"] == arguments.catalog_sha256 and
            result["resource_sha256_after"] == resources and
            result["private_artifact_sha256_after"] == result["private_artifact_sha256_before"])
        encoded = json.dumps(result, indent=2, ensure_ascii=False) + "\n"
        Path("result.json").write_text(encoded, encoding="utf-8")
        (evidence / "result.json").write_text(encoded, encoding="utf-8")
    require(result["passed"], "private binary, frozen catalog or literal files changed")
    print("PASS one nextL26 UI case: contextual caption, counts 1/2/5, C legacy fallback, no deletion")
    print(evidence / "result.json")


if __name__ == "__main__":
    main()
