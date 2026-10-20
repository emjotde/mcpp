# mcpp

`mcpp` is an independently implemented, MIT-licensed orthodox terminal file
manager written in C++20. It provides a familiar two-pane workflow without
using Midnight Commander source code or resources.

Providers cover the local Linux filesystem and bounded, read-only tar/cpio
archives. Remote work, including Azure Blob Storage and SSH/SFTP, is paused.

## Current functionality

- Independent dual panes with keyboard and mouse focus
- Directory history, quick change, hidden-file toggle, filtering, sorting,
  tree view, hotlist, and panelized search results
- Cursor/word editing, selection, kill/yank and session history in text inputs
- Alt-Tab path completion in filename, directory and copy/move destination inputs
- Selection, masks, inversion, and recursive search results
- F2 user menu, F3 range/hex viewer, and F4 conflict-aware editor
- F5 copy, F6 move, F7 mkdir, F8 delete, F9 menu, and F10 quit
- Concurrent copy/move/delete jobs with independent progress and cancellation
- Per-file overwrite, skip, all/none and newer-source-only choices
- Retry, skip, ignore-all or abort on transfer/removal errors
- `Ctrl-X j` background jobs view with recent results and pause/resume/show/abort
- Background completion preserves the current editor, viewer, or prompt
- Independent viewer and diff screens alongside editor buffers
- Context-sensitive help with topics, links, and back navigation
- Background failures remain visible on return to panels; `Esc` dismisses the notice
- Copy/move destinations accept directories or single-file exact names;
  relative destinations use the active pane and a trailing slash requests a directory
- Skipped sources are reported as incomplete and keep their selection marks
- Atomic no-clobber writes and symlink/root confinement
- Source-cycle rejection, preserved copy modification times and copied hardlink groups
- Sparse regular-file copies and safe recreation of FIFOs and permitted device nodes
- Read-only tar/cpio browsing, viewing, extraction and native VFS paths
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
- MC-format configurable keys, including editor/manager extended keymaps
- Staged viewer hex/ASCII edits with explicit save/discard decisions
- UTF-8-aware character search, greedy wildcard captures and regex replacement escapes

The implementation is behaviorally familiar but does not copy Midnight
Commander implementation, resources, tests, or artwork. A shallow MC
checkout under ignored `reference/` is used only to study behavior and
documentation; no MC implementation is copied.

## Build

Requires a C++20 compiler, CMake, `pkg-config`, and PCRE2 10.34 or newer.
Debian/Ubuntu provide the PCRE2 headers and library in `libpcre2-dev`.

```bash
cmake -S . -B build/release \
  -DCMAKE_BUILD_TYPE=Release \
  -DMCPP_WARNINGS_AS_ERRORS=ON
cmake --build build/release --parallel 12
ctest --test-dir build/release --output-on-failure
```

The `mcpp`, `mcppedit`, `mcppview`, and `mcppdiff` binaries remain in the
selected build directory; installation is not required.

## Command-line controls

All four frontends share option parsing. `--help-all`, `--help-terminal`, and
`--help-color` describe the available controls. `--datadir`, `--datadir-info`,
and `--configure-options` report resource directories and build features
without starting the UI.

`-d` / `--nomouse` disables mouse reporting and mouse actions. `-a` /
`--stickchars` uses ASCII frames and Tree branches without changing file text.
`-b` / `--nocolor` uses terminal-default colors with text emphasis; `-c` /
`--color` enables at least the basic color palette. `-u` / `--nosubshell` and
`-U` / `--subshell` override persistent Bash for the current run.

```bash
build/release/mcpp --nomouse --stickchars --nocolor --nosubshell
build/release/mcppedit --colors 'editnormal=lightgray,blue:editmarked=black,cyan' file.txt
```

`-C` / `--colors` overrides supported color roles for this run without changing
the saved skin. Unknown roles and unsupported fields are errors, not ignored
settings. Native backend controls `-x/-X/-g/-t/-s/-k` remain unavailable;
`--help-terminal` explains why. ASCII frames do not implement non-UTF-8 terminal
output or 7-bit input.

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

## Screens and help

`Alt-Backtick` opens the screen list from the manager, editor, viewer, or diff.
`Alt-}` and `Alt-{` select the next and previous screen. Multiple viewers and
comparisons retain their own positions, search settings, bookmarks, staged edits,
and source lifetimes. Returning from the editor goes back to the comparison that
opened it. Closing one screen does not discard another screen's edits.

`F1` opens help for the current screen or dialog without dismissing it.
Use Tab/Shift-Tab to select links, Enter/Right to follow, F2 for the index, and
F3/Left to go back. Arrow/Page keys, Home/End, the wheel, and link clicks navigate
the page; F10 or Escape returns to the original screen. Key-learning and raw
key-capture prompts continue to receive F1 rather than opening help.
The English help is independently written; MC `.hlp` import and full-text
help search are not implemented.

`Ctrl-X j` lists file-operation jobs. Pause/Resume, Show, and Abort affect only
the selected job. Inputs, options, conflict answers, and cancellation belong to
that job, not whichever pane is active later. Background questions wait for
Show (F3); they do not replace an editor, help page, or unfinished dialog.
Commands can run while jobs continue. Exiting cancels and joins remaining jobs.

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

Use `Alt-Tab` (or Escape followed by Tab) to complete input.
The first press completes the common prefix; a second opens the matching list.
Configuration / Complete: show all opens that list on the first press.
Completion supports paths, `$` variables, `~` usernames and `@` hostnames;
commander command positions also complete builtins, reserved words and executable
PATH entries. Variables come from the process environment and users/hosts from
local system data; completion never runs shell hooks, substitutions or commands,
and performs no network lookup. Quotes, selections, cursor suffixes and UTF-8
boundaries are retained. Editor paths use the file's directory, while archive
members retain their provider through completion and F4 acceptance.

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
Editor codepage changes retain dirty text, marks and undo/redo history.
Unrepresentable conversions fail without replacing the draft or consuming
history. Saving retains the selected file encoding. Raw hexadecimal search
uses encoded file bytes and maps matches back to document positions, without
requiring a reopen with No translation.

Learn keys tests the listed terminal keys and teaches a missing sequence.
Printable ASCII keys, Tab, and Return are not accepted as replacement terminal
sequences. Configuration controls single/double Escape and the single-Escape
timeout. The build normalizes physical Ctrl-Space separately from FTXUI's
internal redraw events without modifying the pinned submodule.

The supported syntax choices and mcpp's rule/configuration file formats are
not MC's complete syntax database or configuration-file compatibility layer.
Remote providers remain explicitly paused. Local archive/VFS and remaining
shell-compatibility work are tracked separately from these local menu controls.

## Configuration files

New setups use `$XDG_CONFIG_HOME/mcpp/ini` and `panels.ini` (normally
`~/.config/mcpp/`). Existing flat `mcpp/config` setups still load and save in
their original format unless a native `ini` is supplied. Native settings overlay
existing legacy preferences on first use without duplicating histories.
No files in MC's own configuration directory are changed.

The native format applies supported local manager, layout, confirmation, panel,
editor, viewer, and diff settings. Save Setup preserves comments and unknown
settings, and saves each file privately and atomically. The two-file update is
not crash-atomic as a pair. Command-line skin and keymap overrides remain
launch-only unless explicitly changed in the UI.

Compatibility is partial: unsupported settings are diagnosed and preserved,
not silently applied or removed. Saved non-listing panel displays, legacy
terminal/codepage options, remote options, and other unmapped MC settings are
not implemented.

## Keymaps

All four executables accept `-K file` / `--keymap=file` and `--nokeymap`.
An explicit file takes precedence over the saved `keymap` setting and
`MCPP_KEYMAP`. Without those, mcpp looks for `mcpp.keymap` in its XDG config
directory, then `/etc/mcpp/`. A bare configured filename also searches those
directories. Command-line overrides are not saved.

Resources use MC section/action/key names, with semicolon-separated alternatives:

```ini
[editor]
Save = ctrl-s; f2
Quit = f10; alt-q

[filemanager]
ExtendedKeyMap = ctrl-x

[filemanager:xmap]
OptionsAppearance = a
```

Definitions replace an action's old bindings; an empty list unbinds it.
Untouched actions retain mcpp's existing shortcuts. Contexts cover panels,
editor, viewer/hex viewer, diff, menus, dialogs and focused controls. Extended
maps consume one following key; Escape cancels the prefix without quitting.
Learned terminal sequences are decoded before action lookup. Invalid resources
report their file, line and column. Binding an unavailable command reports an
error rather than running a different action.

## Dialog histories

Named dialog histories persist in the private `mcpp/input-history` file,
independently from Auto save setup. Filename/directory, copy destination,
search/replace/goto and explicitly named native inputs retain their own history.
History navigation preserves an unfinished draft and cursor when returning to
it. Cleanup uses the existing confirmation setting. Failed writes/acceptance
report an error without discarding the field; arbitrary menu prompt answers and
key queries are not automatically persisted.

## Directory hotlist

`Ctrl-\` opens the named, hierarchical directory hotlist. Native controls add
the current directory, create groups, move entries between groups and remove
entries with the configured confirmation. `Ctrl-X h` adds the current location;
the configurable `HotListAdd` action uses the same flow.

The private `mcpp/hotlist` file uses MC's public `GROUP` / `ENTRY ... URL` /
`ENDGROUP` format. Existing flat bookmarks are preserved when imported.
Failed writes and cancelled forms retain the pending label and existing data.
Archive entries store full native VFS locations, not ambiguous member paths.

## User and editor menus

Panel menus prefer a local `.mc.menu`, then `$XDG_CONFIG_HOME/mcpp/menu`.
Editor menus use `mcpp/mcedit/menu`, with the shared menu as a compatibility
fallback. Existing `key|label|command` rows keep their original macro rules.
Native menus accept key/title rows with indented command blocks and
condition/default-selection lines, for example:

```text
i Inspect selected file
    stat -- %f
```

Native expansion supports active/other-pane context, tagged files, prompts,
editor block/error scratch files and internal view/cd directives. Execution and
editor block replacement retain terminal, draft, lock and command-error guards.
Conditions do not execute shell commands. Ordinary macros keep values literal
inside quoted words, tagged lists, command substitutions, ANSI quotes, arithmetic,
and heredocs. `%0` explicitly inserts raw shell text instead; use it only when
that behavior is intended and the value is trusted.

`%cd` accepts one literal directory argument, menu macros, named homes, and
CDPATH. Relative paths use the provider and directory captured before prompts;
archive-relative paths never search host CDPATH directories.
Ordinary arithmetic values must be signed decimal integers; trusted expressions
require `%0`. Nested old-style backticks, `case` inside command substitutions,
unsupported heredoc delimiters, and values that would terminate a heredoc or
lose tabs with `<<-` fail explicitly. Modern `$(...)` substitutions are supported.

## External syntax definitions

The editor can load a user-authored MC `Syntax` registry from its config
directory and definitions from `syntax.d/`. Native patterns, contexts, keywords
and explicit colors feed the existing renderer; available definitions appear
in the syntax chooser. The lowercase legacy `syntax` override file and builtin
languages remain available. Selected external IDs persist in saved setup.
Loading is transactional: a malformed replacement reports an error while
retaining the current definition and draft. Registry filename and first-line
expressions use PCRE2, including lookarounds, Unicode properties and case folding.

This is not complete MC syntax compatibility yet. Keyword/context case folding
is ASCII-oriented and pattern sets are byte-based. Unsupported directives fail
explicitly. Context `spellcheck` flags
filter whole-document results and continuation checks independently of syntax
colors; explicit word checks remain available outside those contexts. If a
replacement changes delimiters and makes new text eligible, run another
whole-document check to include it. No MC syntax database is bundled.

## Extension actions

The config directory can contain native `mc.ext.ini` (Version 4.0), legacy
`mc.ext`, or the existing `extensions` rules, in that precedence. Native
filename/type/MIME matching selects Open/View/Edit actions and uses the shared
menu macro/directive engine. Legacy `pattern=command` keeps its full-path
expansion. Internal View honors a matching View action; external View follows
VIEWER/PAGER. Native Edit actions run with either editor preference.

Invalid resources and missing helpers report explicit errors, without executing
a legacy fallback. Native expressions use PCRE2. External archive commands
require a local copy; internal `%view` reads the archive provider. Compatibility
remains bounded by documented macro restrictions and a 16 MiB buffered-filter limit.

## Single-file transformations

Native single-file paths work with the viewer, editor and comparison frontends:

```bash
build/release/mcppview 'report.gz/ugz://'
build/release/mcppdiff report.txt 'report.gz/ugz://'
```

The result is one read-only file, not a directory. A converter such as `gz`
compresses its source; `ugz` decompresses it. The corresponding standard tools
also support bz, bz2, lz, lz4, lzma, xz and zst converters when installed.
Missing tools and failed conversions report errors. Parsed viewing recognizes
compressed output without treating the provider's internal `/` as a host path.
Editor Save As can create a writable local copy; saving back never runs a
reverse conversion.

F5's Source mask also accepts an explicit operand such as `report.gz/ugz://`.
It resolves from the captured source pane; a relative destination still uses
that pane's directory. Unrelated selection marks are retained. F6 rejects
read-only transformed sources before creating a destination.

Local `/1` commands in `sfs.ini` receive a private source copy as `%1`, the
literal suffix parameter as `%2`, and a pre-created output file as `%3`.
The registry comes from `MC_DATADIR/sfs.ini` when set, otherwise
`/etc/mcpp/sfs.ini` or `/etc/mc/sfs.ini` when present. Commands are trusted user
configuration, not sandboxed plugins. No MC helper rules are bundled.

Inputs and outputs are currently limited to 64 MiB each. Helpers also have
time/memory limits. Cached snapshots retain the source provider and revision;
changed inputs or rules produce new results without changing open viewers.
Non-local helper flags are rejected explicitly.

## Local archives and copying

Enter opens recognized tar/cpio files; directories and `..` navigate inside,
and `Ctrl-PageUp` at the archive root returns to its containing local directory.
Viewing and copying out use the archive provider, not similarly named host
paths. File bytes, modes, modification times, safe symlinks and hardlink groups
are retained when requested. Archives are read-only in the UI, like MC TarFS:
editor Save reports that restriction, while Save As can publish a local copy.
External file commands and comparisons require a local copy.

Native paths work at startup, in directory changes and in saved pane paths:

```text
/path/to/sample.tar/utar://directory
/path/to/sample.cpio/ucpio://directory/file
```

Active VFS list (`Ctrl-X a`) reopens cached archives. Virtual FS options set
the timeout for freeing unused providers; active panes, viewers and editor
buffers keep their snapshots alive. A changed container opens a new revision.
Command execution from an archive uses its containing local directory and does
not replace the archive pane with the shell's physical directory.

The archive reader supports ustar/common GNU/PAX tar and newc/CRC cpio.
Compressed variants use installed `gzip`, `bzip2`, `xz`, or `zstd` commands;
missing tools fail explicitly. Limits are 64 MiB encoded, 256 MiB expanded,
100,000 members and 4,096 bytes per member path. Nested archives, sparse/special
archive members, older cpio formats and other local VFS families remain open
compatibility work. Archive data is indexed in memory, never blindly extracted
into a host directory.

Local copying retains sparse holes where the filesystem supports them, with
exact logical bytes and size. FIFOs and device nodes are recreated rather than
read as streams; device creation still needs normal Linux permissions. Unix
sockets are rejected. Cancellation, source identity, overwrite choices and
hardlink groups retain their guards. A failed final directory sync is reported
even if the destination has already been published.

Copy/move dialogs support filename transformation masks as well as source
filtering. Captures and case-conversion replacements can rename matching files;
shell-style masks and ECMAScript regex are supported, not full PCRE. The native
Dive option controls placement into an existing destination folder. Stable
symlinks adjusts local relative links to keep their original referent, using
revision-checked staged publication. Unsupported nonlocal stable-link handling
is not silently substituted.

Regular local-file collisions offer native Append and Reget choices when
applicable. Append publishes the destination followed by the source; Reget
requires a smaller destination matching the source prefix and copies only the
remaining suffix. Both stage publication and retain revision/cancellation
checks. Archive, remote, symlink and special-file append is not advertised.

Recursive deletion asks before descending into nonempty directories, with
native Yes/No/All/None/Abort choices. All and None apply to the rest of that
operation. None leaves nonempty directories and their selection marks intact,
while selected regular files and empty directories can still be removed.
Safe delete defaults to No in both initial and recursive confirmations.

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
Search highlights take priority over changed-row colors in comparisons.
Disabled menu labels use the skin's inactive-menu colors and attributes without
extra forced dimming; the Default and Grayscale fallbacks remain readable.

User-provided MC-format skins can also be selected by name or file path.
Appearance lists discovered skin files alongside the sixteen original palettes;
preview, Cancel and saved selection retain the same behavior. Native color
aliases, inherited values and supported attributes are applied to actual roles.
Reloading an invalid resource reports an error and retains the previous palette.
Native `inverse` and `reverse` attributes are both accepted. Widget-character
settings are recognized as metadata rather than mistaken for color names.
F2 popup normal, title and selected colors are independent of main-menu colors.
Fallback editor syntax colors follow the editor background, including light
editors in otherwise dark skins.

Native `filehighlight.ini` filename/type rules and the legacy `highlighting`
file feed the selected skin's file-colour groups. Rules that do not supply a
matching group leave builtin type colours intact. Search includes mcpp's config
and installed resource directories; no MC skins or rules are bundled.
Native filename expressions use PCRE2. Compatibility remains partial for custom
line/widget glyphs and some auxiliary widget-colour roles.
Unsupported glyph customization produces a warning without preventing the skin's
colors from loading.

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

Local source growth, truncation and replacement are observed without reopening.
For redirected input, pass `/dev/stdin` (or an fd alias) to `mcppview` or
`mcpp --view`; `-` remains a literal filename. Streams spool to private seekable
storage, remain raw/read-only, and can be left before EOF. Keyboard input comes
from the terminal rather than the data stream. Source changes while hex edits
are staged report a conflict instead of resampling a newer save revision;
changed formatted snapshots require an explicit Raw/Unformat refresh.

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
Parsed/formatted data uses private seekable storage instead of the former
64 MiB memory-buffer limit. Mapping uses owned immutable snapshots, not live
files that could be truncated underneath it. Advanced searches retain the
existing wildcard, word, direction and codepage behavior. Full POSIX-regex
subjects remain limited to 2,147,483,647 bytes by libc; disk space, `off_t`,
provider limits and regex workspace impose their own explicit limits.
Ordinary raw viewing and forward literal/hex search retain bounded range reads.

Digits `0`-`9` choose a viewer bookmark slot; `m` stores the displayed position
and `r` restores it. `Alt-R` cycles ruler off/top/bottom. `Alt-E` chooses a data
codepage and `Alt-Shift-E` opens viewer history. `Ctrl-F` / `Ctrl-B` open adjacent
files, checking staged hex changes first. Quick view remains deliberately
read-only and supports Go to, native Search, Parse and codepage selection.

The wheel scrolls hex content immediately while retaining the cursor's relative
row. Hex progress follows the active byte. Click a hex digit or ASCII byte to
position the cursor, including the low nibble of a hex byte. Both columns
highlight the selected byte; Tab changes which column receives input without
losing the matching-byte highlight. Edits stay staged
until `F6` Save or an explicit save decision on exit. Hardlinked files offer
detach / preserve links / cancel before publication; preserve links is an
explicit non-atomic write. The final byte reports 100%.
Manual navigation becomes the next search origin. Cancelled/failed wrap
attempts retain the visible location, and successful text searches reveal
their hit through wrapping and horizontal scrolling.

## Editor buffers

Writable local files acquire an MC-compatible `.#filename` lock on the first
change, not merely when opened. A conflicting edit offers Grab lock or Ignore
lock; Escape cancels without changing the draft. Pending edits, asynchronous
results and macros resume after an accepted decision. Save As keeps source and
destination decisions separate, and successful save/close releases owned locks.
A session never deletes a lock that another session has taken over.
Readonly archive members and temporary diff buffers do not create host locks.

Files above 64 MiB require confirmation before their contents are read.
Accept opens the file; declining retains the current draft and other queued
CLI files. This applies to manager editing, editor Open/history, standalone
startup and Find/Diff round trips. Exactly 64 MiB opens without that warning.
The former 16 MiB rejection is removed, and backups use guarded streaming
copies rather than a separate 16 MiB limit.

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
