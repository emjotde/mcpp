#!/usr/bin/env python3
"""One cached native/mcpp comparison for repetition-only syntax keywords."""

import argparse
import hashlib
import json
import os
from pathlib import Path

from terminal_session import TerminalSession
from test_syntax_format_ui import colors_for


SYNTAX = r"""file . Original\sConsuming\sPatterns
context default lightgray black
 spellcheck
 keyword whole sentinel yellow
 keyword \[ab\] brightgreen
 keyword * brightred
"""
TEXT = "sentinel\nabba! aba\n"
FIXTURE_SHA256 = hashlib.sha256((SYNTAX + TEXT).encode()).hexdigest()
BASE = Path("/review/cases/syntax-consumption-3d1ea")
NATIVE_BINARY = Path("/review/control/mc")


def main():
    if (os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file()):
        raise SystemExit("requires uid1001 in the owned disposable container")
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--native", action="store_true")
    mode.add_argument("--mcpp", type=Path)
    parser.add_argument("--sha256")
    arguments = parser.parse_args()
    native = arguments.native
    if not native and not arguments.sha256:
        parser.error("--mcpp requires --sha256; it never reruns the native control")
    binary = NATIVE_BINARY if native else arguments.mcpp
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    if arguments.sha256 and digest != arguments.sha256:
        raise SystemExit(f"binary hash differs: {digest}")
    BASE.mkdir(exist_ok=True)
    if BASE.is_symlink() or BASE.stat().st_uid != os.geteuid():
        raise SystemExit("fixture base must be owned and not a symlink")
    cache = BASE / "native.json"
    cached = json.loads(cache.read_text()) if cache.is_file() else None
    if cached:
        assert cached["fixture_sha256"] == FIXTURE_SHA256, "native fixture cache differs"
        assert cached["sha256"] == hashlib.sha256(NATIVE_BINARY.read_bytes()).hexdigest(), (
            "cached native binary differs"
        )
    if native and cached:
        print(f"Using cached native control: {cache}")
        return
    if not native and not cached:
        parser.error("run --native once to cache the native control before --mcpp")
    root = BASE / (("native-" if native else "mcpp-") + str(os.getpid()))
    root.mkdir()
    fixture = root / "original.consume"
    fixture.write_text(TEXT)
    ui = root / "ui"
    config = ui / "config" / ("mc" if native else "mcpp")
    config.mkdir(parents=True)
    if native:
        definitions = ui / "data/mc/syntax"
        definitions.mkdir(parents=True)
        (config / "ini").write_text(
            "[Midnight-Commander]\nuse_subshell=0\nconfirm_exit=0\n"
            "editor_syntax_highlighting=true\n"
        )
    else:
        definitions = config
        (config / "config").write_text(
            "auto_save_setup=false\neditor_line_numbers=false\neditor_syntax=true\n"
        )
    (definitions / "Syntax").write_text(SYNTAX)
    os.environ["LD_LIBRARY_PATH"] = "/review/control/lib"
    command = ["-u", "-e", fixture] if native else ["--edit", fixture]
    with TerminalSession(binary, "/usr/bin/tmux", ui, command, record_exit=True) as terminal:
        terminal.wait_for("sentinel")
        terminal.press("C-End")
        terminal.wait_idle()
        terminal.save("consuming-patterns")
        ansi = terminal.text(ansi=True)
        colors = {line: colors_for(ansi, line) for line in TEXT.splitlines()}
        result = {
            "binary": str(binary), "sha256": digest, "fixture_sha256": FIXTURE_SHA256,
            "colors": colors, "root": str(root),
        }
        (root / "colors.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result))
        assert fixture.read_text() == TEXT, "syntax comparison changed fixture bytes"
        if not native:
            assert colors == cached["colors"], (
                f"mcpp differs from cached native colors: {colors!r}"
            )
        terminal.press("F10")
        terminal.wait_file(terminal.exit_file, b"0\n")
    assert fixture.read_text() == TEXT, "syntax comparison changed fixture bytes"
    assert colors["sentinel"] == [11] * len("sentinel"), "syntax registry failed to load"
    if native:
        cache.write_text(json.dumps(result, indent=2) + "\n")
        print(f"Cached native control: {cache}")
    else:
        print("PASS mcpp consumes repetition-only keywords exactly like the cached native control")
    print(root)


if __name__ == "__main__":
    main()
