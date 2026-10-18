from pathlib import Path
from contextlib import contextmanager
import json
import sys
import tempfile
import traceback

from terminal_session import ApplicationExitError, TerminalSession


MODES = ["Normal", "Regular expression", "Wildcard", "Hexadecimal"]
FLAGS = ["Case sensitive", "Backwards", "In selection", "Whole words"]
MENUS = ["File", "Edit", "Search", "Command", "Format", "Window", "Options"]


def boxes(rows):
    for top, row in enumerate(rows):
        left = row["text"].find("┌")
        right = row["text"].find("┐", left + 1)
        if left < 0 or right < 0:
            continue
        for bottom in range(top + 1, len(rows)):
            text = rows[bottom]["text"]
            if len(text) <= right or text[left] != "└" or text[right] != "┘":
                continue
            contents = []
            for inner in rows[top + 1:bottom]:
                text = inner["text"]
                if len(text) <= right or text[left] != "│" or text[right] != "│":
                    continue
                focused = "".join(
                    text[max(start, left + 1):min(end, right)]
                    for start, end in inner["spans"] if start < right and end > left + 1
                ).strip()
                contents.append({"text": text[left + 1:right].strip(), "focused": focused})
            yield {"left": left, "right": right, "top": top, "bottom": bottom, "rows": contents}
            break


def menu_bar(rows):
    return bool(rows) and all(name in rows[0]["text"] for name in MENUS)


def menu_state(rows):
    if not menu_bar(rows):
        return None
    for box in boxes(rows):
        if box["top"] != 1:
            continue
        category = next(
            (name for name in MENUS if rows[0]["text"].find(name) == box["left"]), None
        )
        selected = [row["text"] for row in box["rows"]
                    if row["focused"] and row["focused"] == row["text"]]
        if category is not None and len(selected) == 1:
            return {
                "category": category, "focus": selected[0],
                "items": [row["text"] for row in box["rows"]],
            }
    return None


def search_state(rows):
    for box in boxes(rows):
        if not any(row["text"] == "Search for:" for row in box["rows"]):
            continue
        values, focused = {}, []
        for row in box["rows"]:
            text = row["text"]
            field = None
            if text.startswith(">"):
                values["needle"] = text[1:].removeprefix(" ")
                field = "query"
            elif text.startswith("Mode: "):
                mode = next((name for name in MODES if text.startswith(f"Mode: {name} ")), None)
                if mode is not None:
                    values["mode"] = mode
                    field = "mode"
            else:
                for flag in FLAGS:
                    if text in (f"[ ] {flag}", f"[x] {flag}"):
                        values[flag] = text.startswith("[x]")
                        field = flag
                        break
            if row["focused"]:
                if field is not None and row["focused"] == text:
                    focused.append(field)
                else:
                    focused.extend(button for button in ("OK", "Find all", "Cancel")
                                   if f"[ {button} ]" in row["focused"])
        if len(values) == 6 and len(focused) == 1:
            return {"values": values, "focus": focused[0]}
    return None


def search_visible(rows):
    return any(
        any(row["text"] == "Search for:" for row in box["rows"])
        for box in boxes(rows)
    )


def observe(terminal, reader, matches, description):
    latest = None

    def ready():
        nonlocal latest
        latest = reader(terminal.styled_rows())
        return latest is not None and matches(latest)

    try:
        terminal.wait(ready, description)
    except ApplicationExitError:
        raise
    except AssertionError as error:
        raise AssertionError(f"{description}; last observed state: {latest!r}") from error
    with (terminal.root / "observations.jsonl").open("a") as output:
        output.write(json.dumps({"expected": description, "observed": latest}) + "\n")
    return latest


def wait_search(terminal, values=None, focus=None):
    return observe(
        terminal, search_state,
        lambda state: (values is None or state["values"] == values) and
                      (focus is None or state["focus"] == focus),
        f"Search dialog values={values!r}, focus={focus!r}",
    )


def wait_search_closed(terminal):
    observe(terminal, lambda rows: {"open": search_visible(rows)},
            lambda state: not state["open"], "Search dialog closes")


def wait_title(terminal, title):
    return observe(
        terminal, lambda rows: next(
            (box for box in boxes(rows) if box["rows"] and box["rows"][0]["text"] == title), None
        ),
        lambda box: True, f"Dialog title {title!r}",
    )


def value_prompt_state(rows):
    for box in boxes(rows):
        if not any(row["text"] == "Value:" for row in box["rows"]):
            continue
        for row in box["rows"]:
            if row["text"].startswith(">"):
                return {
                    "value": row["text"][1:].removeprefix(" "),
                    "width": box["right"] - box["left"] - 3,
                }
    return None


@contextmanager
def editor_session(binary, tmux, root, path):
    with TerminalSession(binary, tmux, root / "ui", ["--edit", path],
                         record_exit=True) as terminal:
        terminal.record_output()
        yield terminal
        terminal.wait_file(terminal.exit_file, b"0\n")


def capture(terminal, name, files):
    terminal.save(name)
    try:
        state = terminal.editor_state()
    except (AssertionError, IndexError):
        state = None
    (terminal.root / f"{name}.json").write_text(json.dumps({
        "editor": state,
        "files": {str(path): path.read_bytes().hex() if path.exists() else None for path in files},
    }, indent=2))


def menu(terminal, category, item):
    terminal.press("F9")
    observe(terminal, lambda rows: {"open": menu_bar(rows)},
            lambda state: state["open"], "Editor menu bar opens")
    terminal.press(*(["Right"] * category), "Enter")
    state = observe(terminal, menu_state,
                    lambda state: state["category"] == MENUS[category],
                    f"{MENUS[category]} popup opens with an actual focused item")
    for _ in range(30):
        if state["focus"].startswith(item):
            terminal.press("Enter")
            observe(terminal, lambda rows: {"open": menu_bar(rows)},
                    lambda state: not state["open"], f"Activate menu item {item!r}")
            return
        previous = state["focus"]
        terminal.press("Down")
        state = observe(
            terminal, menu_state,
            lambda state: state["category"] == MENUS[category] and state["focus"] != previous,
            f"Menu Down changes focus from {previous!r}",
        )
    raise AssertionError(f"Menu item not focused: {item}\n{terminal.text()}")


def dialog(terminal):
    return wait_search(terminal)["values"]


def search(terminal, needle, mode=0, sensitive=False, backwards=False,
           in_selection=False, whole=False):
    terminal.press("F7")
    previous = wait_search(terminal, focus="query")["values"]
    terminal.press(*(["BSpace"] * len(previous["needle"])))
    terminal.type(needle)
    typed = dict(previous, needle=needle)
    wait_search(terminal, values=typed, focus="query")
    keys = ["Tab", *(["Right"] * ((mode - MODES.index(previous["mode"])) % len(MODES)))]
    for name, wanted in zip(FLAGS, (sensitive, backwards, in_selection, whole)):
        keys.append("Tab")
        if previous[name] != wanted:
            keys.append("Space")
    keys.append("Tab")
    terminal.press(*keys)
    expected = {
        "needle": needle, "mode": MODES[mode],
        **dict(zip(FLAGS, (sensitive, backwards, in_selection, whole))),
    }
    wait_search(terminal, values=expected, focus="OK")
    terminal.press("Enter")
    wait_search_closed(terminal)
    assert "Search string not found" not in terminal.text(), terminal.text()


def expect_search(terminal, needle, mode=0, sensitive=False, in_selection=False, whole=False):
    terminal.press("F7")
    expected = {
        "needle": needle, "mode": MODES[mode], "Case sensitive": sensitive,
        "Backwards": False, "In selection": in_selection, "Whole words": whole,
    }
    wait_search(terminal, values=expected, focus="query")
    terminal.press("Escape")
    wait_search_closed(terminal)


def prompt(terminal, value):
    observe(terminal, value_prompt_state, lambda state: True, "Value prompt opens")
    terminal.press("C-u")
    observe(terminal, value_prompt_state, lambda state: state["value"] == "",
            "Ctrl-U clears the Value input")
    value = str(value)
    terminal.type(value)
    observe(terminal, value_prompt_state,
            lambda state: bool(state["value"]) and value.endswith(state["value"]),
            "Value input displays the typed path at the caret")
    terminal.press("Enter")
    terminal.wait_for("Value:", present=False)


def open_file(terminal, path):
    menu(terminal, 0, "Open file")
    prompt(terminal, path)
    terminal.wait_editor(row=1, column=1, selection=0, dirty=False)
    terminal.wait_for(path.read_text().splitlines()[0])


def save(terminal, path, expected):
    terminal.save_editor()
    terminal.wait_editor(dirty=False)
    terminal.wait_file(path, expected)


def close_file(terminal, discard=False):
    menu(terminal, 0, "Close")
    if discard:
        wait_title(terminal, "Close file")
        terminal.wait_for("Save before close?")
        terminal.type("n")
        terminal.wait_for("Save before close?", present=False)


def repeat_search(terminal):
    terminal.raw(b"\x1b[31~")
    terminal.wait_idle()


def fixture(root, name, data):
    path = root / name
    path.write_bytes(data)
    return path


def switch_list_workflow(binary, tmux, root):
    first = b"alpha A\nrow2 alpha A\nrow3\n"
    second = b"beta B\nother beta B\nend\n"
    a = fixture(root, "a.txt", first)
    b = fixture(root, "b.txt", second)
    with editor_session(binary, tmux, root, a) as terminal:
        terminal.wait_editor(row=1, column=1)
        terminal.step("End")
        terminal.type("!")
        terminal.step("Home")
        search(terminal, "alpha")
        state_a = terminal.wait_editor(row=1, column=1, selection=5, dirty=True)
        open_file(terminal, b)
        terminal.step("Down", "End")
        terminal.type("?")
        terminal.step("Home")
        search(terminal, "beta", sensitive=True, whole=True)
        state_b = terminal.wait_editor(row=2, column=7, selection=4, dirty=True)
        capture(terminal, "b-dirty", [a, b])
        menu(terminal, 5, "Next buffer")
        terminal.wait_editor(**state_a)
        repeat_search(terminal)
        capture(terminal, "a-repeat", [a, b])
        assert "Search string not found" not in terminal.text(), (
            "Returning to A must repeat A's alpha search, not B's beta search"
        )
        terminal.wait_editor(row=2, column=6, selection=5)
        terminal.step("C-u")
        save(terminal, a, first)
        menu(terminal, 5, "Next buffer")
        terminal.wait_editor(**state_b)
        terminal.step("C-u")
        save(terminal, b, second)
        terminal.raw(b"\x1br")
        terminal.wait_idle()
        changed_b = b"beta B\nother beta B?\nend\n"
        save(terminal, b, changed_b)
        expect_search(terminal, "beta", sensitive=True, whole=True)
        menu(terminal, 5, "Buffer list")
        wait_title(terminal, "Buffer list")
        terminal.step("End", "Enter")
        expect_search(terminal, "alpha")
        menu(terminal, 5, "Previous buffer")
        expect_search(terminal, "beta", sensitive=True, whole=True)
        close_file(terminal)
        expect_search(terminal, "alpha")
        terminal.wait_file(a, first)
        terminal.wait_file(b, changed_b)
        capture(terminal, "independent-files-and-close", [a, b])
        close_file(terminal)


def scoped_close_workflow(binary, tmux, root):
    first = b"alpha alpha\noutside alpha\n"
    second = b"other\nbeta2\nend\n"
    a = fixture(root, "a.txt", first)
    b = fixture(root, "b.txt", second)
    with editor_session(binary, tmux, root, a) as terminal:
        terminal.wait_editor(row=1, column=1)
        terminal.step("End")
        terminal.type("!")
        terminal.step("Home", "F3", "End", "F3", "Home")
        search(terminal, "alpha", in_selection=True)
        state_a = terminal.wait_editor(row=1, column=1, selection=5, dirty=True)
        open_file(terminal, b)
        terminal.step("Down", "End")
        terminal.type("?")
        terminal.step("Home", "F3", "End", "F3", "Home")
        search(terminal, "beta[0-9]", mode=1, sensitive=True, in_selection=True)
        capture(terminal, "b-different-scope-options", [a, b])
        close_file(terminal, discard=True)
        terminal.wait_editor(**state_a)
        repeat_search(terminal)
        capture(terminal, "a-scope-after-close", [a, b])
        assert "Search string not found" not in terminal.text(), (
            "Closing B must restore A's needle, options and selection-search scope"
        )
        terminal.wait_editor(row=1, column=7, selection=5)
        expect_search(terminal, "alpha", in_selection=True)
        repeat_search(terminal)
        terminal.wait_for("Search string not found")
        terminal.step("Enter")
        save(terminal, a, b"alpha alpha!\noutside alpha\n")
        terminal.wait_file(b, second)
        capture(terminal, "scope-excludes-outside-match", [a, b])
        close_file(terminal)


def fresh_context_workflow(binary, tmux, root):
    a = fixture(root, "a.txt", b"alpha\noutside alpha\n")
    b = fixture(root, "b.txt", b"beta\n")
    new = root / "new.txt"
    with editor_session(binary, tmux, root, a) as terminal:
        terminal.wait_editor(row=1, column=1)
        terminal.step("End")
        terminal.type("!")
        terminal.step("Home", "F3", "End", "F3", "Home")
        search(terminal, "alpha", mode=1, sensitive=True, in_selection=True, whole=True)
        terminal.step("C-n")
        prompt(terminal, new)
        terminal.wait_editor(row=1, column=1, selection=0, dirty=False)
        capture(terminal, "new-buffer", [a, b, new])
        expect_search(terminal, "")
        terminal.type("NEW")
        save(terminal, new, b"NEW")
        close_file(terminal)
        expect_search(terminal, "alpha", mode=1, sensitive=True, in_selection=True, whole=True)
        open_file(terminal, b)
        expect_search(terminal, "")
        close_file(terminal)
        expect_search(terminal, "alpha", mode=1, sensitive=True, in_selection=True, whole=True)
        menu(terminal, 0, "Open file")
        prompt(terminal, root / "missing.txt")
        terminal.wait_for("Editor error")
        terminal.step("Enter")
        expect_search(terminal, "alpha", mode=1, sensitive=True, in_selection=True, whole=True)
        save(terminal, a, b"alpha!\noutside alpha\n")
        terminal.wait_file(b, b"beta\n")
        terminal.wait_file(new, b"NEW")
        capture(terminal, "fresh-and-failed-open-context", [a, b, new])
        close_file(terminal)


def quit_editor(terminal, menu_route):
    if menu_route:
        menu(terminal, 0, "Quit")
    else:
        terminal.step("F10")
    wait_title(terminal, "Close file")
    terminal.wait_for("Save before close?")


def wait_exit(terminal):
    terminal.wait_file(terminal.exit_file, b"0\n")
    terminal.wait(
        lambda: terminal.run(
            "display-message", "-p", "-t", "test:0.0", "#{pane_dead}"
        ).strip() == "1",
        "Editor exits only after all dirty-buffer decisions",
    )


def quit_stored_workflow(binary, tmux, root, menu_route, save_changes):
    original_a = b"alpha\n"
    original_b = b"beta\n"
    a = fixture(root, "a.txt", original_a)
    b = fixture(root, "b.txt", original_b)
    with editor_session(binary, tmux, root, a) as terminal:
        terminal.wait_editor(row=1, column=1)
        terminal.step("End")
        terminal.type("!")
        terminal.step("Home")
        search(terminal, "alpha")
        state_a = terminal.wait_editor(dirty=True, selection=5)
        open_file(terminal, b)
        search(terminal, "beta", sensitive=True)
        state_b = terminal.wait_editor(dirty=False, selection=4)
        quit_editor(terminal, menu_route)
        capture(terminal, "stored-dirty-quit-prompt", [a, b])
        terminal.type("c")
        terminal.wait_idle()
        terminal.wait_for("Save before close?", present=False)
        terminal.wait_editor(**state_b)
        terminal.wait_file(a, original_a)
        menu(terminal, 5, "Next buffer")
        terminal.wait_editor(**state_a)
        expect_search(terminal, "alpha")
        menu(terminal, 5, "Next buffer")
        terminal.wait_editor(**state_b)
        expect_search(terminal, "beta", sensitive=True)
        capture(terminal, "cancel-keeps-both-buffers-and-searches", [a, b])
        quit_editor(terminal, menu_route)
        terminal.type("y" if save_changes else "n")
        terminal.wait_idle()
        wait_exit(terminal)
        terminal.wait_file(a, b"alpha!\n" if save_changes else original_a)
        terminal.wait_file(b, original_b)
        capture(terminal, "stored-buffer-save-or-discard", [a, b])


def quit_cancel_later_workflow(binary, tmux, root):
    a = fixture(root, "a.txt", b"alpha\n")
    b = fixture(root, "b.txt", b"beta\n")
    c = fixture(root, "c.txt", b"clean\n")
    with editor_session(binary, tmux, root, a) as terminal:
        terminal.wait_editor(row=1, column=1)
        terminal.type("A")
        state_a = terminal.wait_editor(dirty=True)
        open_file(terminal, b)
        terminal.type("B")
        state_b = terminal.wait_editor(dirty=True)
        open_file(terminal, c)
        state_c = terminal.wait_editor(dirty=False)
        quit_editor(terminal, False)
        terminal.type("n")
        terminal.wait_idle()
        terminal.wait_for("Save before close?")
        terminal.type("c")
        terminal.wait_idle()
        terminal.wait_for("Save before close?", present=False)
        terminal.wait_editor(**state_c)
        menu(terminal, 5, "Next buffer")
        terminal.wait_editor(**state_a)
        menu(terminal, 5, "Next buffer")
        terminal.wait_editor(**state_b)
        menu(terminal, 5, "Next buffer")
        terminal.wait_editor(**state_c)
        terminal.wait_file(a, b"alpha\n")
        terminal.wait_file(b, b"beta\n")
        capture(terminal, "later-cancel-defers-earlier-discard", [a, b, c])
        quit_editor(terminal, True)
        terminal.type("n")
        terminal.wait_idle()
        terminal.type("n")
        terminal.wait_idle()
        wait_exit(terminal)
        terminal.wait_file(a, b"alpha\n")
        terminal.wait_file(b, b"beta\n")
        terminal.wait_file(c, b"clean\n")


def run(binary, tmux, parent, names):
    parent = Path(parent).resolve()
    parent.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="mce-", dir=parent))
    cases = {
        "switch-list": switch_list_workflow,
        "scoped-close": scoped_close_workflow,
        "fresh-context": fresh_context_workflow,
        "quit-stored-save": lambda binary, tmux, root: quit_stored_workflow(
            binary, tmux, root, menu_route=False, save_changes=True
        ),
        "quit-stored-discard": lambda binary, tmux, root: quit_stored_workflow(
            binary, tmux, root, menu_route=True, save_changes=False
        ),
        "quit-cancel-later": quit_cancel_later_workflow,
    }
    results = []
    for name in names or cases:
        directory = root / name
        directory.mkdir()
        try:
            cases[name](binary, tmux, directory)
            results.append({"case": name, "status": "PASS"})
        except Exception as error:
            details = traceback.format_exc()
            (directory / "failure.traceback.txt").write_text(details)
            results.append({
                "case": name, "status": "FAIL",
                "error": str(error) or repr(error), "error_type": type(error).__name__,
                "traceback": details, "artifacts": str(directory / "ui"),
            })
        print(f"[{results[-1]['status']}] E2 {name}", flush=True)
        if results[-1].get("error_type") == "ApplicationExitError":
            break
    (root / "results.json").write_text(json.dumps(results, indent=2))
    print(f"Editor workflow evidence: {root}", flush=True)
    return 0 if all(item["status"] == "PASS" for item in results) else 1


if __name__ == "__main__":
    raise SystemExit(run(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]))
