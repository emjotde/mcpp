#!/usr/bin/env python3
"""One bounded editor-dropdown model group, checked with std-Python gettext."""

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
    block = text.split(">& editor_menu_items() {", 1)[1].split("return menus;", 1)[0]
    groups = re.findall(r"^            \{\n(.*?)^            \},", block,
                        re.MULTILINE | re.DOTALL)
    require(len(groups) == 7, "editor source must retain exactly seven literal dropdown groups")
    pattern = re.compile(
        r'\{\s*("(?:[^"\\]|\\.)*")\s*,\s*(-?\d+)'
        r"(?:\s*,\s*('(?:[^'\\]|\\.)*'))?\s*\}")
    actual = []
    for category, group in enumerate(groups):
        for row, match in enumerate(pattern.finditer(group)):
            label, action, mnemonic = match.groups()
            actual.append([str(category), str(row), ast.literal_eval(label), action,
                           str(ord(ast.literal_eval(mnemonic))) if mnemonic else "0"])
    with (root / "source-editor-rows.tsv").open(encoding="utf-8", newline="") as data:
        expected = list(csv.reader(data, delimiter="\t"))
    require(actual == expected,
            "editor row indices, English labels, actions, mnemonics or shortcut padding changed")
    require(len(actual) == 89 and sum(row[3] != "-2" for row in actual) == 68,
            "editor dropdown row/caption counts differ")
    return {"groups": 7, "rows": len(actual), "captions": 68}


def gettext_checks(root):
    checked, catalogs = 0, {}
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
            # Empty entries deliberately try the next language instead of displaying nothing.
            key = context + "\x04" + original
            actual = original
            for catalog in catalogs[pair]:
                value = catalog.gettext(key)
                if value and value != key:
                    actual = catalog.pgettext(context, original)
                    break
            require(actual == expected,
                    f"std-Python gettext differs for {context}/{original}: {actual!r}")
            checked += 1
    return checked


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
            "new model root must be fresh and below /review/cases without symlinked parents")
    parent = root.parent.stat()
    require(parent.st_uid == 1001 and stat.S_ISDIR(parent.st_mode) and not parent.st_mode & 0o077,
            "new model parent must be an owned private directory")
    binary = arguments.binary.resolve(strict=True)
    require(binary.is_relative_to("/home/mcpp/src/build/local-parity") and
            binary.is_file() and binary.stat().st_uid == 1001 and os.access(binary, os.X_OK),
            "requires the isolated owned editor-caption model executable")
    source = arguments.app_source.resolve(strict=True)
    require(source.is_relative_to("/home/mcpp/src") and source.is_file(),
            "App source oracle must come from the approved source snapshot")
    snapshot = Path("/home/mcpp/src")
    sources = [source, snapshot / "include/mcpp/translation.hpp",
               snapshot / "src/translation.cpp", snapshot / "src/builtin_menu_captions.hpp"]
    os.umask(0o077)
    result = {
        "case": arguments.case, "binary": str(binary), "binary_sha256": digest(binary),
        "app_source": str(source), "source_sha256": {str(path): digest(path) for path in sources},
        "root": str(root),
    }
    subprocess.run([str(binary), arguments.case, str(root)], check=True,
                   env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
    if arguments.case == "positive":
        result["source_map"] = source_rows(source, root)
    result["gettext_checks"] = gettext_checks(root)
    result["catalog_sha256"] = {
        str(path.relative_to(root)): digest(path) for path in sorted(root.glob("*/LC_MESSAGES/mcpp.mo"))
    }
    require(digest(binary) == result["binary_sha256"] and
            {str(path): digest(path) for path in sources} == result["source_sha256"],
            "model executable or frozen translation/App sources changed")
    result["passed"] = True
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{arguments.case}: std-Python gettext checks: {result['gettext_checks']}; passed")


if __name__ == "__main__":
    main()
