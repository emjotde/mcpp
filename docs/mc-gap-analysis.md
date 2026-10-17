# mcpp versus GNU Midnight Commander: gap analysis

Review date: 2026-10-17

Compared versions:

- GNU Midnight Commander 4.8.30, installed at `/usr/bin/mc`
- mcpp working tree based on commit `37263dc`

## Conclusion

mcpp now provides a comparable Linux local-filesystem workflow for the common
commander, viewer, and editor paths covered below. It is not a complete clone
of GNU Midnight Commander.

A ten-topic GNU Screen audit captured 115 matched MC/mcpp pairs covering panel
navigation, menus, selection, Quick view, viewer, editor, operations,
metadata/links, and terminal layouts. The comparison drove fixes for menu
activation, shortcuts, focused Quick view, viewer paging/chrome, selection
masks, editor indentation/clipboard behavior, operation cancellation redraw,
terminal sizing, and shell transitions.

The largest remaining gaps are advanced panel modes, richer operation and
metadata dialogs, persistent subshell integration, help/jobs, macros,
archive/VFS support, and SSH/cloud providers.

## Review method

The review used:

1. MC's installed binary, help, configure flags, and VFS installation.
2. MC's default keymap: 201 unique action names and 393
   context/action combinations across file manager, panel, input, menu,
   dialog, tree, help, editor, viewer, hex viewer, and diff viewer contexts.
3. MC's manual sections for panels, menus, operations, search, hotlist,
   options, viewer, editor, subshell, and VFS.
4. Behavioral inspection of the upstream source dispatch and operation code.
5. Side-by-side GNU Screen sessions at 80x24, 100x30, 120x34-40,
   72x22, and resized layouts.
6. A ten-topic matrix of 115 matched hardcopy pairs, with raw captures and
   per-topic findings retained as audit artifacts.
7. Review of mcpp source plus Debug, Release, ASan, and UBSan test matrices.

MC source was inspected only to understand behavior. No MC code or resources
were reused.

## What mcpp currently does

The following local functionality works in basic cases:

- two directory panes with `..`, file metadata, free space, and hidden files;
- keyboard and mouse pane focus;
- cursor movement, page movement, history, sorting, and hotlist;
- marking, masks, inversion, and selected-item operations;
- F2 user menu and F9 category pull-down state;
- F3 text/hex viewer;
- F4 basic text editor with save conflict checks and undo/redo;
- F5 copy, F6 move/rename, F7 mkdir, and F8 delete;
- local symbolic links, hard links, chmod, and chown;
- asynchronous copy/move/delete with status updates and cancellation;
- recursive filename/content search and directory comparison;
- quick view and file information;
- local shell commands and an interactive child shell;
- atomic regular-file output, root deletion protection, and symlink
  confinement;
- Debug, Release, ASan/UBSan, pseudo-terminal, and Docker tests.

These are useful foundations, not parity.

## Correctness issues in the current implementation

### Blockers

1. **Dangling capture in asynchronous collision handling**

   `src/app.cpp` constructs the operation collision callback with `[&]`,
   capturing the stack parameter `overwrite` by reference, then copies that
   callback into a worker thread. If a collision is discovered after the
   preflight check, invoking the callback reads a dead stack reference.
   This is undefined behavior.

2. **Worker can outlive the terminal object**

   Background threads store and call a raw `ftxui::App*`. F10 blocks normal
   exit while an operation is running, but signals, terminal failures, or
   exceptions can end the event loop first. The local FTXUI object is then
   destroyed before the `Impl` and its `std::jthread`, allowing a late
   `terminal->Post()` through a dangling pointer.

3. **Unsupported special files can block indefinitely**

   Local copy distinguishes directories and symlinks, then treats every other
   entry as a regular stream. Opening a FIFO for input can block indefinitely;
   sockets, devices, and other special files have undefined or misleading
   behavior. MC identifies these types and applies explicit policies.

4. **Operation UI is not an operation controller**

   The status line cannot skip the current item, pause/continue, retry an
   error, ignore one/all errors, or inspect failures. Cancellation is
   all-or-nothing. This is unsafe for large multi-item operations because the
   user cannot resolve an individual failure.

### High priority

5. **Copy/move metadata semantics are incomplete**

   Regular copies preserve mode only. They do not preserve timestamps,
   ownership, xattrs, ACLs, sparse layout, hard-link relationships, filesystem
   attributes, or stable relative symlinks. There is no follow-links option,
   dive-into-subdirectories option, reflink/preallocation behavior, or umask
   policy.

6. **Overwrite behavior is far below MC**

   mcpp offers a whole-operation yes/no overwrite confirmation. MC offers
   per-file Yes, No, Append, Reget, All, Older, None, Smaller, Size differs,
   zero-length protection, and Abort, with both source and destination
   metadata shown.

7. **User-menu loading is not secure or compatible**

   MC checks ownership and world-writability of local and user menu files.
   mcpp uses a different one-line format and does not validate ownership or
   permissions. Placeholder replacement is sequential, so a filename
   containing `%d` or `%D` can be rewritten after `%f` expansion, corrupting
   quoting and command meaning.

8. **Configuration writes are not hardened**

   Configuration uses a predictable `.tmp` pathname through `std::ofstream`.
   It does not use exclusive no-follow creation, fsync, mode enforcement, or
   robust replacement/rollback.

9. **Provider operations still run synchronously on the UI thread**

   Directory refresh, collision preflight, recursive search, compare, viewer
   range loads, quick view, stat, and free-space queries can block rendering.
   This is already visible with large or slow local directories and would be
   unacceptable for Blob or SSH providers.

10. **Terminal and operation lifetimes are not modeled explicitly**

    There is no state machine that owns operation start, progress, conflict
    prompts, cancellation, completion, terminal shutdown, and worker join.
    UI state and worker state are coupled inside a 2,500-line `app.cpp`.

### Medium priority

11. `getpwuid()` and `getgrgid()` are non-reentrant but can be reached while
    worker operations and UI refreshes overlap.
12. Undo always marks the editor dirty, even when undo returns to the saved
    state.
13. UTF-8 editing is byte-aware but not grapheme- or display-column-aware.
14. UI truncation counts bytes, not terminal cell width.
15. Directory comparison supports one fixed size/time strategy and has no
    thorough byte comparison.
16. Search has no regex, whole-word, ignored-directory, pause/resume, or
    panelize semantics.
17. A dead legacy internal-menu handler remains in `app.cpp`, separate from
    the active user-menu implementation.
18. Cross-device move rollback and destination backup cleanup are not
    failure-injection tested.

## Behavioral comparison

### Main screen and panels

| Capability | MC | mcpp | Status |
|---|---|---|---|
| Equal dual panes | Yes | Yes | Working |
| Parent row | Yes | Yes | Working |
| Full Name/Size/Time view | Yes | Yes | Basic |
| Brief view | 1-9 columns | One column | Partial |
| Long `ls -l` view | Yes | No | Missing |
| User-defined panel format | Yes | No | Missing |
| Sort modes | 8 plus unsorted/natural options | Name, extension, size, mtime | Partial |
| Reverse sorting | Yes | Yes | Working |
| Panel filters | Glob/regex, files-only, case options | Case-sensitive glob filter | Partial |
| Quick search | Incremental panel search | Incremental Ctrl-S/Alt-S search | Working |
| Directory sizes | On demand | Shows directory inode size | Missing |
| Tree panel | Dynamic/static global tree | Lazy global tree with current-directory ancestry, async expansion, mouse/keyboard navigation, and explicit collapse controls | Close |
| Info panel | Full panel mode | Live metadata panel | Close |
| Quick-view panel | Viewer-capable panel | Fixed-width preview with Tab/mouse focus and scrolling | Partial |
| Panelize | Find/external command results become panel | Asynchronous cancellable name results become panel | Partial |
| Horizontal split and resizing | Yes | No | Missing |
| Swap/synchronize panes | Yes | No | Missing |
| File highlighting | Rules, types, permissions | Built-in grayscale type/mode cues | Partial |
| Codepage selection | Yes | No | Missing |
| Tiny terminal adaptation | Truncates fields/keybar intentionally | Compact fields and keybar | Working |

At 60x15, both programs keep panel navigation and current-item information
usable. mcpp keeps all ten compact key labels visible, while MC drops later
labels when necessary.

The follow-up source and side-by-side review aligned Ctrl-PageUp/Down
directory traversal, parent-row cursor restoration,
Alt-Y/Alt-U history movement, Shift-Up/Down marking, pointed-pane mouse
wheel behavior, Space command-line input, and incremental quick search.
Remaining navigation differences are MC's directory-history popup,
multi-column horizontal movement, richer panel-filter options,
directory-size calculation, and extension/executable Enter actions.

### Menus and input widgets

| Capability | MC | mcpp | Status |
|---|---|---|---|
| F2 user menu | Local/user/system lookup, conditions, multiline commands | One global `key|label|command` file | Partial |
| F9 pull-down | Full Left/File/Command/Options/Right menus | Matching menu rows with implemented actions enabled and unavailable rows faded | Partial |
| Dialog widgets | Buttons, checkboxes, radio, tab order, hotkeys | Buttons, checks, radio-like modes, tab order | Partial |
| Input editing | Cursor, words, kill/yank, selection, history, completion | Append/backspace only | Missing |
| Mouse in menus/dialogs | Yes | Panel rows and commander pull-downs; editor/dialog mouse incomplete | Partial |
| Hypertext help | Contextual, indexed, links/history | Static key summary | Missing |
| Learn/redefine keys | Yes | No | Missing |

### Copy, move, and delete

The side-by-side F5 test demonstrates the gap directly.

MC's copy dialog includes:

- source and destination masks;
- shell-pattern mode;
- follow links;
- preserve attributes;
- stable symlinks;
- dive into existing subdirectories;
- foreground/background choice;
- OK and Cancel buttons.

mcpp's dialog contains only a destination text field.

Further missing operation behavior:

- per-file conflict decisions and apply-to-all variants;
- skip, suspend/continue, retry, ignore, and ignore-all;
- current-file and total progress bars;
- accurate recursive totals before operation;
- background job list with stop/restart/kill;
- recursive delete choices Yes/No/All/None/Abort;
- source/target mask transforms and case conversion;
- successful-only unmarking;
- disk-full recovery and resume/reget;
- external filesystem and special-file policies.

### Viewer

| Capability | MC viewer | mcpp viewer |
|---|---|---|
| Dedicated full-screen UI/keybar | Yes | Yes; unavailable actions are faded |
| File offset/size/percentage | Yes | Displayed end, size, and percentage |
| Wrap | Yes | Yes |
| Hex view | Yes | Basic |
| Hex editing/save | Yes | No |
| Goto offset/line/percent | Yes | No |
| Forward/backward search and continue | Yes | One forward search |
| Raw/nroff modes | Yes | No |
| Ruler | Yes | No |
| Bookmarks | Yes | No |
| Next/previous file | Yes | No |
| Codepages | Yes | No |
| Viewer history/screen selector | Yes | No |
| Horizontal scrolling | Yes | No |

The mcpp viewer uses the same full-screen shape as `mcview`: a status line,
file content, and a viewer-specific function-key bar. Goto, raw/nroff,
formatting, bookmarks, and horizontal scrolling remain unavailable.

### Editor

This comparison used `/usr/bin/mcedit` from MC 4.8.30, its installed manual
and default keymap, every pull-down menu, and paired pseudo-terminal editing
checks. The current mcpp editor now looks like mcedit at first glance, but it
is not behaviorally equivalent.

What is already close:

- a dedicated full-screen editor with an editor-specific header and keybar;
- a character cursor, UTF-8 code-point movement, and horizontal scrolling;
- persistent linear F3/Shift-arrow marking and F5/F6/F8 block operations;
- insert/overwrite modes and mcedit-compatible core navigation keys;
- F7 literal search, F4 single replacement, save-as, and goto-line;
- undo/redo, atomic saves, external-change detection, and dirty-close
  confirmation;
- regular-file symlink editing that updates the target without replacing the
  link;
- visible save errors and readable narrow-terminal rendering.

#### File and window lifecycle

| Capability | mcedit | mcpp | Gap |
|---|---|---|---|
| Existing file | Yes | Yes | Close |
| New/open another file | File menu and `Ctrl-N` | Single-buffer File menu and `Ctrl-N` | Partial |
| Multiple files/windows | Multiple movable/resizable windows | One buffer | Missing |
| File history | Persistent history dialog | Persistent 20-file history | Close |
| Save/save-as | Confirmation, overwrite handling, save modes | Save and no-clobber save-as | Partial |
| Insert/copy block to file | Yes | No | Missing |
| Concurrent-edit lock | Lock file and user decision | Revision conflict detected only at save | Partial |
| Backups/recovery | Configurable save modes and recovery support | Atomic replacement only | Partial |
| Large-file handling | Warns at 64 MiB and offers to continue | Hard rejection above 16 MiB | Different |
| Binary/non-printable editing | Supported | Bytes load, but display/edit behavior is incomplete | Incorrect |

The mcpp safety baseline is good: writes are atomic, stale revisions are
rejected, and symlink targets are resolved once when opened. It does not yet
offer mcedit's save-mode choices, lock interaction, or recovery workflow.
Multiple open buffers can be listed and switched from the Window menu.

#### Editing and navigation

| Behavior | mcedit | mcpp |
|---|---|---|
| Insert/overwrite | `Insert` toggles mode | `Insert` toggles mode |
| Word movement | `Ctrl-Left/Right`, `Ctrl-Z/X` | Same core bindings |
| File top/bottom | `Ctrl-Home/End` | Same |
| Delete word/line/to-EOL | Dedicated bindings | Line and EOL; word deletion missing |
| Desired column on vertical move | Preserved across short lines | Preserved across short lines |
| Page Up/Down | Uses the visible editor height | Uses the visible editor height | Close |
| Tab handling | Configurable real/fake tabs and tab width | Configurable space indentation stops |
| Existing tabs | Configurable width | Configurable width |
| Auto-indent | Configurable | Configurable |
| Literal insertion | `Ctrl-Q` | No |
| Completion | `Alt-Tab` | No |
| Matching bracket | `Alt-B` | Same |
| Paragraph formatting/sort/filter | Yes | No |

Paired checks now produce the same results for overwrite insertion,
Shift-arrow marking with F8 deletion, `Ctrl-U` undo, `Alt-R` redo, word
movement, and viewport-sized Page Up/Down.

#### Selection and clipboard

mcpp implements one persistent linear F3/Shift-arrow selection and can copy,
move, delete, or repeatedly copy it. Rectangular F13/Alt-arrow blocks support
copy, move, delete, rendering, and clipboard operations. Typing and ordinary
character deletion do not consume a linear block. mcedit additionally
supports:

- word, file-boundary, and scrolling selection variants beyond the basic
  Shift-arrow/Home/End/Page bindings;
- line/word marking and block indentation;
- mouse drag block selection.

mcpp supports copy/cut/paste through `Ctrl-Insert`, `Shift-Delete`, and
`Shift-Insert` and safely persists both clip text and column-mode metadata in
a mode-0600 atomic cache file across processes.

#### Search and replace

| Capability | mcedit | mcpp |
|---|---|---|
| Forward literal search | Yes | Yes |
| Case-sensitive option | Yes | Yes |
| Backward search | Yes | Yes |
| Regular expression/wildcard/hex | Yes | Yes |
| Whole words/in selection | Yes | Yes |
| All-charsets search | Yes | No |
| Search again | `F17` | Yes |
| Find all | Results action | Match count |
| Failed-search feedback | Dialog | Dialog |
| Interactive replace choices | Replace/skip/all/one | Yes, literal search |
| Replace again | `F14` | No |

mcpp's F4 flow provides Normal, Regular expression, Wildcard, and Hexadecimal
modes plus case, direction, whole-word, and selection-scope options, followed
by Replace, Skip, All, One, and Cancel decisions. Remaining differences are
all-charsets search, a navigable Find All results list, and F14 replace-again.

#### Key compatibility

The core bindings now align. Two default mcedit actions remain unavailable:

| Action/key | mcedit | mcpp |
|---|---|---|
| `Ctrl-F` | Save block to file | Unavailable notice |

Esc, undo/redo, goto-line, insert/overwrite, word navigation, and file
top/bottom now use mcedit's default bindings. `Ctrl-S` toggles built-in syntax
highlighting. `Ctrl-F` remains reserved for an unimplemented mcedit action
rather than silently performing the old conflicting search action.

#### Menus, status, and options

mcpp shows the same seven top-level editor menu names. File, Edit, and Search
contain a working subset, and Command can toggle syntax highlighting. Window
supports next/previous/list buffer actions; Format remains a placeholder.
Options provides general indentation and
whitespace settings plus persistent Save Setup. mcedit additionally exposes
insert/copy file,
bookmarks, syntax selection and encoding, macros, spell checking, literal
insertion, formatting, external filters, richer window management,
options, save modes, and key learning. mcpp editor menus also lack mouse item
activation and menu hotkeys.

The mcedit header reports insert/overwrite state, selection size, line and
column details, total lines/bytes, character code, and window state. mcpp
reports path, modified/overwrite state, line/column, selection/cursor/total
bytes, and Unicode character value. It provides
built-in VS Code Dark+-style highlighting for common C-family, Rust, Go,
JavaScript, Python, shell, JSON, Markdown, and configuration files, but no
external rule database or syntax chooser. Line numbers are toggleable with
Alt-N. Visible tabs and trailing spaces are configurable, and Tab uses the
fake-indent width in leading indentation and the real tab width after text.
Bookmarks, codepages, spell checking, macros, and session recovery are absent.

The next editor work should focus on mouse drag selection and richer
multi-window commands. Macros, spell checking, and code navigation are later
parity work, not requirements for a dependable basic editor.

### Command line and subshell

mcpp can run `/bin/sh -c` and launch an interactive child shell, but it does
not implement MC's synchronized persistent subshell.

Missing behavior:

- prompt/input cursor editing and word operations;
- shell/file/variable/user/host completion;
- command history dialog and persistent history;
- path/file insertion shortcuts;
- `cd` internal semantics and CDPATH handling;
- macro substitution used by menus and extension handlers;
- pause-after-run modes;
- panel toggle showing previous command output;
- shell directory synchronization when returning from Ctrl-O;
- screen/session selector.

### Search, tree, panelize, and compare

- Search lacks regex/shell-pattern selection, whole words, ignored
  directories, pause/resume, repeated search, and content-result context.
- Search results are not a persistent panel mode and cannot be restored.
- External panelize and saved panelize commands are absent.
- Directory tree view and tree maintenance are absent.
- Compare has no quick/thorough/size-only choice and no visual diff viewer.
- File diff/merge and dynamic diff editing are absent.

### Configuration and customization

Missing:

- complete configuration, layout, panel, confirmation, appearance, key
  learning, and VFS dialogs;
- custom keymap loading;
- skins and shadows;
- arbitrary listing formats;
- extension handlers/file associations;
- filename highlight rules;
- per-panel persisted settings;
- locale/i18n;
- appropriate terminal capability handling beyond FTXUI defaults;
- XTerm title updates;
- multiple simultaneous viewer/editor screens.

### VFS and archives

Only the local provider is enabled. All of the following MC capabilities are
missing or intentionally paused:

- tar/archive VFS;
- extfs archive/package adapters;
- FTP;
- SFTP;
- shell/FISH VFS;
- undelete VFS;
- VFS cache controls;
- Azure Blob provider and direct Blob-to-Blob operations.

## Test gaps

Current tests are valuable but narrow. Missing test coverage includes:

- operation conflict races and the dangling callback path;
- SIGINT/SIGTERM while worker operations are posting UI events;
- FIFOs, sockets, devices, sparse files, xattrs, ACLs, and hard-link sets;
- disk full, permission changes during operation, I/O errors, and rollback;
- very large directories and millions of entries;
- tiny terminals, resize storms, tmux/Screen variants, mouse sequences, and
  Unicode display widths;
- every F-key dialog and pull-down action;
- background pause/retry/skip behavior;
- viewer/editor keymap conformance;
- command completion and subshell directory synchronization;
- differential behavioral tests against MC;
- fuzzing for paths, menu expansion, config parsing, terminal events, and
  provider responses.

## Recommended order

1. **Stop feature expansion and fix correctness blockers.**
   Remove dangling captures, define terminal/worker ownership, reject or
   support special files, harden config/menu files, and add failure injection.
2. **Build reusable input, dialog, and operation controllers.**
   Current one-field prompts cannot support MC behavior.
3. **Complete local panel semantics.**
   Listing formats, filters, quick search, tree, panelize, layout, resizing,
   and small-terminal behavior.
4. **Complete file-operation semantics.**
   Masks, metadata, hardlinks, sparse files, overwrite choices, retries,
   pause/continue, background jobs, and detailed progress.
5. **Separate and deepen viewer/editor applications.**
   Give each its own state, keybar, menus, tests, and full-screen lifecycle.
6. **Implement persistent subshell and command-line editing.**
7. **Add configuration/help/customization.**
8. **Only then resume archive, SSH, or Blob providers.**

The Azure Blob integration should not start until local operation semantics and
worker lifetime are stable; otherwise remote latency and failure modes will
amplify the current problems.
