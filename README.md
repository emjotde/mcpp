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
- Cursor/word editing, selection, kill/yank and session history in text inputs
- Alt-Tab path completion in filename, directory and copy/move destination inputs
- Selection, masks, inversion, and recursive search results
- F2 user menu, F3 range/hex viewer, and F4 conflict-aware editor
- F5 copy, F6 move, F7 mkdir, F8 delete, F9 menu, and F10 quit
- Asynchronous copy/move/delete progress and cancellation
- Per-file overwrite, skip, all/none and newer-source-only choices
- Retry, skip, ignore-all or abort on transfer/removal errors
- `Ctrl-X j` background jobs view with recent results and pause/resume/show/abort
- Background completion preserves the current editor, viewer, or prompt
- Background failures remain visible on return to panels; `Esc` dismisses the notice
- Copy/move destinations accept directories or single-file exact names;
  relative destinations use the active pane and a trailing slash requests a directory
- Skipped sources are reported as incomplete and keep their selection marks
- Atomic no-clobber writes and symlink/root confinement
- Source-cycle rejection, preserved copy modification times and copied hardlink groups
- Creation permissions follow the process umask when permissions are not preserved
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
- Clickable function-key bars, consistent modal controls and pointer-editable command text
- Direct View/Edit from Find results with the result position retained on return
- Stable listing viewports and filename-preserving headers for long paths
- Staged viewer hex/ASCII edits with explicit save/discard decisions
- UTF-8-aware character search, greedy wildcard captures and regex replacement escapes

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
| Tree click / double-click | Select a directory / open it in the opposite pane |
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

Moving up through Tree ancestors loads their immediate folders in the background.
Expanded branches and the selected path are retained; unvisited subtrees are not
scanned recursively.

Simple prompts use `Tab` / `Shift-Tab` to move between the input, OK and
Cancel. Input Enter accepts; focused buttons accept Enter or Space, and can
also be clicked. Escape cancels. File / View file... prompts for a filename;
relative filenames are resolved against the active pane. Prefilled defaults
are selected for replacement; recalled history and command drafts stay editable.

Text inputs support `Left`/`Right`, `Home`/`End`, `Ctrl-Left`/`Ctrl-Right`,
Shift movement for selection, and Delete/Backspace at the cursor. `Ctrl-K`
kills to the end, `Ctrl-W` kills the previous word, and `Ctrl-Y` yanks it back.
`Alt-P`/`Alt-N` recall session history. In dialogs, `Ctrl-U` clears the field;
the commander's `Ctrl-U` pane-swap shortcut is unchanged.

Use `Alt-Tab` (or Escape followed by Tab) to complete a dialog path.
Repeated `Alt-Tab` cycles ambiguous matches. This completes filesystem paths,
not shell variables or command names. Editor file dialogs resolve and complete
relative paths against the edited file's directory.

Left / Right / Panelize by name... performs local recursive filename matching.
External-command panelization is not implemented: its Command menu entry is
disabled, and `Ctrl-X !` reports that it is unavailable.

## Editor buffers

Each editor buffer keeps its own search query, options, selection scope, cursor,
viewport, mark, and undo history. Moving a block keeps its destination marked. Rectangular
copies preserve real spaces without filling virtual source cells.

Atomic editor saves retain user attributes, access ACLs and ownership where
permitted. A linked file offers an explicit detach/preserve-links/cancel choice.
Preserving links writes the existing inode and is not atomic; cancellation and
write errors are reported rather than represented as a completed atomic save.
Read-only files are not silently replaced. Save As supports confirmed replacement
and unchanged/LF/CRLF/CR line endings.

Undo stores edit differences rather than a full document for every keystroke, so
large documents retain useful history within the memory budget. Persistent marks
survive ordinary range edits and Undo. Regex replacement supports zero-width
anchors, capture expansion, escapes and case transformations.

Scrolling keeps the view steady until the cursor reaches an edge. PageUp/Down
move the view and cursor together, and the final page stays filled through EOF.
`Ctrl-Home`/`Ctrl-End` go to the first/last line while retaining the preferred
column; `Ctrl-PageUp`/`Ctrl-PageDown` go to the top/bottom visible line.
`Ctrl-Up`/`Ctrl-Down` scroll one line; the mouse wheel scrolls two.
`F8` deletes the marked block, or the whole current line if no block is marked.
Delete still removes one character, and `Ctrl-U` undoes either deletion.

`F2` and File > Save ask for confirmation with Save/Cancel, including for a
clean buffer. Cancel preserves the draft, cursor and selection. Choosing Yes
in the modified-file close/quit dialog saves without asking a second time.

Click the editor header or press `F9` to open its menus, then click a category
or action. Drag with the left mouse button to mark text; hold `Alt` while
starting a drag to mark columns. Shift-click extends from the current cursor.

File > Close closes the active buffer. `Esc`, `F10` and File > Quit check every
dirty buffer, including inactive buffers. Cancel keeps all buffers open, and
discard decisions take effect only when quitting completes. A failed save leaves
the buffers open.

## License

mcpp is MIT-licensed. FTXUI is also MIT-licensed and retains its own
copyright notice under `third_party/ftxui/LICENSE`.
