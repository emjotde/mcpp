#!/usr/bin/env python3
"""Run one bounded model group and cross-check its original fixtures with gettext."""

import argparse
import gettext
import os
from pathlib import Path
import subprocess


CONTEXT = "delete-dialog"
SINGULAR = "Delete {count} selected item?"
PLURAL = "Delete {count} selected items?"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("case", choices=("context", "plural", "failure"))
    parser.add_argument("root", type=Path)
    arguments = parser.parse_args()
    require(os.getuid() == 1001 and os.geteuid() == 1001 and
            Path("/.dockerenv").is_file() and
            Path("/review/.operation-regressions-disposable").is_file(),
            "requires the approved UID-1001 disposable Docker container")
    root = arguments.root
    require(root.is_absolute() and root.parent.resolve() == Path("/review/cases") and
            not root.exists(), "model root must be fresh and directly under /review/cases")
    os.umask(0o077)
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    subprocess.run([str(arguments.binary.resolve(strict=True)), arguments.case, str(root)],
                   check=True, env=environment)
    expected = root / "oracle.tsv"
    checked = 0
    if expected.is_file():
        for line in expected.read_text(encoding="utf-8").splitlines():
            mode, locale, fallback, count, text = line.split("\t")
            with (root / locale / "LC_MESSAGES/mcpp.mo").open("rb") as source:
                catalog = gettext.GNUTranslations(source)
            if fallback != "-":
                with (root / fallback / "LC_MESSAGES/mcpp.mo").open("rb") as source:
                    catalog.add_fallback(gettext.GNUTranslations(source))
            if mode == "caption":
                actual = catalog.pgettext(CONTEXT, "Delete")
            else:
                require(mode == "count", "unexpected gettext oracle operation")
                actual = catalog.npgettext(CONTEXT, SINGULAR, PLURAL, int(count))
                actual = actual.replace("{count}", count)
            require(actual == text, f"Python gettext differs for {locale}/{mode}/{count}: {actual!r}")
            checked += 1
    print(f"{arguments.case}: Python gettext cross-checks: {checked}")


if __name__ == "__main__":
    main()
