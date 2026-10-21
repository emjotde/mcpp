#!/usr/bin/env python3
"""The single reserved editor-dropdown UI group; requires a matching parent build."""

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


SNAPSHOT = Path("/home/mcpp/src")
FIXTURES = Path("/review/cases/editor-menu-captions-cd58-v1/positive")
CATALOG = FIXTURES / "editor-le/LC_MESSAGES/mcpp.mo"
SOURCE_ROWS = FIXTURES / "source-editor-rows.tsv"
PINNED = {
    CATALOG: "fab7419fa4d6612a40c23ade2e28c20be08f5b353255cc31390ea0d7763add7d",
    SOURCE_ROWS: "e686ae61eb9806baa8254f3e728d4e320f7162c4ff2a9614f50cc0a53b607e91",
    SNAPSHOT / "include/mcpp/translation.hpp":
        "5daaf4b1582d99674661850e49214bbfda609165d3f487849fedaafdeb6062c3",
    SNAPSHOT / "src/translation.cpp":
        "e43e982cdade217be3eb26753bd006600573eed6fa10b4e59f183485919954fa",
    SNAPSHOT / "src/builtin_menu_captions.hpp":
        "c259860f16fc6a89f6e5dd58111ae79c2f1bbf837b24ec353631c8c4cadad858",
}
PRESERVED_SHA256 = {
    "270425ee58de7cfddd2884ea6a36f1512e0e01dae9fca8def6fa809d0f2dfe3b",
    "6654f2b0bd739241b5206a1c5081bc62f6d50f7bb9f231f18b0d1c0e0b38dc99",
}
CONTEXTS = tuple("editor-" + name + "-menu" for name in
                 ("file", "edit", "search", "command", "format", "window", "options"))
OFFSETS = (0, 9, 18, 29, 41, 52, 63)
ENGLISH_WIDTHS = (23, 30, 23, 39, 26, 20, 25)
FILENAME = "Move-café.txt"
PAYLOAD = "Move\nCopy\nNew\nliteral café Żółć é 新規 🧪\n".encode()
MENU = b"a Move\n    :\nb Copy\n    :\n"
PREFERENCES = (
    b"theme=gray\nauto_save_setup=false\nuse_subshell=false\nconfirm_exit=false\n"
    b"drop_menus=true\nmenubar_visible=true\n"
)


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


def width(char):
    return 0 if unicodedata.combining(char) else (
        2 if unicodedata.east_asian_width(char) in ("W", "F") else 1)


def styled_cells(ansi):
    underline, rows = False, []
    for line in ansi.splitlines():
        cells = []
        for token in SGR.split(line):
            if token.startswith("\x1b["):
                underline = style_sgr(underline, token, 4, 24)
                continue
            for char in token:
                count = width(char)
                if count == 0:
                    require(bool(cells), "combining character has no complete preceding glyph")
                    previous = len(cells) - 1
                    while previous and not cells[previous]["text"]:
                        previous -= 1
                    cells[previous]["text"] += char
                else:
                    cells.append({"text": char, "underline": underline})
                    cells.extend({"text": "", "underline": underline} for _ in range(count - 1))
        rows.append(cells)
    return rows


def prefix(text, available):
    result, used = "", 0
    for char in text:
        count = width(char)
        if used + count > available:
            break
        result += char
        used += count
    return result, used


def capture(terminal, evidence, name):
    terminal.wait_idle()
    ansi = terminal.text(ansi=True)
    text = SGR.sub("", ansi)
    require("\ufffd" not in text, f"{name}: a translated UTF-8 glyph was broken")
    (evidence / (name + ".ansi")).write_text(ansi, encoding="utf-8")
    (evidence / (name + ".txt")).write_text(text, encoding="utf-8")
    return text, styled_cells(ansi)


def expected_rows():
    result = json.loads((FIXTURES / "result.json").read_text(encoding="utf-8"))
    require(result["passed"] and result["case"] == "positive" and
            result["source_map"] == {"groups": 7, "rows": 89, "captions": 68},
            "requires the already-passed editor-only positive model, not a model rerun")
    translations = {}
    with (FIXTURES / "oracle.tsv").open(encoding="utf-8", newline="") as source:
        for locale, _, context, original, translated in csv.reader(source, delimiter="\t"):
            if locale == "editor-le":
                translations[(context, original)] = translated
    groups = [[] for _ in CONTEXTS]
    with SOURCE_ROWS.open(encoding="utf-8", newline="") as source:
        for category, row, label, action, mnemonic in csv.reader(source, delimiter="\t"):
            category, row, action = int(category), int(row), int(action)
            require(row == len(groups[category]), "frozen editor row order changed")
            original, gap, shortcut = label.partition("  ")
            suffix = gap + shortcut if gap else ""
            groups[category].append({
                "label": label, "action": action, "mnemonic": int(mnemonic),
                "original": original, "suffix": suffix,
                "translated": (translations[(CONTEXTS[category], original)]
                               if action != -2 else ""),
            })
    require(tuple(map(len, groups)) == (16, 20, 8, 22, 7, 7, 9),
            "frozen editor category sizes changed")
    return groups


def check_dropdown(terminal, category, rows, translated, evidence, name):
    _, screen = capture(terminal, evidence, name)
    popup_left = OFFSETS[category]
    require(screen[1][popup_left]["text"] in ("┌", "╔", "┏", "╭", "+"),
            f"{name}: editor dropdown top-left corner moved")
    right = next((column for column in range(popup_left + 1, len(screen[1]))
                  if screen[1][column]["text"] in ("┐", "╗", "┓", "╮", "+")), None)
    require(right is not None, f"{name}: editor dropdown right border is missing")
    left = popup_left + 1
    if not translated:
        require(right - popup_left + 1 == ENGLISH_WIDTHS[category],
                f"{name}: original English popup width changed")
    shortcuts, captions = 0, 0
    for index, row in enumerate(rows):
        if row["action"] == -2:
            continue
        cells = screen[index + 2][left:right]
        content = "".join(cell["text"] for cell in cells)
        caption = row["translated"] if translated else row["original"]
        if caption == row["original"]:
            require(content.rstrip() == " " + row["label"],
                    f"{name}/{index}: original menu_row_label spacing/shortcut changed")
        else:
            available = right - left - 1 - len(row["suffix"])
            visible, used = prefix(caption, available)
            require(visible and content.startswith(" " + visible),
                    f"{name}/{index}: contextual caption differs: {content!r}")
            require(not any(cell["underline"] for cell in cells[1:1 + used]),
                    f"{name}/{index}: translation invented a byte-based mnemonic")
            if row["suffix"]:
                require(content.endswith(row["suffix"]),
                        f"{name}/{index}: original shortcut padding/right edge changed")
        if row["suffix"]:
            require(content.rstrip().endswith(row["suffix"].strip()),
                    f"{name}/{index}: original shortcut is missing or clipped")
            shortcuts += 1
        captions += 1
    return {"category": category, "captions": captions, "shortcuts": shortcuts,
            "popup_width": right - popup_left + 1}


def exercise(binary, expected_hash, tmux, root, evidence, groups, result):
    files = root / "files"
    private_directory(files)
    document = files / FILENAME
    document.write_bytes(PAYLOAD)
    resources = {str(document): digest(document)}
    result["phases"] = {}
    for phase, locale, translated in (("translated", "editor-le", True), ("english", "C", False)):
        require(digest(binary) == expected_hash, "matching App/core binary changed before launch")
        profile = root / phase
        private_directory(profile)
        config = profile / "config/mcpp"
        (config / "mcedit").mkdir(parents=True)
        preferences, menu = config / "config", config / "mcedit/menu"
        preferences.write_bytes(PREFERENCES)
        menu.write_bytes(MENU)
        resources.update({str(path): digest(path) for path in (preferences, menu)})
        result["resources_before"] = resources
        command = [
            "-u", "LC_ALL", "-u", "MCPP_LOCALEDIR", "LC_CTYPE=C.UTF-8",
            "LC_MESSAGES=" + locale, "LANGUAGE=editor-le", "MCPP_LOCALEDIR=" + str(FIXTURES),
            str(binary), "-u", "--nokeymap", "--edit", str(document),
        ]
        observation = {"locale": locale, "dropdowns": [], "passed": False}
        result["phases"][phase] = observation
        with TerminalSession("/usr/bin/env", tmux, profile, command, width=120, height=34,
                             record_exit=True, record_stderr=True) as terminal:
            terminal.wait_editor(dirty=False)
            terminal.wait_for("literal café Żółć")
            text, _ = capture(terminal, evidence, phase + "-literal-document")
            body = "\n".join(text.splitlines()[1:-1])
            require(FILENAME in text and "Copy" in body and "New" in body,
                    f"{phase}: literal filename or editor content was translated")
            terminal.press("F9")
            terminal.wait_for(groups[0][0]["translated"] if translated else "Open file...")
            for category, rows in enumerate(groups):
                if category:
                    terminal.step("Right")
                observation["dropdowns"].append(check_dropdown(
                    terminal, category, rows, translated, evidence, f"{phase}-dropdown-{category}"))
            terminal.step("Right", "Right", "Down", "Down")
            terminal.press("Enter")
            terminal.wait_editor(dirty=False, flags="---O")
            capture(terminal, evidence, phase + "-separator-skip-enter-dispatch")
            terminal.step("F9", "Right")
            terminal.press("t")
            terminal.wait_editor(dirty=False, flags="----")
            capture(terminal, evidence, phase + "-original-mnemonic-dispatch")
            if translated:
                terminal.press("F11")
                terminal.wait_for("User menu")
                text, _ = capture(terminal, evidence, phase + "-literal-custom-menu")
                require(re.search(r"\ba\s+Move\b", text) and re.search(r"\bb\s+Copy\b", text),
                        "editor custom-menu titles were translated")
                terminal.press("Escape")
                terminal.wait_for("User menu", present=False)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
            stderr = terminal.stderr_file.read_bytes()
            (evidence / (phase + "-stderr.txt")).write_bytes(stderr)
            require(not stderr, f"{phase}: unexpected diagnostics: {stderr!r}")
        require(sorted(path.name for path in files.iterdir()) == [FILENAME] and
                document.read_bytes() == PAYLOAD, "editor UI changed file bytes/names or left a lock")
        observation.update({"passed": True, "separator_dispatch_unchanged": True,
                            "original_mnemonic_dispatch_unchanged": True, "exit_status": 0})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--sha256", required=True, help="parent's new matching App/core digest")
    parser.add_argument("--app-source-sha256", required=True, help="parent's matching App source digest")
    parser.add_argument("--tmux", default="/usr/bin/tmux")
    arguments = parser.parse_args()
    require(os.getuid() == os.geteuid() == 1001 and Path("/.dockerenv").is_file() and
            Path("/review/.operation-regressions-disposable").is_file(),
            "requires the approved UID-1001 disposable Docker container")
    require(all(re.fullmatch("[0-9a-f]{64}", value) for value in
                (arguments.sha256, arguments.app_source_sha256)), "requires exact parent-provided hashes")
    binary = arguments.binary.resolve(strict=True)
    require(binary.is_relative_to(SNAPSHOT / "build/local-parity") and binary.name == "mcpp" and
            binary.stat().st_uid == 1001 and os.access(binary, os.X_OK) and
            digest(binary) == arguments.sha256 and arguments.sha256 not in PRESERVED_SHA256 and
            not arguments.sha256.startswith("75f3"),
            "requires a new owned matching App/core/all-caption-header build, never a 70-caption binary")
    app_source = SNAPSHOT / "src/app.cpp"
    require(digest(app_source) == arguments.app_source_sha256 and
            "detail::builtin_editor_dropdown_captions" in
            app_source.read_text(encoding="utf-8").split(
                "ftxui::Element render_editor_menu() {", 1)[1].split(
                    "ftxui::Element render_editor_save_dialog()", 1)[0],
            "requires the parent's matching single editor dropdown render hook")
    require(all(digest(path) == expected for path, expected in PINNED.items()),
            "frozen editor catalog, row oracle, or translation module changed")
    groups = expected_rows()
    os.umask(0o077)
    root = Path("/review/cases") / ("editor-menu-ui-cd58-" + str(os.getpid()))
    evidence = Path("/review/evidence/session") / root.name
    private_directory(root)
    private_directory(evidence)
    os.chdir(root)
    result = {
        "case": "one-reserved-editor-dropdown-actual-ui", "binary": str(binary),
        "binary_sha256": arguments.sha256, "app_source_sha256": arguments.app_source_sha256,
        "root": str(root), "evidence": str(evidence), "model_rerun": False, "passed": False,
    }
    try:
        exercise(binary, arguments.sha256, arguments.tmux, root, evidence, groups, result)
        result["passed"] = True
    except BaseException as error:
        result["error"] = repr(error)
        raise
    finally:
        result["resources_after"] = {
            name: digest(Path(name)) if Path(name).is_file() else None
            for name in result.get("resources_before", {})
        }
        result["resources_unchanged"] = result["resources_after"] == result.get("resources_before", {})
        result["matching_build_unchanged"] = (
            digest(binary) == arguments.sha256 and
            digest(app_source) == arguments.app_source_sha256 and
            all(digest(path) == expected for path, expected in PINNED.items()))
        result["passed"] = (result["passed"] and result["resources_unchanged"] and
                            result["matching_build_unchanged"])
        encoded = json.dumps(result, indent=2, ensure_ascii=False) + "\n"
        (root / "result.json").write_text(encoded, encoding="utf-8")
        (evidence / "result.json").write_text(encoded, encoding="utf-8")
    require(result["passed"], "matching build or literal user resources changed")
    print("PASS reserved UI: all 68 editor dropdown captions, UTF-8, shortcuts, literal data and English")
    print(evidence / "result.json")


if __name__ == "__main__":
    main()
