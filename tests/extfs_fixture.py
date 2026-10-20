#!/usr/bin/python3 -B
"""Original tiny JSON container helper for native extfs protocol tests."""

import json
from pathlib import Path
import sys


def main():
    command, container, *arguments = sys.argv[1:]
    path = Path(container)
    document = json.loads(path.read_text())
    if document.get("log"):
        with Path(document["log"]).open("a") as output:
            output.write(json.dumps([command, container, *arguments]) + "\n")
    if command in document.get("unsupported", []):
        print("original fixture: unsupported " + command, file=sys.stderr)
        return 7
    nodes = document["nodes"]
    if command == "list":
        if "listing" in document:
            sys.stdout.write(document["listing"])
            return 0
        for name, node in nodes.items():
            kind = node["kind"]
            payload = node.get("data", "").encode()
            target = node.get("target", "")
            permission = node.get("mode", {"f": "-rw-r--r--", "d": "drwxr-xr-x", "l": "lrwxrwxrwx"}[kind])
            size = len(payload) if kind == "f" else len(target.encode()) if kind == "l" else 0
            suffix = " -> " + target if kind == "l" or node.get("hardlink") else ""
            if node.get("hardlink"):
                suffix = " -> " + node["hardlink"]
            date = node.get("date", "Jan 02 2024")
            print(f"{permission} {node.get('links', 1)} 1001 1001 {size} {date} {name}{suffix}")
        return 0
    if command in document.get("noop", []):
        return 0
    name = arguments[0]
    if command == "copyout":
        output = Path(arguments[1])
        if document.get("unsafe_copyout"):
            output.unlink(missing_ok=True)
            output.symlink_to(document["unsafe_copyout"])
        else:
            output.write_bytes(nodes[name]["data"].encode())
        return 0
    if command == "copyin":
        nodes[name] = {"kind": "f", "data": Path(arguments[1]).read_bytes().decode()}
        if document.get("during_copyin"):
            Path(document["during_copyin"]).write_text("external source change\n")
    elif command == "mkdir":
        if name in nodes:
            return 3
        nodes[name] = {"kind": "d"}
    elif command == "rm":
        if nodes[name]["kind"] == "d":
            return 4
        del nodes[name]
    elif command == "rmdir":
        if nodes[name]["kind"] != "d" or any(key.startswith(name + "/") for key in nodes):
            return 5
        del nodes[name]
    else:
        return 9
    path.write_text(json.dumps(document))
    return 0


if __name__ == "__main__":
    sys.exit(main())
