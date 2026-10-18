# mcpp

`mcpp` is an independently implemented, MIT-licensed orthodox terminal file
manager written in C++20. It provides a familiar two-pane workflow without
using Midnight Commander source code or resources.

The first provider is the local Linux filesystem. Planned providers are
Azure Blob Storage through azcpp. SSH/SFTP work is currently paused.

## Current functionality

- Independent dual panes with keyboard and mouse focus
- Directory history, quick change, hidden-file toggle, filtering, sorting,
  tree view, hotlist, and panelized search results
- Selection, masks, inversion, and recursive search results
- F2 user menu, F3 range/hex viewer, and F4 conflict-aware editor
- F5 copy, F6 move, F7 mkdir, F8 delete, F9 menu, and F10 quit
- Asynchronous copy/move/delete progress and cancellation
- Background completion preserves the current editor, viewer, or prompt
- Background failures remain visible on return to panels; `Esc` dismisses the notice
- Copy/move destinations accept directories or single-file exact names;
  relative destinations use the active pane and a trailing slash requests a directory
- Skipped sources are reported as incomplete and keep their selection marks
- Atomic no-clobber writes and symlink/root confinement
- Mode, owner/group, file information, free-space display, and links
- Directory comparison plus fixed-width Quick view and live Info panels
- Local command line, history, and a disposable interactive child shell
- Persisted panel paths and preferences
- MC-style visual state cues using a grayscale-only palette
- Toggleable VS Code Dark+-style syntax colors within grayscale UI chrome
- Toggleable editor line-number gutter and mcedit-style position header
- Persisted editor tab width, indentation, auto-indent, and whitespace markers
- Persistent linear/column blocks and atomic cross-session editor clipboard
- Persistent editor history and switchable multi-buffer editing
- Clickable editor menus and mouse-drag linear/Alt-drag column blocks

The implementation is behaviorally familiar but does not use Midnight
Commander source code, resources, strings, tests, or artwork. A shallow MC
checkout under ignored `reference/` is used only to study behavior and
documentation; no MC implementation is copied.

## Build

```bash
cmake -S . -B build/release \
  -DCMAKE_BUILD_TYPE=Release \
  -DMCPP_WARNINGS_AS_ERRORS=ON
cmake --build build/release --parallel 12
ctest --test-dir build/release --output-on-failure
```

The binary remains in the selected build directory; installation is not
required.

When Python 3 and tmux are available, CMake also enables isolated terminal
workflow tests. These check selection, navigation, dialogs, and persisted
state rather than just the presence of screen labels. No Python packages
are required.

FTXUI is pinned as a Git submodule:

```bash
git clone --recurse-submodules <repository>
```

Sandboxed destructive tests run without host mounts:

```bash
bash tests/run_container.sh
```

The focused operation regressions reuse an already-cached test image and
already-built binaries, without host mounts, dependency installs, or builds:

```bash
bash tests/run_operation_regressions.sh \
    build/release/mcpp build/release/mcpp_tests \
    ./operation-regression-evidence
```

The runner copies the binaries and tests with `docker cp`, runs model and
terminal/error-injection checks inside disposable containers, and preserves
filesystem hashes and terminal logs. The new model groups are explicitly
enabled in the guarded runner and the default Docker test image, and skipped
in ordinary host unit runs. Background race checks require overlap;
`MCPP_BG_BYTES` can increase their fixture from the default 1 GiB to 2 GiB.
The default cached image is `mcpp-test:${USER:-local}`, matching
`tests/run_container.sh`; `MCPP_TEST_IMAGE` can override it. If evidence
export fails, the runner reports failure and keeps its owned container for
recovery instead of discarding the evidence.

The current parity gaps against GNU Midnight Commander are documented in
[docs/mc-gap-analysis.md](docs/mc-gap-analysis.md).

## Initial key map

| Key | Action |
|---|---|
| `Tab` | Switch pane, or focus/unfocus quick view |
| `Enter` | Enter directory |
| `Ctrl-PageUp` | Parent directory |
| `Ctrl-PageDown` | Enter selected directory |
| `Insert` | Toggle selection |
| `+` / `-` | Select / unselect a case-sensitive filename mask |
| `*` | Invert files, including directory symlinks; leave real-directory marks unchanged |
| `Ctrl-S` / `Alt-S` | Incremental panel search |
| `Ctrl-X Q` | Toggle quick view |
| Tree `Enter` | Open selected directory and expand its branch |
| Tree `Space` / `+` / `-` | Toggle, expand, or collapse branch |
| Tree `F2` | Rescan the selected branch without losing selection |
| `F3` | View |
| `F4` | Edit |
| `F5` | Copy |
| `F6` | Move |
| `F7` | Create directory |
| `F8` | Delete |
| `F9` | Menu |
| `F10` | Quit |

Simple prompts use `Tab` / `Shift-Tab` to move between the input, OK and
Cancel. Input Enter accepts; focused buttons accept Enter or Space, and can
also be clicked. Escape cancels. File / View file... prompts for a filename;
relative filenames are resolved against the active pane.

Left / Right / Panelize by name... performs local recursive filename matching.
External-command panelization is not implemented: its Command menu entry is
disabled, and `Ctrl-X !` reports that it is unavailable.

## Editor buffers

Each editor buffer keeps its own search query, options, selection scope, cursor,
mark, and undo history. Moving a block keeps its destination marked. Rectangular
copies preserve real spaces without filling virtual source cells.

Click the editor header or press `F9` to open its menus, then click a category
or action. Drag with the left mouse button to mark text; hold `Alt` while
starting a drag to mark columns. Shift-click extends from the current cursor.

File > Close or `Esc` closes the active buffer. `F10` or File > Quit checks every
dirty buffer, including inactive buffers. Cancel keeps all buffers open, and
discard decisions take effect only when quitting completes. A failed save leaves
the buffers open.

## License

mcpp is MIT-licensed. FTXUI is also MIT-licensed and retains its own
copyright notice under `third_party/ftxui/LICENSE`.
