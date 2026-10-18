# mcpp versus GNU Midnight Commander

Review date: 2026-10-17

This is a functional review of the supported Linux local-filesystem workflows,
not a claim that mcpp is a complete MC clone. Archive, SSH/SFTP, Azure, and
spelling support remain deferred.

## Source audit of the current implementation

The source comparison uses mcpp code at `14b7a82` and MC reference commit
`b2706063b0e3abe9b7a6b7edef0a63e512dfb28c` (2026-10-16; its NEWS identifies
the 4.8.33 series). The installed runtime control is MC 4.8.30, not a build
of that reference commit. Reference-version changes must therefore not be
inferred from the older binary alone.

MC paths below are relative to `reference/mc/`; mcpp paths are relative to
this repository. This is an audit, not a claim that the findings are fixed.
Uncommitted, unbuilt shell work is not counted as an implemented feature.
MC implementation and resources were inspected, not copied into mcpp.

The default keymap has 393 action entries across 15 sections. That inventory
is a checklist, not a test-pass count: multiple sections share actions, and
one reachable action can still have incorrect behavior. The findings below
come from tracing the relevant implementation paths, with bounded runtime
reproductions for the most important cases.

### Behavioral bugs and incomplete workflows

P1 means unexpected file/metadata changes, misleading synchronization choices,
or failure of operation controls. P2 means a broken or incomplete normal
workflow. Source-only findings are distinguished from reproduced outcomes.

| ID | Priority | Finding and consequence | MC evidence | mcpp evidence / verification |
|---|---|---|---|---|
| A01 | P1 | Following a directory symlink back to an ancestor has no source-cycle guard. Copy can repeatedly reproduce the same subtree until cancellation or an error. | `src/filemanager/file.c:3148-3160` rejects cyclic source links using ancestor identity. | `src/operations.cpp:608-650` recursively follows links without an ancestor set. A bounded probe created 18 repeated `self` directories before the probe cancelled at 20 callbacks. |
| A02 | P1 | Copies lose source modification times. Backup results and later older/newer decisions are consequently wrong. | `src/filemanager/file.c:3020-3022,3332` restores timestamps. | `src/operations.cpp:512-726` and `src/local_provider.cpp:967-1073` restore permissions, not times; the provider has no time-setting API. Reproduced: source timestamp from 2000 became the copy time. |
| A03 | P1 | Saving a hardlinked file silently detaches that pathname. Other linked names retain the old bytes, without a choice or warning. | `src/editor/editcmd.c:174-191` offers detach, retain-link behavior, or Cancel. | `src/editor.cpp:1548-1580`, `src/local_provider.cpp:664-800` always publish a replacement. Reproduced: edited name contained `Xoriginal`, linked name still contained `original`, and their inodes differed. |
| A04 | P1 | The generic copy path provides no mid-file progress/control callbacks. Pause and Skip can fail to take effect during that file; ordinary cancellation still has separate checks. | `src/filemanager/file.c:2741,2854` checks operation buttons during copying. | `src/operations.cpp:684-708` stages the source and writes with an empty progress callback; `:726` only reports after publication. A 256 KiB copy produced only two callbacks and no intermediate callback. Applies when direct-copy conditions are not met, including Preserve permissions off. |
| A05 | P1 | Compare directories marks both versions of a differing file, including the older copy, and includes directories. Using those marks for synchronization can select the wrong source. | `src/filemanager/cmd.c:211-303` skips directories and does not mark an older source. `:1001-1017` offers Quick, Size only, and Thorough. | `src/compare.cpp:83-107`, `src/app.cpp:5020-5059` produce undirected differences and mark both panes. Reproduced with equal-size files where only the left copy was newer. |
| A06 | P2 | Copying with Preserve permissions off creates a new regular file as 0600 rather than applying the normal creation mode and umask. | `src/filemanager/file.c:2978-2983` uses 0666 filtered by umask. | `src/local_provider.cpp:701-705` defaults new output to owner read/write; the generic copy path leaves that mode. Reproduced with a 0755 source producing a 0600 destination. |
| A07 | P2 | Find's filename matcher only understands `*` and `?`; character classes, brace alternatives, and escaping do not match MC glob behavior. | `lib/search/glob.c:50-126`, `src/filemanager/find.c:1702-1707`. | `src/search.cpp:23-66`. Reproduced: `[ab]*.txt` returned zero results for `a.txt`, `b.txt`, and `c.txt`. |
| A08 | P2 | A commander command such as `cd child` reports exit 0 but leaves the pane unchanged. `exit` also exits only the temporary child shell. | `src/filemanager/command.c:159-185` handles plain cd and exit within the file manager. | `src/app.cpp:2890-2973` runs every command in a child. Reproduced using the final directory written by `--printwd`. |
| A09 | P2 | Default Tab and indentation Backspace change different bytes from mcedit. Two indent Tabs remain eight spaces instead of becoming a tab; Backspace deletes one space instead of the half-tab. | `src/editor/edit.c:81-85,1488-1510,3588-3605`. | `src/app.cpp:8391-8413`, `src/editor.cpp:1074-1097`. Reproduced: MC saved `09` for two Tabs and nothing after Tab/Backspace; mcpp saved eight and three spaces respectively. |
| A10 | P2 | Editor-relative path completion searches the pane directory, while accepting the path resolves it relative to the edited file. Opening a file outside the pane can therefore offer names from the wrong directory. | Editor file inputs resolve relative paths in the edited file's directory; see `src/editor/editcmd.c:143-151,959-1020`. | `src/app.cpp:3080-3098` resolves against the editor path, but `:6555-6558` supplies `active().path()` to completion. Source-confirmed. |
| A11 | P2 | Selection is only partially wired: Ctrl-Shift word/document/window/scroll selection and Alt-Page column selection are missing. Shift/Ctrl-Enter's unindented Return and Tab variants are also absent. | `misc/mc.default.keymap:259-389`, `src/editor/edit.c:3440-3560,3644-3690`. | `src/app.cpp:8025-8143,8347-8423` implements only a subset of those events. A menu entry or basic Shift-arrow support does not establish full selection parity. |
| A12 | P2 | Viewer input and mode behavior are partial. Default `q`, `n`, Space, alternate movement and half-page controls are not wired; F2 still toggles wrap in hex mode instead of entering hex editing. | `misc/mc.default.keymap:396-476`, `src/viewer/actions_cmd.c:385-537`. | `src/app.cpp:6572-6660`; `include/mcpp/viewer.hpp` has no hex-edit/save API. Source-confirmed. |
| A13 | P2 | Viewer repeat search wraps without MC's continue-from-beginning decision. Viewer, Find, and pane quick-search case folding is byte-based and misses non-ASCII case pairs. | `src/viewer/search.c:330-346`; `lib/search/regex.c:790-823` uses UTF-8-aware matching. | `src/viewer.cpp:16-25,463-565`, `src/search.cpp:12-20`, `src/pane.cpp:425-465`. Editor search's UTF-8 correction did not fix these other search paths. |
| A14 | P2 | Save As cannot confirm replacement of an existing file and lacks MC's line-break conversion options. Direct `--edit` of a nonexistent pathname fails instead of creating a new buffer. | `src/editor/editcmd.c:959-1058` handles overwrite; `src/editor/edit.c:292-318` creates a new file. | `src/editor.cpp:1582-1610`, `src/app.cpp:3091-3097,10132-10145`. Source-confirmed; these are distinct from the F2 confirmation already fixed. |
| A15 | P2 | Initial copy/delete measurement errors bypass the new Retry/Skip controller. Find aborts the entire traversal on an unreadable directory instead of continuing with accessible results. | `src/filemanager/file.c` has per-file error decisions; `src/filemanager/find.c:1250-1318` continues seeking an accessible directory. | `src/operations.cpp:266-314,738-740,1042-1047`; `src/search.cpp:138-148` and `src/app.cpp:1758-1848`. Source-confirmed. |
| A16 | P2 | Escape closes only the active editor buffer, whereas MC maps Escape and F10 to Quit. With multiple buffers the two keys therefore have different scope in mcpp. | `misc/mc.default.keymap` editor Quit binding; `src/editor/editwidget.c:444-455`. | `handle_editor()` in `src/app.cpp` calls different close/quit functions. This is documented mcpp behavior, but not an established user-approved MC deviation. |

### Missing local capabilities

These are capability gaps, not evidence that already implemented operations fail.
They remain open work rather than being treated as acceptable merely because an
entry is disabled.

| Area | Missing or partial behavior | Source anchors |
|---|---|---|
| Copy/move metadata and decisions | Preserve hardlink sets across a copied group; ownership where permitted; remaining metadata options; append/reget, name-transform masks, stable links, dive-into-subdirectory choices and recursive-delete decisions. The copied hardlink pair was reproduced as two independent inodes. | MC `src/filemanager/file.c:359-480,2413-2428`; `src/filemanager/filegui.c:1375-1407`; mcpp `include/mcpp/operations.hpp`, `src/app.cpp:5200-5270`. |
| Commander/file actions | Enter does not execute an executable file or use an Open association. Shift-F3/F4/F5/F6/F8 raw/new/single-item actions and pathname/tagged-name insertion are not wired. Bulk chmod/chown choices are absent. | MC `src/filemanager/panel.c:2860-2930,3560-3581`; `src/filemanager/filemanager.c:646-775`; mcpp `src/pane.cpp:189-207`, `src/app.cpp:9780-9990`. |
| Panels and Tree | Listing formats, multi-column/custom fields, split/resizing, directory sizes, persistent tree cache and the Command-menu modal tree chooser. Requested mcpp Tree Enter/expand behavior is not itself a bug. | MC `src/filemanager/boxes.c`, `layout.c`, `treestore.c:211-376,631-662`; mcpp panel menu and in-memory Tree state. |
| Find, panelization and compare | Find content/regex/options/ignore directories, search-result panelization, external-command panelization, compare-method selection, and file diff/merge. The dormant content-search API also bypasses content filtering for non-file entries and needs correction before exposure. | MC `src/filemanager/find.c:564-820,1694-1707`, `panelize.c`, `src/diffviewer/ydiff.c`; mcpp `src/search.cpp:150-167`, `src/app.cpp:1727-1848,4640-4700`. |
| Editor actions and persistence | Block-to-file, insert-file, bookmarks, literal insertion, completion, replace-again, navigable Find All, formatting/filter actions, file locks, backup modes, saved file positions and richer save options. The 16 MiB hard limit also differs from MC's large-file confirmation. | MC `src/editor/editmenu.c:72-205`, `edit.c:465-512,2224-2255`, `editcmd.c`; mcpp disabled editor actions, `src/editor.cpp:159-163`, path-only editor history. |
| Viewer | Goto, bookmarks, backward/regex/hex search options, raw/nroff/ruler, hex editing/save, next/previous files, history and growing/streamed sources. Standalone viewer quit returns to panels rather than exiting. | MC `src/viewer/actions_cmd.c:385-537`, `datasource.c`, `growbuf.c`; mcpp `include/mcpp/viewer.hpp`, `src/app.cpp:6572-6660,10119-10130`. |
| Shell and background work | Persistent Bash state and cwd synchronization, command execution while another operation runs, concurrent jobs and full screen switching. Current job history is only for a single active worker. | MC `src/subshell/`, `src/background.c`; mcpp `src/app.cpp:2890-2957` and operation-running guards. |
| Configuration and integration | Persistent input histories, per-pane settings, configurable keymaps/default aliases, complete options dialogs, extension handlers and MC-compatible user-menu conditions/macros. | MC `misc/mc.default.keymap`, `src/setup.c`, `src/usermenu.c`; mcpp `include/mcpp/config.hpp`, `src/config.cpp`, `src/user_menu.cpp`. |

### Do not blindly reproduce MC

- Malformed booleans are a reproduced safety hazard: `confirm_delete=tru`
  silently disables mcpp's confirmation, and the disposable file was deleted
  without a prompt (`src/config.cpp:20-22,124-129`). MC's boolean getter also
  returns the unchecked parser result (`lib/mcconfig/get.c:189-201`), so this
  is not an MC parity miss. It is still a real mcpp issue to fix.
- mcpp also silently drops automatic configuration-save failures on exit
  (`src/app.cpp:10227-10230`). Keep this in the error-reporting fix queue;
  do not infer a successful save from a clean process exit.
- Retain mcpp's safe publication, external-change checks, physical alias checks
  and successful-only unmarking. Hardlink decisions should be added without
  discarding those protections.
- Linux scope, grayscale chrome, the syntax palette, and the requested Tree
  opening/expansion behavior are established choices, not missing MC features.
- Archive/VFS, Azure, SSH/SFTP and spelling remain explicitly paused.

### Fix order and audit discipline

1. Fix file outcomes first: source cycles, timestamps/hardlinks, copy controls,
   compare direction, and invalid confirmation settings.
2. Fix ordinary interaction next: creation modes, command cd/exit, editor
   indentation and selection, path completion, save/create flows and viewer
   navigation/search.
3. Complete local capabilities in workflow-sized groups. Trace every advertised
   action through its handler, model, filesystem effects, Cancel/error path and
   persistence. A disabled label is a known gap, not completion.

Every fix should turn its finding into a small normal functional regression.
Use disposable no-host-mount containers for destructive/error cases; use actual
MC terminal controls where source defaults or state transitions need confirming.
Source inspection, compilation, and a similar-looking screenshot are different
forms of evidence and must not be counted interchangeably. Sanitizers and
optional profiling are not part of this audit.

## Earlier behavioral review

## Review method

The comparison used GNU Midnight Commander 4.8.30 and a frozen mcpp baseline
at `7af99c29378e798f00b0f110a3c94734aec47387`. The baseline executable's SHA-256
was `43c17efc44b2aed84056cc1325188e937f9d14bd1dbf96711843eac4c4533d0c`.
The corrections described below belong to the subsequent review changes.

Four independent reviews covered panels/menus, operations/search, editor, and
viewer/terminal/configuration. Tree and cross-feature checks were integrated
separately. Each terminal had private HOME, configuration, cache, and fixtures.
The four reviews recorded 88 representative case groups, including partial
comparisons and unsupported actions rather than counting those as passes.

Evidence includes rendered PNGs, ANSI/cell captures, exact keystrokes, saved
configuration bytes, and before/after filesystem metadata and hashes.
Destructive and error-injection operation checks used disposable Docker
containers without host mounts. Actual GNU Screen sessions covered both
`altscreen on` and `altscreen off`.

MC controls were calibrated before comparing results: its delayed Escape
handling, Tree static/dynamic mode, radio selection, and dialog focus differ
from simple assumptions. MC source was inspected to understand behavior;
its implementation and resources were not copied.

Screenshots were opened and inspected, not just counted. A screenshot alone
does not prove selection identity, operation targets, saved bytes, or the
absence of a late asynchronous update. The new regressions check those
outcomes directly.

## Defects addressed by this review

| Area | Baseline defect | Required corrected behavior |
|---|---|---|
| Tree | Expansion inserts rows before the cursor and changes the selected path | Retain selection by path and keep operation targets consistent |
| Tree | Rescan resets selection and expanded descendants to startup ancestry | Rescan the chosen branch, retain surviving expansions, and discover new children |
| Tree | Failed rescan retains children but marks their branch collapsed | Keep cached expansion state consistent so collapse and retry still work |
| Tree | Wide rescan merges repeatedly search the entire cached subtree | Index child subtrees once, keeping the merge linear instead of freezing the UI |
| Tree | Enter executes a pending commander command | Open/expand the Tree selection; retain pending text for the listing |
| Tree | Narrow UTF-8 names are fitted by bytes | Fit whole glyphs by display columns |
| Quick view | Home/End move the retained listing cursor | Navigate the viewer without changing the listing target |
| Prompts | Tab cannot reach Cancel; clicking Cancel does nothing | Real input/OK/Cancel focus, reverse Tab, keyboard activation, and mouse controls |
| Selection | Plain `*` also selects real directories | Invert files and directory symlinks; retain existing real-directory marks |
| Menus | View file... immediately opens the cursor file | Prompt for a filename; resolve relative paths against the active pane |
| Menus | External panelize launches a filename-glob prompt | Disable the unsupported external-command action; name the local helper accurately |
| Viewer | A tab hides subsequent content and paging skips it | Expand tabs while tracking original source bytes |
| Viewer | A 256 KiB cache boundary produces a short page and omitted bytes | Refill bounded reads through the viewport and preserve page continuity |
| Configuration | Live Save setup serializes old panel preferences | Save current panel state immediately, not only on exit |
| Editor | F6 clears the moved block mark, so F8 deletes unrelated text | Keep the destination block marked and preserve undo behavior |
| Editor | Search state leaks between open buffers | Restore each buffer's query, options, and selection scope |
| Editor | Quit behaves like closing just the active buffer | Check all dirty buffers before quitting; Cancel retains the clean active buffer and pending discard decisions |
| Editor | Case-insensitive normal search misses uppercase UTF-8 letters | Match under the UTF-8 locale and retain original byte offsets |
| Editor | Rectangular copy invents spaces for nonexistent source cells | Preserve empty source rows and pad only the destination as needed |
| Move | A single-file F6 target replaces an existing directory and deletes its contents | Treat an existing directory as a directory destination; preserve unrelated entries |
| Move | Fallback copies through a destination alias to the source and then deletes the source | Reject physical self/descendant targets and restrict fallback to supported rename failures |
| Move | Same-inode hardlink rename is reported as a completed move although the source remains | Compare `lstat` identity for regular files and symlink objects; retain both names and the mark |
| Copy | A single-file F5 filename is treated as a directory | Support an exact new filename as well as an existing directory |
| Background operations | Completion hides a dirty editor, allowing unsaved edits to be lost | Preserve the current editor/viewer/dialog and its close protection |
| Background operations | A hidden error is replaced by the editor's later status | Retain a pending failure/skip/cancellation notice and show it on return to panels |
| Background operations | A pane swap makes completion clear unrelated marks | Track the source pane after swapping and retain unrelated marks |
| Symlinks | Confirmed overwrite fails with Preserve permissions off | Replace the link safely without changing either target file |
| Panelized copy | A duplicate basename is silently skipped while reporting completion | Report skipped sources and an incomplete result explicitly |

Integration checks also cover cancelling an in-flight or queued Tree rescan,
preserving the requested rescan path while selection moves, retrying a failed
expansion, and keeping operation results visible after automatic Tree refresh.
These checks caught additional mistakes during the corrections.

## Workflows exercised

| Area | Checks and outcome boundaries |
|---|---|
| Tree | Left/right current directories, hidden cwd, Enter/open/expand, deep connectors, narrow Unicode fitting, asynchronous selection, real delete-dialog target, Tab round-trips, rescan, collapse/re-expand, permission retry, and GNU Screen resize |
| Listings | Parent cursor restoration, directory history, failed change-directory preserving marks, masks, inversion, filters, refresh, swapping, and restoring listing modes |
| Quick/Info | Preview scrolling, panel switching, mouse focus, restored cursors/marks, and File-menu operation targets |
| Menus/dialogs | Implemented File/Command routes and shortcuts; disabled actions remain visibly unavailable; generic prompt cancellation and filename entry |
| Operations | Exact bytes/modes, directory destinations, overwrite/skip policies, source masks, preserve/follow links, self-copy rejection, pause/resume/skip/abort, staging cleanup, background state, and unrelated selection preservation |
| Failure handling | Internal tmpfs disk-full, read-only destinations, cancelled deletion, symlink-only deletion, and retaining the previous destination before publication |
| Find/panelize | Nested matches, result navigation, cancellation without late publication, no traversal into symlink directories, stale-result rejection after navigation, nested refresh/filter/link actions, and duplicate basenames |
| Compare | Equal, size-mismatched, and one-sided entries; replacement rather than accumulation of marks |
| Viewer | Wrap/unwrapped paging, tabs, horizontal scroll, text/hex switching, Home/End, search/repeat, cache boundaries, empty/error states, panel return, and resize |
| Editor | UTF-8/tabs/preferred column, overwrite, undo/redo, persistent blocks, rectangular operations, cross-process clipboard, search modes/errors, selection-scoped replacement, save conflicts, symlink saves, new/save-as/close, and multiple buffers |
| Configuration | Live Save setup, fresh-profile restart, ordinary-exit persistence, malformed widths, and escaped strings |
| Shell/terminal | Commands, child-shell input/output, return to panels, working canonical/echo state, and Screen history behavior |

The operations background-editor comparison with MC was inconclusive: the
isolated MC attempts did not reach an editor before timeout. mcpp's unsaved
edit loss was independently established from its screen, exit state, and
unchanged disk bytes; no MC behavior was inferred.

The cold standalone MC editor history comparison was also partial: its
private history files remained empty. mcpp history persistence/reopen was
exercised, but equivalent MC history-item/cursor restoration was not proved.

MC Info did not take focus in the tested Tab/mouse attempts. Its retained
listing operations were observed, but they are not an equivalent control
for mcpp's explicitly focused Info mode.

## Current local functionality and remaining gaps

### Panels and menus

mcpp supports two local listings, parent navigation and history, sorting,
hidden files, case-sensitive filename filters, masks, selection, Quick view,
live Info, Tree, pane swapping, hotlist, and filename-search panelization.

Tree opens at the chosen pane's cwd. Enter opens the selected directory in
the opposite listing and expands it. Right expands/moves to a child; Left
selects the parent. Space toggles, `+` expands, and `-` collapses. A single
click focuses a node and a double-click opens it. Explicit collapse controls
and Enter expansion are requested mcpp behavior, not an assertion that all
MC Tree modes use the same controls.

Unavailable listing formats, multi-column layouts, split/resizing controls,
directory-size calculation, codepage selection, full help, key learning, and
custom keymaps remain gaps. Grayscale chrome is intentional.

Dialog text inputs support cursor/word editing, Shift selection, mouse positioning
and dragging, kill/yank, and session history. Filename/directory and operation
destination inputs provide path completion with ambiguous-match cycling.
Editor search/replacement and numeric options use the same editing controls.
Persistent input histories and shell-variable/command-name completion remain
gaps. Editor menus support mouse category/action selection.

### File operations and search

The copy/move dialog includes source masks, destination input, preserve/follow
link and permission options, background choice, and OK/Cancel. The running
operation supports progress, pause/resume, skipping, and cancellation.
Regular-file publication uses staging/no-clobber behavior rather than writing
over the visible destination while copying.

Failures, skipped-source results, and cancellation during another full-screen
view remain pending instead of closing that view or losing the result.
The commander shows `Background [Esc]` on return; Escape dismisses the notice.

Collisions now offer per-file Overwrite/Skip, All/None, Older (overwrite only
when the source is newer), and Cancel. Transfer, rename and removal errors offer
Retry, Skip, Ignore all, and Abort. Skipped items remain selected and produce
an incomplete result. `Ctrl-X j` opens recent job results and active-job
pause/resume, show-progress and abort controls.

This is still not MC's full operation controller. Only one operation can run
at once, job history is session-local, and initial traversal/measurement errors
still abort the operation. Recursive-delete choices, append/reget, and resuming
a partially copied file remain gaps; Retry restarts the failed file.
ACLs, xattrs, sparse layout, hard-link sets, and all failure/rollback
combinations were not established by this review.

Find and filename panelization are asynchronous and cancellable. They are
not an external-command panelizer. Saved external panelize commands,
advanced Find options/context, thorough byte comparison, and diff/merge
remain unavailable.

### Viewer

The viewer has its own full-screen header/keybar, bounded range reads,
text/hex display, wrap, horizontal scroll, Home/End, search, and repeat search.
Goto, raw/nroff, format, bookmarks, hex editing/save, next/previous file,
codepages, and screen selection remain unavailable.

Known differences: repeat search wraps automatically instead of asking to
continue from the beginning; standalone `--view` returns to panels on F3
instead of exiting like `mcview`; hex-to-text offset alignment differs.
In the tested far-right unwrapped search, neither program automatically
revealed the match. That is not counted as a mcpp-only defect.

### Editor

mcpp has switchable buffers, per-buffer editing/search state, persistent file
history, linear/rectangular blocks, persistent clipboard, overwrite,
undo/redo, preferred display columns, horizontal scrolling, and
normal/regex/wildcard/hex search with replacement options.

Atomic saves reject external changes and preserve a regular-file symlink
when writing its target. New buffers, save-as, dirty-close decisions, and
independent buffer undo/save are supported. Files larger than 16 MiB remain
rejected rather than using MC's large-file confirmation workflow.

F10 and File > Quit check inactive dirty buffers as well as the current one.
Cancel retains every buffer, including after an earlier No decision. File >
Close and Escape still close only the current buffer.

Mouse dragging supports linear blocks and Alt-drag column blocks.

The editor follow-up compared actual `mcedit` screens for EOF with and without a
final newline, paging, arrows, mouse wheel, horizontal scrolling, document/window
jumps, and F8. The view now stays steady rather than recentering on every move,
fills the final page, and retains per-buffer scroll positions. Document jumps
retain the preferred column, including through short lines. F8 deletes the whole
unmarked line; an empty mark is cleared without deleting text. The character
readout distinguishes a newline from EOF.

F2 and File > Save show Save/Cancel confirmation for clean and dirty buffers.
Keyboard and mouse cancellation retain the draft, cursor and mark. Save failures
keep the editor open and dirty. Yes in the modified-file close/quit dialog saves
directly, without a redundant confirmation.

Missing features include movable/resizable editor windows,
block-to-file actions, lock interaction, backup/recovery modes,
macros, spell checking, literal insertion, completion, all-charsets search,
navigable Find All results, replace-again, formatting, and external filters.
Binary/non-printable editing and grapheme-cluster editing are not claimed
equivalent to mcedit.

Syntax colors use a built-in Dark+-style scheme; the rest of the UI stays
grayscale. External syntax databases, encoding selection, and richer options
remain gaps.

### Shell, configuration, and providers

`Ctrl-O` launches a disposable interactive child shell, not MC's persistent
subshell toggle. Use `exit` to return. A child `cd` does not change the pane
cwd, and a later shell starts at the pane directory rather than resuming the
previous shell. Terminal restoration working does not close this gap.

Configuration persists panel paths and common preferences, editor options,
history, and hotlist. Full per-pane preferences, skins, localization,
extension handlers, and MC's complete options dialogs remain absent.

Only the local Linux provider is enabled. Archive/extfs, FTP, SSH/SFTP/FISH,
Azure, undelete, and VFS cache controls are not part of this completed local
review.

## Coverage limits

### Final verification

| Build | Functional checks |
|---|---|
| Debug | 12/12 CTest entries passed; all 15 standalone operation cases passed |
| Release | 12/12 CTest entries passed; all 15 standalone operation cases passed |
| ASan + UBSan | 11/12 passed together; the remaining Tree workflow passed its bounded targeted rerun |

Guarded filesystem models and the dedicated editor regressions also passed
with fatal sanitizer diagnostics enabled. All destructive/error-injection
checks used disposable containers without host mounts.

Extended sanitizer background-race automation did not complete reliably and
is not counted as passing. This review stopped further stress-test tuning;
the Debug/Release operation results above are the verified functional result.

The committed model and terminal regressions reproduce concrete failures,
not every MC action or filesystem condition. They do not establish behavior
for millions of entries, every Unicode sequence, resize/signal storms,
crash durability on every filesystem, arbitrary special devices, or all
metadata combinations. Some local provider and viewer work is synchronous;
remote-latency behavior has not been evaluated.

Passing these checks or matching selected screenshots is evidence for the
listed workflows only. Deferred providers and advanced MC features should
not be presented as implemented.
