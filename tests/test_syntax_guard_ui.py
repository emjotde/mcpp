#!/usr/bin/env python3
"""Original, bounded controls for empty keywords and wildcard-edge contexts."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re

from terminal_session import SGR, TerminalSession
from test_syntax_format_ui import colors_for


BASE = Path("/review/cases/l22-guards-a639")
EVIDENCE = Path("/review/evidence/session")
NATIVE_EVIDENCE = EVIDENCE / "l22-guards-a639-native.json"
PRE_GUARD_MCPP_SHA256 = {
    "03687bc3f3b9eca7209546264abdfa7f7bbc9e703783fbc54a2556316ec466f8",
    "e886331864b03066cf2eac1343b2835c7ec8b92a264bfab4982378bb3aa12b5c",
    "6adce2cccfe562dc441dc763ce8c233b552c52908d7d602c0537c2a29d214847",
}
FIXTURES = [
    {
        "name": "empty-keyword",
        "syntax": r"""file . Original\sEmpty\sKeyword
context default lightgray black
 spellcheck
 keyword whole sentinel yellow
 keyword \[\] brightmagenta
context < > brightgreen black
""",
        "text": "za <tag> sentinel\néa <tag> sentinel\nsentinel <tag>\n",
    },
    {
        "name": "empty-options",
        "syntax": r"""file . Original\sEmpty\sOptions
context default lightgray black
 keyword whole sentinel yellow
 keyword whole \[\] brightmagenta
 keyword linestart \[\] brightcyan
context < > brightgreen black
 spellcheck
""",
        "text": "za  <tag> sentinel\nsentinel  <tag>\nz <tag>\n!\n",
    },
    {
        "name": "edge-bytes",
        "syntax": r"""file . Original\sWildcard\sBytes
context default lightgray black
 keyword whole sentinel yellow
context *A B brightmagenta black
 spellcheck
context C* D brightgreen black
 spellcheck
context E *F brightcyan black
 spellcheck
context G H* brightred black
 spellcheck
context < > yellow black
""",
        "text": (
            "sentinel\nq AoneB \x01AtwoB tail\nCwordD <ok> tail\nCraw\x00 Dtail\n"
            "EinsideF \x01finishF tail\nGinsideH tail\nbefore Hraw\x00 after\noutside\n"
        ),
    },
    {
        "name": "edge-whole",
        "syntax": r"""file . Original\sWildcard\sBoundaries
context default lightgray black
 keyword whole sentinel yellow
context wholeright I* J brightmagenta black
 spellcheck
context wholeright K L* brightcyan black
 spellcheck
""",
        "text": (
            "sentinel\nIword J  outside\nIword  J  after\nK body L outside\n"
            "K body L  after\noutside\nIwrap\n after\nJ  outside\n"
        ),
    },
]


def fragments(text):
    return [
        piece for line in text.split("\n")
        for piece in re.split(r"[\x00-\x1f]", line) if piece
    ]


def capture_colors(ansi, text):
    rows = ansi.splitlines()
    first = fragments(text.split("\n")[0])[0]
    start = next(index for index, row in enumerate(rows) if first in SGR.sub("", row))
    return [
        [{"text": piece, "colors": colors_for(rows[start + index], piece)}
         for piece in fragments(line)]
        for index, line in enumerate(text.split("\n")[:-1])
    ]


def fixture_hash(case):
    return hashlib.sha256((case["syntax"] + case["text"]).encode()).hexdigest()


def owned_directory(path):
    path.mkdir(exist_ok=True)
    if path.is_symlink() or path.stat().st_uid != os.geteuid():
        raise SystemExit(f"requires an owned directory, not a symlink: {path}")


def main():
    if (os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file()):
        raise SystemExit("requires uid1001 in the owned disposable container")
    parser = argparse.ArgumentParser()
    parser.add_argument("--mcpp", type=Path)
    parser.add_argument("--sha256")
    parser.add_argument("--syntax-source-sha256")
    arguments = parser.parse_args()
    native = arguments.mcpp is None
    if not native and (not arguments.sha256 or not arguments.syntax_source_sha256):
        parser.error("--mcpp requires --sha256 and --syntax-source-sha256")
    binary = Path("/review/control/mc") if native else arguments.mcpp.resolve()
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    if arguments.sha256 and digest != arguments.sha256:
        raise SystemExit(f"binary hash differs: {digest}")
    source_digest = None
    expected = None
    hashes = [fixture_hash(case) for case in FIXTURES]
    if native:
        if NATIVE_EVIDENCE.exists():
            cached = json.loads(NATIVE_EVIDENCE.read_text())
            assert (cached["sha256"] == digest and cached["fixture_hashes"] == hashes and
                    cached["capture_format"] == 2)
            if cached["status"] in ("CAPTURED", "FROZEN"):
                print(f"Using cached original native controls: {NATIVE_EVIDENCE}")
                return
            raise SystemExit("native evidence is incomplete; do not silently rerun probes")
        result_path = NATIVE_EVIDENCE
    else:
        expected = json.loads(NATIVE_EVIDENCE.read_text())
        assert (expected["status"] == "FROZEN" and expected["fixture_hashes"] == hashes and
                expected["capture_format"] == 2)
        if digest == expected["sha256"] or digest in PRE_GUARD_MCPP_SHA256:
            raise SystemExit("matching UI validation requires a new mcpp guard build")
        source_digest = hashlib.sha256(
            Path("/home/mcpp/src/src/syntax_rules.cpp").read_bytes()
        ).hexdigest()
        if source_digest != arguments.syntax_source_sha256:
            raise SystemExit("syntax source differs from the matching-build checkpoint")
        result_path = EVIDENCE / f"l22-guards-a639-ui-{digest[:16]}.json"
        if result_path.exists():
            cached = json.loads(result_path.read_text())
            assert cached["sha256"] == digest and cached["fixture_hashes"] == hashes
            if cached["status"] == "PASS":
                print(f"Using cached matching-app guard comparison: {result_path}")
                return
            raise SystemExit("matching-app evidence is incomplete; inspect before retrying")
    owned_directory(BASE)
    run = BASE / ("native" if native else "ui-" + digest[:16])
    owned_directory(run)
    result = {
        "status": "running", "binary": str(binary), "sha256": digest,
        "syntax_source_sha256": source_digest, "fixture_hashes": hashes, "cases": [],
        "capture_format": 2,
        "comparison": "printable fragments; nonprinting display glyphs are not syntax evidence",
    }
    result_path.write_text(json.dumps(result, indent=2) + "\n")
    os.environ["LD_LIBRARY_PATH"] = "/review/control/lib"
    for index, case in enumerate(FIXTURES):
        root = run / (case["name"] + "-" + str(os.getpid()))
        root.mkdir()
        fixture = root / "original.guard"
        fixture.write_bytes(case["text"].encode())
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
        (definitions / "Syntax").write_text(case["syntax"])
        command = [
            "--foreground", "--signal=TERM", "--kill-after=1", "18", str(binary),
            *(["-u", "-e"] if native else ["--edit"]), str(fixture),
        ]
        with TerminalSession("/usr/bin/timeout", "/usr/bin/tmux", ui, command,
                             record_exit=True, record_stderr=True) as terminal:
            terminal.wait_for("sentinel", timeout=4)
            assert "Error" not in terminal.text(), "syntax loader displayed an error"
            terminal.press("C-End")
            terminal.wait_idle()
            terminal.save("guard-end")
            colors = capture_colors(terminal.text(ansi=True), case["text"])
            if not native:
                assert colors == expected["cases"][index]["colors"], (
                    f"{case['name']} differs from frozen native colors: {colors!r}"
                )
            terminal.press("C-Home")
            terminal.wait_idle()
            terminal.save("guard-home")
            terminal.press("C-End")
            terminal.wait_idle()
            terminal.save("guard-rewind")
            assert capture_colors(terminal.text(ansi=True), case["text"]) == colors, (
                "rewinding changes guard colors"
            )
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
        assert fixture.read_bytes() == case["text"].encode(), "guard control changed fixture bytes"
        result["cases"].append({
            **case, "root": str(root), "fixture_sha256": hashes[index],
            "accepted": True, "exit_status": 0, "bytes_unchanged": True,
            "rewind_stable": True, "colors": colors,
        })
        result_path.write_text(json.dumps(result, indent=2) + "\n")
    result["status"] = "CAPTURED" if native else "PASS"
    result_path.write_text(json.dumps(result, indent=2) + "\n")
    print(f"{result['status']} original guard group: {result_path}")


if __name__ == "__main__":
    main()
