#!/usr/bin/env python3
"""One bounded native MC SFS interface control, using original fixture rules."""

import json
import os
from pathlib import Path

from terminal_session import TerminalSession


def main():
    base = Path("/review/cases/single-transform-native")
    if os.geteuid() != 1001 or not Path("/.dockerenv").is_file():
        raise RuntimeError("native SFS control requires the non-root disposable container")
    base.mkdir(exist_ok=True)
    if base.is_symlink() or base.stat().st_uid != os.geteuid():
        raise RuntimeError("native SFS fixture base must be owned and not a symlink")
    root = base / ("run-" + str(os.getpid()))
    root.mkdir(exist_ok=False)
    os.environ["MC_DATADIR"] = str(root)
    os.environ["LD_LIBRARY_PATH"] = "/review/control/lib"
    (root / "sfs.ini").write_text(
        "probe/1\t/bin/echo SOURCE=%1 PARAM=%2 > %3\n"
        "probe/1\t/bin/echo WRONG-DUPLICATE > %3\n"
        "literal/0\t/bin/echo SOURCE=%1 PARAM=%2 > %3\n"
        "parameter:2\t/bin/echo SOURCE=%1 PARAM=%2 > %3\n"
    )
    source = root / "source text"
    source.write_text("native single-file fixture\n")
    results = {}
    paths = {
        "local": str(source) + "/probe://tail",
        "literal": str(source) + "/literal://tail",
        "parameter": "/parameter://tail",
    }
    for name, path in paths.items():
        session = TerminalSession(
            "/review/control/mc", "/usr/bin/tmux", root / name,
            ["-u", "-v", path], record_exit=True, record_stderr=True,
        )
        try:
            session.wait_idle()
            results[name] = session.text()
            session.save("native-sfs")
            session.press("F10")
        finally:
            session.close()
    evidence = Path("/review/evidence/single-transform-native.json")
    evidence.write_text(json.dumps(results, indent=2))
    assert "SOURCE=" + str(source) + " PARAM=tail" in results["local"], results["local"]
    assert "WRONG-DUPLICATE" not in results["local"], results["local"]
    assert "Cannot open" in results["literal"], results["literal"]
    assert "Cannot open" in results["parameter"], results["parameter"]
    print("Native MC 4.8.30: /name://suffix, local /1, %1/%2/%3, first duplicate verified")


if __name__ == "__main__":
    main()
