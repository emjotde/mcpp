# mcpp versus GNU Midnight Commander

Review date: 2026-10-17

This is a functional review of the supported Linux local-filesystem workflows,
not a claim that mcpp is a complete MC clone. Archive, SSH/SFTP, Azure, and
spelling support remain deferred.

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

Generic inputs now have usable controls but remain simpler than MC's input
widgets: full cursor/word editing, selection, completion, and input history
are not implemented. Editor-menu mouse behavior is not complete.

### File operations and search

The copy/move dialog includes source masks, destination input, preserve/follow
link and permission options, background choice, and OK/Cancel. The running
operation supports progress, pause/resume, skipping, and cancellation.
Regular-file publication uses staging/no-clobber behavior rather than writing
over the visible destination while copying.

Failures, skipped-source results, and cancellation during another full-screen
view remain pending instead of closing that view or losing the result.
The commander shows `Background [Esc]` on return; Escape dismisses the notice.

This is still not MC's full operation controller. Rich per-file overwrite
choices, retry/ignore workflows, background job management, recursive-delete
choices, append/reget, and resumable disk-full recovery are not equivalent.
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

Missing features include movable/resizable editor windows, full mouse block
selection, block-to-file actions, lock interaction, backup/recovery modes,
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
