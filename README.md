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
- Local command line, history, and persistent Bash with Ctrl-O switching
- Persisted panel paths and preferences
- MC-style visual state cues, a grayscale default, and 16 selectable skins
- Toggleable syntax colors adapted to dark, light, and monochrome skins
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

The `mcpp`, `mcppedit`, `mcppview`, and `mcppdiff` binaries remain in the
selected build directory; installation is not required.

## Standalone editor

`mcppedit` opens the shared editor directly, without entering the file manager:

```bash
build/release/mcppedit document.txt
build/release/mcppedit +120 source.cpp other.cpp
build/release/mcppedit source.cpp:120 other.cpp:40:
build/release/mcppedit -- -draft.txt
build/release/mcppedit
```

This is a separate C++ executable, not a shell wrapper or an installed alias.
It shares the editor, preferences, histories, clipboard, and save protections
with `mcpp --edit`. Existing and new filenames are supported. Without filenames,
an unnamed buffer opens; no placeholder file is created. Saving it requests a
name, including when saving an inactive buffer during Quit, and cancellation
retains the draft.

Multiple files open as buffers in argument order, duplicate paths are opened
once, and `+line` selects the starting line in the first file. Each filename can
also carry `:line` or `:line:`; an existing literal filename takes precedence
over that notation. A zero line selects the first line. `mcpp --edit` accepts
the same multi-file and line-position forms. CLI filenames resolve against the
process working directory, not an automatically restored panel directory.
As in MC, all positional arguments with `--edit` are editor files, not pane
directories; pane paths can still be restored from saved setup.
Closing the final buffer or quitting exits the editor process. Help and version
are available through `--help` and `--version`.

## Standalone viewer and diff

```bash
build/release/mcppview document.txt
build/release/mcppview -- -draft.txt
build/release/mcppdiff old.cpp new.cpp
```

Both are separate C++ executables using the shared viewer or diff/merge UI.
Quit exits the process rather than returning to the file manager. Staged hex
edits and merged changes retain the same Save/Discard/Cancel, revision,
read-only, and linked-file protections. Editing from a standalone diff returns
to that comparison before quitting the process.

## Testing

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
Copy/Move keeps its action buttons and source identity visible while the options
body scrolls with the wheel or PageUp/PageDown. Main menus scroll their selected
action into view on short terminals.
Press `F9`, then a main-menu category's underlined letter (`l`, `f`, `c`, `o`
or `r`) to open it without changing the command draft. Uppercase works too.
Quick cd resolves relative directories against the active pane and retains
logical symlink paths. Paths in the prompt and status escape control characters
instead of breaking the terminal layout.

Text inputs support `Left`/`Right`, `Home`/`End`, `Ctrl-Left`/`Ctrl-Right`,
Shift movement for selection, and Delete/Backspace at the cursor. `Ctrl-K`
kills to the end, `Ctrl-W` kills the previous word, and `Ctrl-Y` yanks it back.
`Alt-P`/`Alt-N` recall session history. In dialogs, `Ctrl-U` clears the field;
the commander's `Ctrl-U` pane-swap shortcut is unchanged.

Use `Alt-Tab` (or Escape followed by Tab) to complete a dialog path.
The first press completes the common prefix; a second opens the matching paths.
Configuration / Complete: show all opens that list on the first press.
This completes filesystem paths,
not shell variables or command names. Editor file dialogs resolve and complete
relative paths against the edited file's directory.

Find results can be panelized with the Panelize button or `Alt-P`.
Left / Right / Panelize restores the last panelized listing; it does not start a
new filename search. Command / External panelize (`Ctrl-X !`) offers the
reference command list, editable command, Panelize, Remove, Add new, and Cancel
controls. It runs in the pane directory and checks every returned pathname
before replacing the listing. Named commands persist in mcpp's configuration.

## Local menu controls

The five manager menus and seven editor menus follow MC 4.8.30's labels, order,
separators, and printed shortcuts. Underlined item letters activate enabled
actions inside an open dropdown, and an outside click dismisses the menu.

Listing format uses Full / Brief / Long / User defined radio choices, Brief's
column count, the user format input, and User mini status. Brief supports 1-9
columns; Long is the full-width, single-row permissions/link/owner/group listing.
User formats accept MC's `half` / `full`, column count, field names,
`<` / `=` / `>` alignment, `:width`, and `+` expansion syntax.
For example: `half type name | size | mtime`.

Sort order offers Unsorted, Name, Version, Extension, Size, Modify time, Access
time, Change time, and Inode, with Executable first, Case sensitive, and Reverse.
Mix all files belongs in Panel options, not Sort order. Panel options also
controls mini-status, SI units, backup/hidden files, fast reload, marking,
navigation, highlighting, quick-search case handling, and saved pane setup.
Explicit Rescan / `Ctrl-R` bypasses fast reload. Explicit CLI pane paths override
automatically saved paths.

Configuration, Layout, Confirmation, Appearance, Learn keys, Chattr, Advanced
chown, and editor Save mode use reference controls rather than generic
substitute options. Layout's split buttons display actual columns or rows, not
percentages. Hidden command input cannot execute an invisible draft.
Appearance provides 16 independently authored skins and optional dialog shadows.
The native skin list previews a selected palette; Cancel restores both the
original skin and shadows.

The terminal renderer requires UTF-8. Display bits reports that fixed terminal
mode; legacy non-UTF-8 terminal output and 7-bit input are not implemented.
The separate pane/editor Encoding commands use the reference Choose codepage
list to interpret file data without renaming files or converting them on disk.
Editor codepage changes require a clean buffer without undo/redo history;
unrepresentable input is rejected before changing the draft. Saving retains the
selected file encoding. Raw hexadecimal search in another file codepage
requires reopening with No translation.

Learn keys tests the listed terminal keys and teaches a missing sequence.
Printable ASCII keys, Tab, and Return are not accepted as replacement terminal
sequences. Configuration controls single/double Escape and the single-Escape
timeout. The build normalizes physical Ctrl-Space separately from FTXUI's
internal redraw events without modifying the pinned submodule.

The supported syntax choices and mcpp's rule/configuration file formats are
not MC's complete syntax database or configuration-file compatibility layer.
Remote providers remain explicitly paused. Local archive/VFS and remaining
shell-compatibility work are tracked separately from these local menu controls.

## Shell

With Bash, commander commands and `Ctrl-O` use the same interactive shell.
Variables, functions, aliases, and the shell PID survive repeated commands and
screen switches. `Ctrl-O` also hides a running foreground command or Bash
`read`; it does not stop the command. Resume it with `Ctrl-O`.

An unfinished shell line or running command prevents a second commander command
from being injected into it. The rejected commander draft remains editable for
retry. File-operation jobs can remain running or paused while shell commands
execute. Completion updates the pane cwd without another key, but does not undo
a directory the user navigated to manually.

Bash startup uses `$XDG_DATA_HOME/mcpp/bashrc` (normally
`~/.local/share/mcpp/bashrc`), falling back to `~/.bashrc`. An mcpp-specific
`inputrc` in the same data directory is loaded when present. Existing prompt
hooks and command status are retained. Logical paths and paths containing
spaces or newlines are synchronized without shell interpolation.

Exiting Bash quits mcpp through its existing confirmation and dirty-editor
guards. Shell startup and transport errors restore the UI and report the
failure. Pause-after-run displays the completed command's output before waiting
for a key; hiding a still-running command does not trigger that pause.

`mcpp -u` / `--nosubshell` selects the earlier one-shot behavior for that run;
`-U` / `--subshell` enables persistent Bash. These overrides do not replace the
saved `use_subshell` preference. Non-Bash shells currently retain one-shot
behavior; persistent ash/dash, ksh, zsh, tcsh, and fish remain compatibility work.

## Skins

Choose **Options / Appearance / Skin** and accept with OK. Saved setup restores
the choice on the next launch. Grayscale remains the default.

| Style | Skin names |
|---|---|
| Dark | `default` (classic blue), `midnight`, `graphite`, `cobalt`, `ocean`, `forest`, `plum`, `rose`, `ember` |
| Light | `paper`, `sand`, `ice` |
| Grayscale / terminal | `grayscale` (shown as `gray`), `amber`, `green` |
| High contrast | `contrast` |

Skins color the panes, menus, dialogs, key bars, Tree, viewer, diff, and editor.
Light skins use darker syntax colors; Amber and Green tint syntax as well as
the surrounding UI. Grayscale retains its optional Dark+-style syntax colors.
Palette selection follows the terminal's reported color capabilities.

All four executables also accept a launch-only override:

```bash
build/release/mcpp --skin midnight
build/release/mcppedit -S paper source.cpp
build/release/mcppview --skin=ocean document.txt
build/release/mcppdiff --skin contrast old.cpp new.cpp
```

The command-line override does not replace the saved skin. An explicit skin
choice in Appearance does. Unknown skin names are errors rather than silent
fallbacks. `gray` and `blue` remain aliases for `grayscale` and `default`.
The legacy saved value `classic` retains its previous grayscale appearance, so
upgrading does not prevent startup or require rewriting the configuration.
Dropdown selections, shortcut letters, the F2 user menu, focused controls, and
popup lists also use skin colors; highlights do not merely invert a near-white
label color shared by several palettes.

## Modal Tree and diff/merge

Command / Directory tree is the same lazy hierarchical Tree used by the panes,
not a one-directory list. Right expands or selects a child; Left collapses or
selects the parent. Enter chooses the directory and Escape cancels without
changing the pane or its marks. `Ctrl-S` searches visible names. F2 rescans,
F3 forgets cached entries without deleting directories, and F4 switches linear
and sibling navigation. Copy/Move/Rmdir return to the Tree after their dialogs
and operations. The original sidebar selection survives the modal chooser.
Visited directories persist in the private `$XDG_CACHE_HOME/mcpp/Tree` cache;
stale children are checked by lazy rescans.

Command / Compare files (`Ctrl-X Ctrl-D`) compares the two selected regular
files side by side. `n` / `p` navigate hunks, `F5` merges displayed left to
right, and Shift-F5 merges right to left. Merges are staged and undoable with
`Ctrl-Z`; no file changes until `F2` saves. Each save retains revision, read-only,
metadata, symlink, and hardlink safeguards. Quit offers Save/Discard/Cancel
without losing a failed-save draft.

Diff `F4` opens the displayed left file in the editor and returns to the
comparison; Shift-F4 edits the other file. F7 searches and F9 offers Normal,
Fastest, Minimal, and the five native case/tab/space/carriage-return options.
`Ctrl-U` swaps displayed sides, `Alt-N` toggles numbers, `Alt-S` toggles symbols,
`<` / `>` adjust the split, and `=` restores equal widths.

## Viewer controls

`F4` switches text/hex display. `F2` toggles text wrapping or hex editing.
`F5` Go to supports one-based logical line numbers, percentages, decimal byte
offsets and hexadecimal offsets. It defaults to lines in text mode and hex
offsets in hex mode. Tab focuses Mode; Left/Right changes it. Invalid input
stays in the dialog for correction, and 100% shows the final page.

Text searches highlight the current occurrence in the viewer and Quick view.
The highlight follows the original UTF-8 bytes through expanded tabs, wrapping
and horizontal clipping; repeating a search moves it to the next occurrence.
The native Search dialog offers Normal, Regular expression, Hexadecimal, and
Wildcard search, with Case sensitive, Backwards, Whole words, and All charsets.
`/` / `?` start forward/backward searches; `Ctrl-S` / `Ctrl-R` continue in that
direction. All charsets interprets candidates using the supported codepages and
maps hits back to the original bytes.

F8 switches Raw/Parse. Parse recognizes gzip, bzip2, xz, and zstd data through
the corresponding installed standard command; missing/failed tools are explicit
errors. Parsed data is read-only, so hex Save cannot replace a compressed file
with decompressed bytes. F9 toggles nroff overstrike formatting, including bold
and underlined text. Formatting and codepages never convert the file on disk.
Parsed/formatted buffers and advanced searches have a 64 MiB buffer limit;
ordinary raw viewing and forward literal/hex search retain bounded range reads.

Digits `0`-`9` choose a viewer bookmark slot; `m` stores the displayed position
and `r` restores it. `Alt-R` cycles ruler off/top/bottom. `Alt-E` chooses a data
codepage and `Alt-Shift-E` opens viewer history. `Ctrl-F` / `Ctrl-B` open adjacent
files, checking staged hex changes first. Quick view remains deliberately
read-only and supports Go to, native Search, Parse and codepage selection.

The wheel scrolls hex content immediately while retaining the cursor's relative
row. Hex progress follows the active byte. Click a hex digit or ASCII byte to
position the cursor, including the low nibble of a hex byte. Edits stay staged
until `F6` Save or an explicit save decision on exit. Hardlinked files offer
detach / preserve links / cancel before publication; preserve links is an
explicit non-atomic write. The final byte reports 100%.
Manual navigation becomes the next search origin. Cancelled/failed wrap
attempts retain the visible location, and successful text searches reveal
their hit through wrapping and horizontal scrolling.

## Editor buffers

General options uses the native Wrap mode, Tabulation, and Other options groups.
None, Dynamic paragraphing, and Type writer wrap all change typing behavior.
Tabulation controls fake half tabs, indentation backspace, fill-with-spaces and
tab spacing; there is no independent invented indent-width control.
Visible tabs and trailing spaces are separate preferences.

Persistent selection off replaces a marked block when typing and clears it on
ordinary motion. Cursor beyond end of line keeps a virtual display position
without creating file bytes until insertion, including mouse positioning and
column marks. Group undo combines consecutive editing actions; navigation and
compound edits separate groups. Automatic wrapping and block replacement remain
single undoable edits. Save confirmation, saved file positions, cursor-after-
insertion and syntax highlighting are independently configurable.

Each editor buffer keeps its own search query, options, selection scope, cursor,
viewport, mark, and undo history. Moving a block keeps its destination marked. Rectangular
copies preserve real spaces without filling virtual source cells.
Search highlighting is separate from the user's persistent linear or column
mark; searching never changes what F8 or block-copy/delete will operate on.
CR-only files keep logical lines after Save As and subsequent editing.

Atomic editor saves retain user attributes, access ACLs and ownership where
permitted. A linked file offers an explicit detach/preserve-links/cancel choice.
Preserving links writes the existing inode and is not atomic; cancellation and
write errors are reported rather than represented as a completed atomic save.
Read-only files are not silently replaced. Save As supports confirmed replacement
and unchanged/LF/CRLF/CR line endings.

File / Insert file and Copy to file use the reference filename prompts.
Their default managed clipfile remains compatible with mcpp's linear/column
clipboard format. Bookmarks (`Alt-K`, `Alt-J`, `Alt-I`, `Alt-O`) and saved
file positions are retained. Window / Move and Resize use arrows and mouse
dragging; Enter accepts and Escape rolls back the geometry.

Save mode offers Quick save, Safe save, and backups with an editable extension,
plus the POSIX-newline warning. Safe save remains mcpp's safer default.
Backup files retain the old bytes and metadata. Quick save is explicitly
in-place; hardlinked saves still require a detach/preserve decision.

Start/Stop record macro records editing keys and asks for the macro hotkey when
stopped. Delete macro asks for that hotkey. Record/Repeat actions starts and
stops recording, then asks only for the repeat count. Literal insertion reads
the next key, not a made-up hexadecimal-input dialog. Date/time inserts the
locale's current date/time immediately. Sort requires a marked block and asks
for `sort(1)` options; paragraph formatting uses the configured wrap length.

External formatter reads the first command from
`$XDG_DATA_HOME/mcpp/mcedit/macros.d/macro.0.sh` (or the equivalent
`~/.local/share` path), using mcpp's existing `key|label|command` menu format.
Its stdin is the marked text; stdout replaces that block, or is inserted at the
cursor if no block exists. Missing/failed commands do not change the draft.
This resource format is not MC macro-file compatibility.

Undo stores edit differences rather than a full document for every keystroke, so
large documents retain useful history within the memory budget. Persistent marks
survive ordinary range edits and Undo. Regex replacement supports zero-width
anchors, capture expansion, escapes and case transformations.

Scrolling keeps the view steady until the cursor reaches an edge. PageUp/Down
move the view and cursor together, and the final page stays filled through EOF.
`Ctrl-Home`/`Ctrl-End` go to the first/last line while retaining the preferred
column; `Ctrl-PageUp`/`Ctrl-PageDown` go to the top/bottom visible line.
`Ctrl-Up`/`Ctrl-Down` scroll one line; the mouse wheel scrolls two.
`Ctrl-Left`/`Ctrl-Right` stop between word and punctuation runs and at line
boundaries without splitting UTF-8 characters.
`F8` deletes the marked block, or the whole current line if no block is marked.
Delete still removes one character, and `Ctrl-U` undoes either deletion.

`F2` and File > Save ask for confirmation with Save/Cancel, including for a
clean buffer, when General / Confirm before saving is enabled. Cancel preserves
the draft, cursor and selection. Choosing Yes
in the modified-file close/quit dialog saves without asking a second time.

Command / Mail (`Alt-M`) uses the native To/Subject/Copies to dialog and sends
the current UTF-8 buffer snapshot to the installed `mail(1)` command through
argv and a private regular-file stdin, never through shell expansion. Recipients
cannot be command options and header fields reject control characters.
Missing/failed mail commands retain both the dialog values and unsaved editor
buffer. A completed command is not a guarantee of recipient delivery.

Command / Spell check checks the document, Check word (`Ctrl-P`) checks only the
word at the cursor, and Change spelling language opens the installed dictionary
list. The native Check word controls offer suggestions and Add word, Replace,
Skip, and Cancel. Replacements remain unsaved edits with Undo; the user's block
and search settings remain independent. Add word persists an Aspell personal
dictionary and checks that a fresh checker process recognizes the word.

Spelling uses the installed standard `aspell(1)` command through argv and a
private regular-file stdin, not a new linked library. UTF-8 byte offsets are
checked against the draft before replacement. Document lines cannot become
Aspell dictionary commands. Missing tools/dictionaries, malformed responses,
and failed personal-dictionary writes are reported without losing the draft;
failed Add word can be retried in the same dialog. A selected dictionary is
validated before replacing the saved spelling-language preference.

Click the editor header or press `F9` to open its menus, then click a category
or action. Drag with the left mouse button to mark text; hold `Alt` while
starting a drag to mark columns. Dragging past the visible top or bottom edge
scrolls to extend the selection, bounded by the document ends. Shift-click
extends an existing mark from its original anchor without changing its linear
or rectangular shape; without a mark, it starts from the current cursor.

File > Close closes the active buffer. `Esc`, `F10` and File > Quit check every
dirty buffer, including inactive buffers. Cancel keeps all buffers open, and
discard decisions take effect only when quitting completes. A failed save leaves
the buffers open.

## License

mcpp is MIT-licensed. FTXUI is also MIT-licensed and retains its own
copyright notice under `third_party/ftxui/LICENSE`.
