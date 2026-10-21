#!/usr/bin/env python3
"""One reserved, hash-pinned UI group for built-in manager/editor captions."""

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import unicodedata

from terminal_session import SGR, TerminalSession, style_sgr


BINARY = Path("/home/mcpp/src/build/local-parity/menu-frame-status-next/mcpp")
BINARY_SHA256 = "270425ee58de7cfddd2884ea6a36f1512e0e01dae9fca8def6fa809d0f2dfe3b"
FIXTURES = Path("/review/cases/builtin-menu-captions-b794-v1/positive")
CATALOG = FIXTURES / "menus-le/LC_MESSAGES/mcpp.mo"
CATALOG_SHA256 = "0b27c7ed38afc37abea6c2a35e5710b1908b6bc3e061eb618123c2cb9c776d6c"
CONTEXTS = ("manager-panel-menu", "manager-file-menu", "manager-command-menu",
            "manager-options-menu", "manager-panel-menu")
OFFSETS = (0, 9, 18, 30, 42)
WIDTHS = (27, 28, 40, 21, 27)
EDITOR_NAMES = ("File", "Edit", "Search", "Command", "Format", "Window", "Options")
EDITOR_FIRST_ROWS = ("Open file...", "Undo", "Search...", "Go to line...",
                     "Insert literal...", "Move", "General...")
EDITOR_OFFSETS = (0, 9, 18, 29, 41, 52, 63)
EDITOR_WIDTHS = (23, 30, 23, 39, 26, 22, 27)
FILENAMES = ("Copy", "Edit", "View", "Éditer-café.txt")
PAYLOAD = "Copy\nView\nEdit\nliteral café Żółć\n".encode()
MENU = b"a Copy\n    : %{Edit}\nb View\n    :\n"
INPUT_LITERAL = "Copy/View/Éditer"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def private_directory(path):
    require(not path.exists(), f"directory must be fresh: {path}")
    path.mkdir(mode=0o700)
    state = path.lstat()
    require(stat.S_ISDIR(state.st_mode) and state.st_uid == 1001 and
            not state.st_mode & 0o077, f"not an owned private directory: {path}")


def cells(text):
    return sum(0 if unicodedata.combining(char) else
               2 if unicodedata.east_asian_width(char) in ("W", "F") else 1 for char in text)


def cell_slice(text, start, end):
    selected, column = [], 0
    for char in text:
        width = cells(char)
        if start <= column and column + width <= end:
            selected.append(char)
        column += width
        if column >= end:
            break
    return "".join(selected)


def underlined_spans(ansi):
    active, rows = False, []
    for line in ansi.splitlines():
        spans, offset = [], 0
        for token in SGR.split(line):
            if token.startswith("\x1b["):
                active = style_sgr(active, token, 4, 24)
            else:
                if active and token:
                    spans.append((offset, offset + len(token)))
                offset += len(token)
        rows.append(spans)
    return rows


def capture(terminal, evidence, name):
    terminal.wait_idle()
    ansi = terminal.text(ansi=True)
    text = SGR.sub("", ansi)
    require("\ufffd" not in text, f"{name}: replacement character in UTF-8 UI")
    (evidence / (name + ".ansi")).write_text(ansi, encoding="utf-8")
    (evidence / (name + ".txt")).write_text(text, encoding="utf-8")
    return text, ansi


def expected_data():
    # These are frozen expectations from the already-passed model, not another model run.
    translations = {}
    with (FIXTURES / "oracle.tsv").open(encoding="utf-8", newline="") as source:
        for locale, _, context, original, translated in csv.reader(source, delimiter="\t"):
            if locale == "menus-le":
                translations[(context, original)] = translated
    groups = [[] for _ in CONTEXTS]
    with (FIXTURES / "source-manager.tsv").open(encoding="utf-8", newline="") as source:
        for category, index, label, action, _ in csv.reader(source, delimiter="\t"):
            category, index, action = int(category), int(index), int(action)
            require(index == len(groups[category]), "frozen source row order differs")
            if action == -2:
                groups[category].append({"separator": True})
                continue
            candidates = [original for context, original in translations
                          if context == CONTEXTS[category] and
                          (label == original or label.startswith(original + " "))]
            require(candidates, f"frozen built-in row has no caption expectation: {label}")
            original = max(candidates, key=len)
            groups[category].append({
                "separator": False, "original": original, "label": label,
                "translated": translations[(CONTEXTS[category], original)],
                "suffix": label[len(original):], "action": action,
            })
    require(tuple(map(len, groups)) == (16, 23, 22, 10, 16),
            "frozen manager row counts differ")
    editor = [translations[("editor-menubar", name)] for name in EDITOR_NAMES]
    return groups, editor


def selected_row(terminal, category, count):
    left, right = OFFSETS[category] + 1, OFFSETS[category] + WIDTHS[category] - 1
    focused = []
    for index, row in enumerate(terminal.styled_rows()[2:2 + count]):
        if any(start < right and end > left for start, end in row["spans"]):
            focused.append(index)
    require(len(focused) == 1, f"category {category}: expected one selected row, got {focused}")
    return focused[0]


def check_manager(terminal, category, rows, translated, evidence, name):
    text, ansi = capture(terminal, evidence, name)
    lines, underlines = text.splitlines(), underlined_spans(ansi)
    left, right = OFFSETS[category] + 1, OFFSETS[category] + WIDTHS[category] - 1
    shortcuts, disabled = [], []
    for index, row in enumerate(rows):
        if row["separator"]:
            continue
        caption = row["translated"] if translated else row["original"]
        content = cell_slice(lines[index + 2], left, right)
        require(content.startswith(" " + caption),
                f"{name}/{index}: caption differs: {content!r}, expected {caption!r}")
        if caption == row["original"]:
            require(content.rstrip() == " " + row["label"],
                    f"{name}/{index}: original English padding/shortcut changed: {content!r}")
        else:
            start = left + 1
            require(not any(a < start + len(caption) and b > start
                            for a, b in underlines[index + 2]),
                    f"{name}/{index}: translated caption invented an underlined accelerator")
        shortcut = row["suffix"].strip()
        if shortcut:
            require(content.rstrip().endswith(shortcut),
                    f"{name}/{index}: original shortcut missing or clipped")
            end = content.rfind(shortcut) + len(shortcut)
            end_column = cells(content[:end])
            if caption != row["original"]:
                require(end_column == right - left,
                        f"{name}/{index}: translated shortcut did not keep its reserved right edge")
            shortcuts.append({"row": index, "key": shortcut, "end_column": end_column})
        if row["action"] < 0:
            disabled.append(index)
    require(selected_row(terminal, category, len(rows)) == 0,
            f"{name}: opening/category navigation changed the initial selection")
    return {"category": category, "rows": len(rows), "shortcuts": shortcuts, "disabled_rows": disabled}


def manager_menus(terminal, groups, translated, evidence, phase):
    terminal.press("F9")
    terminal.wait_idle()
    observations = []
    for category, rows in enumerate(groups):
        if category:
            terminal.step("Right")
        observations.append(check_manager(
            terminal, category, rows, translated, evidence, f"{phase}-manager-{category}"))
        if translated and category == 0:
            for expected in (15, 13, 8):
                terminal.step("Up")
                require(selected_row(terminal, 0, len(rows)) == expected,
                        "Up selected a separator or disabled network entry")
            capture(terminal, evidence, phase + "-disabled-network-skipped")
            terminal.step("Down")
            require(selected_row(terminal, 0, len(rows)) == 13,
                    "Down did not skip all three disabled network entries")
            terminal.press("F9")
            terminal.wait_for("Drzewo", present=False)
            terminal.press("F9")
            terminal.wait_for("Drzewo")
    terminal.press("F9")
    terminal.wait_for("Drzewo" if translated else "Listing format...", present=False)
    return observations


def literal_user_menu(terminal, evidence, phase):
    terminal.press("F2")
    terminal.wait_for("User menu")
    text, _ = capture(terminal, evidence, phase + "-literal-user-menu")
    require(re.search(r"\ba\s+Copy\b", text) and re.search(r"\bb\s+View\b", text),
            "literal custom-menu labels were translated")
    terminal.press("Enter")
    terminal.wait_for("[< OK >]")
    text, _ = capture(terminal, evidence, phase + "-literal-function-argument")
    require(re.search(r"[│║]\s*Edit\s*[│║]", text),
            "literal native-menu function argument was translated")
    terminal.type(INPUT_LITERAL)
    terminal.wait_for(INPUT_LITERAL)
    capture(terminal, evidence, phase + "-literal-input")
    terminal.press("Escape")
    terminal.wait_for("[< OK >]", present=False)
    terminal.press("Escape")
    terminal.wait_for("User menu", present=False)


def editor_menus(terminal, captions, evidence, phase):
    terminal.press("F4")
    terminal.wait_editor(dirty=False)
    terminal.wait_for("literal café Żółć")
    terminal.press("F9")
    terminal.wait_for("Open file...")
    expected = "     ".join(captions)
    observations = []
    for category, first_row in enumerate(EDITOR_FIRST_ROWS):
        if category:
            terminal.step("Right")
        text, ansi = capture(terminal, evidence, f"{phase}-editor-{category}")
        require(text.splitlines()[0].startswith(expected),
                f"{phase}/{category}: seven editor captions or original suffix spacing changed")
        popup = cell_slice(text.splitlines()[2], EDITOR_OFFSETS[category] + 1,
                           EDITOR_OFFSETS[category] + EDITOR_WIDTHS[category] - 1)
        require(popup.lstrip().startswith(first_row),
                f"{phase}/{category}: arrow navigation reached the wrong editor menu")
        if phase.startswith("translated"):
            position = 0
            spans = underlined_spans(ansi)[0]
            for caption in captions:
                require(not any(a < position + len(caption) and b > position for a, b in spans),
                        "translated editor caption invented a byte-based mnemonic")
                position += len(caption) + 5
        observations.append({"category": category, "caption": captions[category],
                             "original_dropdown": first_row})
    terminal.step("Right")
    terminal.wait_for("Open file...")
    terminal.press("Enter")
    terminal.wait_for("Open file...", present=False)
    terminal.wait_for("Open file")
    text, _ = capture(terminal, evidence, phase + "-editor-enter-dispatch")
    require("Copy" in text, "editor menu Enter dispatch changed the literal filename")
    terminal.press("Escape")
    terminal.wait_for("Open file", present=False)
    terminal.wait_editor(dirty=False)
    terminal.press("F10")
    terminal.wait_for("literal café Żółć", present=False)
    return observations


def exercise(binary, expected_hash, tmux, root, evidence, result, resume=False):
    left, right = root / "left", root / "right"
    if not resume:
        private_directory(left)
        private_directory(right)
        for name in FILENAMES:
            (left / name).write_bytes(PAYLOAD)
        result["resources"] = {
            str(path): digest(path) for path in [CATALOG, FIXTURES / "oracle.tsv",
                                               FIXTURES / "source-manager.tsv",
                                               *(left / name for name in FILENAMES)]
        }
        result["phases"] = {}
    else:
        require(all(Path(name).is_file() and digest(Path(name)) == value
                    for name, value in result["resources"].items()),
                "a preserved resource changed before resuming the same UI group")
        require(len(result["phases"]["translated"]["manager"]) == 5 and
                "c-fallback" not in result["phases"],
                "only the unfinished user-text/editor/C-fallback checkpoint can be resumed")
    groups, editor = expected_data()
    for phase, locale, translated in (("translated", "menus-le", True), ("c-fallback", "C", False)):
        require(digest(binary) == expected_hash, "matching private binary changed before launch")
        capture_phase = phase + "-tail" if resume and translated else phase
        ui = root / capture_phase
        private_directory(ui)
        preferences = ui / "config/mcpp"
        preferences.mkdir(parents=True)
        (preferences / "config").write_text(
            "theme=gray\nuse_subshell=false\nconfirm_exit=false\n"
            "drop_menus=true\nmenubar_visible=true\n"
            "use_internal_view=true\nuse_internal_edit=true\n", encoding="utf-8")
        menu = preferences / "menu"
        menu.write_bytes(MENU)
        result["resources"][str(menu)] = digest(menu)
        command = [
            "-u", "LC_ALL", "-u", "MCPP_LOCALEDIR", "LC_CTYPE=C.UTF-8",
            "LC_MESSAGES=" + locale, "LANGUAGE=menus-le", "MCPP_LOCALEDIR=" + str(FIXTURES),
            str(binary), "-u", "--nokeymap", "--", str(left), str(right),
        ]
        observation = (result["phases"]["translated"] if resume and translated else
                       {"message_locale": locale, "passed": False})
        result["phases"][phase] = observation
        with TerminalSession("/usr/bin/env", tmux, ui, command, width=120, height=32,
                             record_exit=True, record_stderr=True) as terminal:
            terminal.wait_for("Éditer-café.txt")
            terminal.step("Home", "Down")
            if not (resume and translated):
                observation["manager"] = manager_menus(terminal, groups, translated, evidence, phase)
            if translated:
                literal_user_menu(terminal, evidence, capture_phase)
            terminal.step("F9", "Right")
            terminal.press("Enter")
            terminal.wait_for("literal café Żółć")
            text, _ = capture(terminal, evidence, capture_phase + "-manager-enter-literal-viewer")
            require("Copy" in text and "View" in text and "Edit" in text,
                    "manager Enter dispatch translated literal filename/file content")
            terminal.press("F10")
            terminal.wait_for("literal café Żółć", present=False)
            observation["editor"] = editor_menus(
                terminal, editor if translated else EDITOR_NAMES, evidence, capture_phase)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
            stderr = terminal.stderr_file.read_bytes()
            (evidence / (capture_phase + "-stderr.txt")).write_bytes(stderr)
            require(not stderr, f"{phase}: unexpected runtime diagnostics: {stderr!r}")
        require(sorted(path.name for path in left.iterdir()) == sorted(FILENAMES) and
                not list(right.iterdir()), "UI navigation changed filenames or left editor locks behind")
        require(all((left / name).read_bytes() == PAYLOAD for name in FILENAMES),
                "UI navigation changed file contents")
        observation.update({"passed": True, "exit_status": 0, "stderr_empty": True})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--tmux", default="/usr/bin/tmux")
    parser.add_argument("--resume", type=Path, help="continue only this controller's unfinished UI checkpoint")
    arguments = parser.parse_args()
    require(os.getuid() == os.geteuid() == 1001 and Path("/.dockerenv").is_file() and
            Path("/review/.operation-regressions-disposable").is_file(),
            "requires the approved UID-1001 disposable Docker container")
    binary = arguments.binary.resolve(strict=True)
    require(binary == BINARY and arguments.sha256 == BINARY_SHA256 and
            digest(binary) == arguments.sha256 and os.access(binary, os.X_OK),
            "requires the parent's exact matching private App/core/all-header build")
    require(digest(CATALOG) == CATALOG_SHA256, "original frozen new-caption fixture changed")
    os.umask(0o077)
    if arguments.resume:
        root = arguments.resume.resolve(strict=True)
        require(root.parent == Path("/review/cases") and root.name.startswith("builtin-menu-ui-b794-") and
                root.stat().st_uid == 1001 and not root.stat().st_mode & 0o077,
                "resume root must be this owned private UI case")
        evidence = Path("/review/evidence/session") / root.name
        require(evidence.is_dir() and evidence.stat().st_uid == 1001 and
                not evidence.stat().st_mode & 0o077, "resume evidence must remain owned and private")
        result = json.loads((root / "result.json").read_text(encoding="utf-8"))
        require(result["binary"] == str(binary) and result["binary_sha256_before"] == arguments.sha256 and
                result["resources_unchanged"] and not result["passed"] and
                "Expected 'User menu'" in result.get("error", ""),
                "resume requires the recorded pre-editor controller synchronization failure")
        result["resumed_from"] = str(root / "result.json")
        result["controller_note"] = "Resumed unfinished tail only after explicit popup-close synchronization; preserved all passed manager observations."
        result.pop("error", None)
    else:
        root = Path("/review/cases") / ("builtin-menu-ui-b794-" + str(os.getpid()))
        evidence = Path("/review/evidence/session") / root.name
        private_directory(root)
        private_directory(evidence)
        result = {
            "case": "one-reserved-builtin-menu-caption-actual-ui", "binary": str(binary),
            "binary_sha256_before": arguments.sha256, "root": str(root), "evidence": str(evidence),
            "catalog_sha256_before": CATALOG_SHA256, "model_rerun": False, "passed": False,
        }
    os.chdir(root)
    try:
        exercise(binary, arguments.sha256, arguments.tmux, root, evidence, result, resume=bool(arguments.resume))
        result["passed"] = True
    except BaseException as error:
        result["error"] = repr(error)
        raise
    finally:
        result["binary_sha256_after"] = digest(binary)
        result["resources_after"] = {
            name: digest(Path(name)) if Path(name).is_file() else None
            for name in result.get("resources", {})
        }
        result["resources_unchanged"] = result["resources_after"] == result.get("resources", {})
        result["passed"] = (result["passed"] and result["resources_unchanged"] and
                            result["binary_sha256_after"] == arguments.sha256)
        encoded = json.dumps(result, indent=2, ensure_ascii=False) + "\n"
        filename = "result-tail.json" if arguments.resume else "result.json"
        (root / filename).write_text(encoded, encoding="utf-8")
        (evidence / filename).write_text(encoded, encoding="utf-8")
    require(result["passed"], "matching binary or literal user/catalog resources changed")
    print("PASS reserved UI: typed manager/editor captions, shortcuts, disabled rows, UTF-8, literal data, C fallback")
    print(evidence / filename)


if __name__ == "__main__":
    main()
