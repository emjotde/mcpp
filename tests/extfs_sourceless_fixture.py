#!/usr/bin/python3 -B
"""Original read-only source-less extfs fixture; no package or network commands."""

import json
import os
from pathlib import Path
import sys


def main():
    settings = json.loads(Path(str(Path(__file__).resolve()) + ".json").read_text())
    arguments = sys.argv[1:]
    with Path(settings["log"]).open("a") as output:
        output.write(json.dumps({"argv": arguments, "cwd": os.getcwd()}) + "\n")
    if not arguments:
        return 7
    command = arguments[0]
    if command in settings.get("unsupported", []):
        print("original source-less fixture: unsupported " + command, file=sys.stderr)
        return 7
    def data(node):
        return (Path(node["from_working"]).read_bytes() if "from_working" in node
                else node.get("data", "").encode())

    if command == "list" and len(arguments) == 1:
        if "listing" in settings:
            sys.stdout.write(settings["listing"])
            return 0
        for name, node in settings["nodes"].items():
            kind = node["kind"]
            target = node.get("target", "")
            payload = data(node)
            permission = {"f": "-r--r--r--", "d": "dr-xr-xr-x", "l": "lrwxrwxrwx"}[kind]
            size = len(payload) if kind == "f" else len(target.encode()) if kind == "l" else 0
            suffix = " -> " + target if kind == "l" else ""
            print(f"{permission} 1 1001 1001 {size} Jan 02 2024 {name}{suffix}")
        return 0
    if command == "copyout" and len(arguments) == 4:
        directory, name, output = arguments[1:]
        if not Path(directory).is_dir():
            print("original source-less fixture: copyout origin is not a directory", file=sys.stderr)
            return 8
        destination = Path(output)
        if settings.get("unsafe_copyout"):
            destination.unlink(missing_ok=True)
            destination.symlink_to(settings["unsafe_copyout"])
        else:
            destination.write_bytes(data(settings["nodes"][name]))
        return 0
    print("original source-less fixture: read-only command", file=sys.stderr)
    return 7


if __name__ == "__main__":
    sys.exit(main())
