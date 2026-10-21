#!/usr/bin/env python3
"""Run one new bounded menu-caption model group with std-Python gettext checks."""

import argparse
import ast
import csv
import gettext
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_rows(source, root):
    text = source.read_text(encoding="utf-8")
    block = text.split(">& pulldown_items() {", 1)[1].split("return items;", 1)[0]
    groups = re.findall(r"^            \{\n(.*?)^            \},", block, re.MULTILINE | re.DOTALL)
    require(len(groups) == 5, "manager source must retain its five literal groups")
    row_pattern = re.compile(
        r'\{\s*("(?:[^"\\]|\\.)*")\s*,\s*(-?\d+)'
        r"(?:\s*,\s*('(?:[^'\\]|\\.)*'))?\s*\}")
    actual = []
    for category, group in enumerate(groups):
        for index, match in enumerate(row_pattern.finditer(group)):
            label, action, mnemonic = match.groups()
            actual.append([str(category), str(index), ast.literal_eval(label), action,
                           str(ord(ast.literal_eval(mnemonic))) if mnemonic else "0"])
    with (root / "source-manager.tsv").open(encoding="utf-8", newline="") as data:
        expected = list(csv.reader(data, delimiter="\t"))
    require(actual == expected,
            "manager English rows, category/row indices, actions, mnemonics or shortcut spacing changed")
    manager_count = len(actual)
    names = re.search(
        r"const std::array<std::string, 7> editor_menu_names\{\s*(.*?)\s*\};",
        text, re.DOTALL)
    keys = re.search(
        r"constexpr std::array<std::size_t, 7> editor_menu_mnemonics\{(.*?)\};", text)
    require(names and keys, "editor English source arrays must remain literal and unchanged")
    captions = [ast.literal_eval(value) for value in re.findall(r'"(?:[^"\\]|\\.)*"', names[1])]
    mnemonics = [value.strip() for value in keys[1].split(",")]
    require(len(captions) == len(mnemonics) == 7, "editor source must retain exactly seven categories")
    actual = [[str(index), caption, mnemonic]
              for index, (caption, mnemonic) in enumerate(zip(captions, mnemonics))]
    with (root / "source-editor.tsv").open(encoding="utf-8", newline="") as data:
        expected = list(csv.reader(data, delimiter="\t"))
    require(actual == expected, "editor category order, English captions, spacing or mnemonics changed")
    return {"manager_rows": manager_count, "editor_captions": len(captions)}


def gettext_checks(root):
    checked = 0
    catalogs = {}
    with (root / "oracle.tsv").open(encoding="utf-8", newline="") as source:
        for locale, fallback, context, original, expected in csv.reader(source, delimiter="\t"):
            pair = (locale, fallback)
            if pair not in catalogs:
                with (root / locale / "LC_MESSAGES/mcpp.mo").open("rb") as data:
                    chain = [gettext.GNUTranslations(data)]
                if fallback != "-":
                    with (root / fallback / "LC_MESSAGES/mcpp.mo").open("rb") as data:
                        chain.append(gettext.GNUTranslations(data))
                catalogs[pair] = chain
            # Our documented empty-entry fallback differs from GNU's empty-string result.
            key = context + "\x04" + original
            actual = original
            for catalog in catalogs[pair]:
                value = catalog.gettext(key)
                if value and value != key:
                    actual = catalog.pgettext(context, original)
                    break
            require(actual == expected, f"std-Python gettext differs for {context}/{original}: {actual!r}")
            checked += 1
    return checked


def check_fixture(root, result):
    binary, source = Path(result["binary"]), Path(result["app_source"])
    if result["case"] == "positive":
        result["source_map"] = source_rows(source, root)
    result["gettext_checks"] = gettext_checks(root)
    result["catalog_sha256"] = {
        str(path.relative_to(root)): digest(path) for path in sorted(root.glob("*/LC_MESSAGES/mcpp.mo"))
    }
    result["passed"] = True
    require(digest(binary) == result["binary_sha256"] and
            digest(source) == result["app_source_sha256"], "model executable or frozen App source changed")
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{result['case']}: std-Python gettext checks: {result['gettext_checks']}; passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("case", choices=("positive", "fallback", "failure"))
    parser.add_argument("root", type=Path)
    parser.add_argument("--app-source", type=Path,
                        default=Path(__file__).resolve().parents[1] / "src/app.cpp")
    arguments = parser.parse_args()
    require(os.getuid() == 1001 and os.geteuid() == 1001 and
            Path("/.dockerenv").is_file() and
            Path("/review/.operation-regressions-disposable").is_file(),
            "requires the approved UID-1001 disposable Docker container")
    root = arguments.root
    require(root.is_absolute() and root.parent.resolve() == root.parent and
            root.parent.is_relative_to("/review/cases") and not root.exists(),
            "new model root must be fresh, below /review/cases, without symlinked parents")
    parent = root.parent.stat()
    require(parent.st_uid == 1001 and stat.S_ISDIR(parent.st_mode) and not parent.st_mode & 0o077,
            "new model parent must be an owned private directory")
    binary = arguments.binary.resolve(strict=True)
    require(binary.is_relative_to("/home/mcpp/src/build/local-parity") and
            binary.is_file() and binary.stat().st_uid == 1001 and os.access(binary, os.X_OK),
            "requires the isolated owned new-model executable")
    source = arguments.app_source.resolve(strict=True)
    require(source.is_relative_to("/home/mcpp/src") and source.is_file(),
            "App source oracle must come from the approved source snapshot")
    os.umask(0o077)
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    result = {
        "case": arguments.case, "binary": str(binary), "binary_sha256": digest(binary),
        "app_source": str(source), "app_source_sha256": digest(source), "root": str(root),
    }
    subprocess.run([str(binary), arguments.case, str(root)], check=True, env=environment)
    check_fixture(root, result)


if __name__ == "__main__":
    main()
