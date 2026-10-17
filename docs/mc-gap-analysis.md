# mcpp versus GNU Midnight Commander: gap analysis

Review date: 2026-10-16

Compared versions:

- GNU Midnight Commander 4.8.30, installed at `/usr/bin/mc`
- mcpp commit `60a821e`

## Conclusion

mcpp is an early local-filesystem prototype with a recognizable orthodox
two-pane layout. It is not yet a functional clone of GNU Midnight Commander.

The default panel screen is now visually close at 120x35, but most MC depth is
missing behind that screen. The largest gaps are file-operation semantics and
dialogs, panel modes, input editing, viewer/editor behavior, persistent
subshell integration, configuration, help, jobs, archive/VFS support, and
failure handling.

mcpp should not currently be presented as having broad MC parity.

## Review method

The review used:

1. MC's installed binary, help, configure flags, and VFS installation.
2. MC's default keymap: 201 unique action names and 393
   context/action combinations across file manager, panel, input, menu,
   dialog, tree, help, editor, viewer, hex viewer, and diff viewer contexts.
3. MC's manual sections for panels, menus, operations, search, hotlist,
   options, viewer, editor, subshell, and VFS.
4. Behavioral inspection of the upstream source dispatch and operation code.
5. Side-by-side pseudo-terminal sessions at 120x35 and 60x15.
6. Direct comparisons of the default screen, F5 copy dialog, F2/F9 menus,
   F3 viewer, and editor behavior.
7. Review of mcpp source, unit tests, sanitizer runs, and Docker tests.

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
| Panel filters | Glob/regex, files-only, case options | No | Missing |
| Quick search | Incremental panel search | Find-next prompt/menu only | Partial |
| Directory sizes | On demand | Shows directory inode size | Missing |
| Tree panel | Dynamic/static tree | No | Missing |
| Info panel | Full panel mode | Small modal | Partial |
| Quick-view panel | Viewer-capable panel | Read-only preview | Partial |
| Panelize | Find/external command results become panel | Separate results screen | Missing |
| Horizontal split and resizing | Yes | No | Missing |
| Swap/synchronize panes | Yes | No | Missing |
| File highlighting | Rules, types, permissions | Focus/mark only | Missing |
| Codepage selection | Yes | No | Missing |
| Tiny terminal adaptation | Truncates fields/keybar intentionally | Keybar and fields become unreadable | Incorrect |

At 60x15, MC keeps usable panels and truncates the keybar. mcpp concatenates
key labels and clips paths/status without a minimum-size mode.

### Menus and input widgets

| Capability | MC | mcpp | Status |
|---|---|---|---|
| F2 user menu | Local/user/system lookup, conditions, multiline commands | One global `key|label|command` file | Partial |
| F9 pull-down | Full Left/File/Command/Options/Right menus | Small subset | Partial |
| Dialog widgets | Buttons, checkboxes, radio, tab order, hotkeys | One text field and Enter/Esc | Missing |
| Input editing | Cursor, words, kill/yank, selection, history, completion | Append/backspace only | Missing |
| Mouse in menus/dialogs | Yes | Top menu and panel rows only | Partial |
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
| Dedicated full-screen UI/keybar | Yes | Main commander chrome remains |
| File offset/size/percentage | Yes | No |
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

The mcpp viewer currently renders inside a rounded box and retains the main
menu, hint, prompt, and file-manager keybar, unlike MC's dedicated viewer.

### Editor

This comparison used `/usr/bin/mcedit` from MC 4.8.30, its installed manual
and default keymap, every pull-down menu, and paired pseudo-terminal editing
checks. The current mcpp editor now looks like mcedit at first glance, but it
is not behaviorally equivalent.

What is already close:

- a dedicated full-screen editor with an editor-specific header and keybar;
- a character cursor, UTF-8 code-point movement, and horizontal scrolling;
- linear F3 marking and basic F5/F6/F8 block operations;
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
| New/open another file | File menu and `Ctrl-N` | No editor action | Missing |
| Multiple files/windows | Multiple movable/resizable windows | One buffer | Missing |
| File history | Persistent history dialog | No | Missing |
| Save/save-as | Confirmation, overwrite handling, save modes | Save and no-clobber save-as | Partial |
| Insert/copy block to file | Yes | No | Missing |
| Concurrent-edit lock | Lock file and user decision | Revision conflict detected only at save | Partial |
| Backups/recovery | Configurable save modes and recovery support | Atomic replacement only | Partial |
| Large-file handling | Warns at 64 MiB and offers to continue | Hard rejection above 16 MiB | Different |
| Binary/non-printable editing | Supported | Bytes load, but display/edit behavior is incomplete | Incorrect |

The mcpp safety baseline is good: writes are atomic, stale revisions are
rejected, and symlink targets are resolved once when opened. It does not yet
offer mcedit's save-mode choices, lock interaction, recovery, or multi-file
workflow.

#### Editing and navigation

| Behavior | mcedit | mcpp |
|---|---|---|
| Insert/overwrite | `Insert` toggles mode | Insert only |
| Word movement | `Ctrl-Left/Right` | No |
| File top/bottom | `Ctrl-Home/End` | No |
| Delete word/line/to-EOL | Dedicated bindings | Character/backspace only |
| Desired column on vertical move | Preserved across short lines | Column is clamped and lost |
| Page Up/Down | Uses the visible editor height | Uses the visible editor height | Close |
| Tab handling | Configurable real/fake tabs and tab width | Always inserts four spaces |
| Existing tabs | Configurable width | Displayed at fixed four-cell stops |
| Auto-indent | Configurable | No |
| Literal insertion | `Ctrl-Q` | No |
| Completion | `Alt-Tab` | No |
| Matching bracket | `Alt-B` | No |
| Paragraph formatting/sort/filter | Yes | No |

Live checks confirmed visible differences:

- `Insert`, then `X` over `abc` produces `Xbc` in mcedit and `Xabc` in
  mcpp.
- `Shift-Right` twice, then F8 leaves `c` in mcedit and `bc` in mcpp because
  mcpp does not implement Shift-arrow marking.
- `Ctrl-U` undoes in mcedit; mcpp currently binds undo to `Ctrl-Z`.

#### Selection and clipboard

mcpp implements one linear F3 selection and can copy, move, or delete it.
Unlike mcedit's persistent block, mcpp consumes the selection when text is
typed or F5 is used, so a block cannot be copied repeatedly without marking it
again. mcedit additionally supports:

- Shift-arrow, word, page, file-boundary, and scrolling selection;
- rectangular column blocks (`F13` and Alt-arrow bindings);
- mark-all, unmark, line/word marking, and block indentation;
- persistent clip-file copy/cut/paste through `Ctrl-Insert`,
  `Shift-Delete`, and `Shift-Insert`;
- mouse positioning and mouse block selection.

mcpp has no reusable clipboard or paste action. Its internal scratch string is
used only while a selected block is copied or moved. It has no standard
terminal-editor selection bindings or persistent clip file.

#### Search and replace

| Capability | mcedit | mcpp |
|---|---|---|
| Forward literal search | Yes | Yes |
| Case-sensitive option | Yes | No UI |
| Backward search | Yes | No |
| Regular expression/wildcard/hex | Yes | No |
| Whole words/selection/all charsets | Yes | No |
| Search again | `F17` | No |
| Find all | Results action | No |
| Failed-search feedback | Dialog | Dialog |
| Interactive replace choices | Replace/skip/all/one | No |
| Replace again | `F14` | No |

mcpp's F4 flow asks for two strings and immediately replaces one
case-insensitive match, wrapping to the start of the file. That is materially
different from mcedit's option dialog and per-match replacement flow.

#### Key compatibility

Several mcpp shortcuts conflict with mcedit's default keymap:

| Action/key | mcedit | mcpp |
|---|---|---|
| `Esc` | Quit/close | Clear selection only |
| Undo | `Ctrl-U` | `Ctrl-Z` |
| Redo | `Alt-R` | `Ctrl-Y` |
| Goto line | `Alt-L` | `Ctrl-G` |
| `Ctrl-S` | Toggle syntax highlighting | Save |
| `Ctrl-F` | Save block to file | Search |
| `Insert` | Toggle insert/overwrite | No action |

The F1-F10 labels are now similar, but matching labels are not sufficient
while common keyboard habits produce different actions.

#### Menus, status, and options

mcpp shows the same seven top-level editor menu names. Only File, Edit, and
Search contain a small working subset; Command, Format, Window, and Options
are placeholders. mcedit exposes open/new/history, insert/copy file, clip-file
operations, bookmarks, syntax and encoding, macros, spell checking, literal
insertion, formatting, external filters, multiple windows, general editor
options, save modes, and key learning. mcpp editor menus also lack mouse item
activation and menu hotkeys.

The mcedit header reports insert/overwrite state, selection size, line and
column details, total lines/bytes, character code, and window state. mcpp
reports path, modified state, line/column, and bytes only. Line numbers,
whitespace markers, syntax highlighting, bookmarks, codepages, spell
checking, macros, and session recovery are absent.

The next editor work should first align default keys, Esc behavior,
Shift-arrow selection, overwrite mode, word/file navigation, and the
search/replace flow. File lifecycle and multi-buffer work come next. Syntax,
macros, spell checking, and code navigation are later parity work, not
requirements for a dependable basic editor.

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
