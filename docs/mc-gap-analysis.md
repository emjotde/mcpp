# mcpp versus GNU Midnight Commander

Review dates: 2026-10-17 through 2026-10-20 (UTC)

This is a functional review of the supported Linux workflows, not a claim that
mcpp is a complete MC clone. Remote providers remain deferred. The current
completion target includes local archives, spelling, and shell integration.

## Measured local parity baseline

The baseline is `fa5f3d1`, compared with the installed Linux MC 4.8.30.
The ignored reference checkout is useful for studying interfaces, but its newer
revision is not the version used for runtime comparisons.

| Measure | Baseline | What this measures |
|---|---:|---|
| Main-menu actions | 66 / 75 enabled | Includes both panel menus; excludes separators. |
| Editor-menu actions | 65 / 68 enabled | Excludes separators. |
| Combined menu actions | 131 / 143 enabled (91.6%) | Availability, not correct behavior or full feature coverage. |
| Local-scope menu actions | 131 / 137 enabled (95.6%) | Excludes the six FTP/Shell/SFTP entries in the two panel menus. |
| Local non-POSIX VFS families | 0 / 5 | tar, cpio, sfs, extfs, and ext2 undelete. |
| Standalone frontends | 2 / 4 | Manager and editor exist; separate viewer and diff frontends do not. |
| Known local gap work packages | 34 open | The table below; packages are not equal-sized features. |

There is no justified overall completion percentage. A menu can expose only part
of a feature, and source presence is not proof of behavioral equivalence.
The 67 addressed audit findings measure fixes in previously exercised workflows,
not the fraction of MC implemented. Likewise, the historical 100-case
investigation includes blocked, inconclusive, and deliberately different cases.
The built-in syntax set has nine highlighting languages plus plain text; the
reference checkout contains 109 syntax definition files. Those are different
units and must not be presented as a language-coverage percentage.

### Local gap checklist

Every row is open at the baseline. Closing a row requires working behavior,
appropriate outcome checks, and an updated disposition here; merely enabling a
menu item or adding a command-line flag is not sufficient. This is a concrete
backlog, not a claim that all untested behavior has been enumerated.

| ID | Remaining work | Baseline evidence |
|---|---|---|
| L01 | Standalone viewer and diff frontends | `CMakeLists.txt` builds only `mcpp` and `mcppedit`. |
| L02 | Unnamed editor startup, per-file line positions, and multi-file manager edit startup | `src/editor_main.cpp` requires a filename; `src/main.cpp` accepts one edit filename. |
| L03 | Remaining applicable MC command-line options | The manager and editor implement only a subset of `mc --help-all`. |
| L04 | Persistent Bash, environment and cwd synchronization, and screen toggling | `run_process()` starts a new child for each invocation; the parked shell draft is not built. |
| L05 | Multiple concurrent operations and command execution alongside them | One `operation_thread`; `run_process()` rejects an active operation. |
| L06 | Append and resume/reget collision choices | `CollisionDecision` has only overwrite, skip, and cancel. |
| L07 | Copy/rename transformation masks, stable symlinks, and directory-placement choices | The dialog filters source names; `OperationOptions` has no destination transform or stable-link/dive policy. |
| L08 | Recursive-delete per-directory/all/none decisions | Removal exposes error handling but no recursive-delete decision callback. |
| L09 | Sparse and special-file copying | Regular-file copying writes every byte; non-file/directory/symlink types are rejected. |
| L10 | Large-file editor confirmation instead of a 16 MiB hard rejection | `EditorModel::open()` and backup handling impose that fixed limit. |
| L11 | Cross-session editor lock interaction | Open/save uses revision checks but no editor-lock lifecycle or takeover decision. |
| L12 | Codepage changes with history and raw-byte search in decoded editor data | Documented reopen requirements in `README.md`; `EditorModel::set_encoding()`. |
| L13 | Growing files and streamed viewer sources | `ViewerModel` retains the opened size and requires a seekable provider file. |
| L14 | Large parsed/formatted views and advanced searches | Viewer transformations and advanced search impose a 64 MiB buffer limit. |
| L15 | Shell-command, variable, user and hostname completion | `complete_path()` completes filesystem paths only. |
| L16 | Persistent dialog input histories | `TextInputState` histories are session-local; config persists selected histories only. |
| L17 | Hierarchical and named directory hotlist entries | `Config::bookmarks` is a flat vector of paths. |
| L18 | MC-compatible configuration files and applicable options | `Config` uses mcpp's independent keys and file locations. |
| L19 | Configurable MC-format keymaps | Learned terminal sequences are supported, but action keymap files are not loaded. |
| L20 | MC user/editor menus, conditions, and complete macro expansion | `load_user_menu()` uses `key|label|command`; only `%f`, `%d`, `%D`, and `%%` are expanded. |
| L21 | MC extension-file matching and action compatibility | Local extension handlers use mcpp's own resource format. |
| L22 | MC syntax-rule format and external definitions | `src/syntax.cpp` has a fixed built-in language set. |
| L23 | MC skin and filename-highlighting formats | Appearance and highlighting use independently defined subsets. |
| L24 | Non-UTF-8 terminal output and 7-bit input modes | Display bits reports the renderer's fixed UTF-8 requirement. |
| L25 | Context-sensitive navigable help | F1 shows a short key table, not the full help browser. |
| L26 | Localized UI and help | UI text is emitted directly; no translation catalog is loaded. |
| L27 | Spell check, current-word checking, and language selection | Three editor menu entries are disabled. |
| L28 | Local tar archives, including compressed variants | Only `LocalProvider` is enabled; `open_location()` rejects other schemes. |
| L29 | Local cpio archives | Same provider limitation. |
| L30 | Single-file transformation VFS | Same provider limitation. |
| L31 | External-filesystem helpers and their public interface | Same provider limitation; no extfs dispatcher. |
| L32 | ext2 undelete | Menu action is disabled; no implementation. |
| L33 | Local VFS cache lifecycle, active list, and options | Active VFS and Virtual FS menu entries are disabled. |
| L34 | Independent viewer/diff screens in the screen selector | `show_screens()` tracks one viewer and editor buffers, but no diff screens or multiple viewers. |

FTP, SFTP, FISH/SSH, and Azure are excluded by the user's current remote-work
pause. Requested Tree behavior, the grayscale UI, and explicit save safeguards
are retained; they are not bugs to remove in pursuit of default MC behavior.

### Verified closures after the baseline

| Package | Disposition |
|---|---|
| L01 | Closed: separate `mcppview` and `mcppdiff` executables share the existing viewer and diff/merge code. Quit exits the process; dirty Save/Discard/Cancel and diff-to-editor return paths retain their guards. |
| L02 | Closed: unnamed startup and New buffers, name-on-save including inactive-buffer Quit, `file:line` / `file:line:` notation, `+line`, existing literal colon filenames, and ordered multi-file `mcpp --edit`. CLI filename resolution and unnamed CLI startup use process cwd rather than restored pane paths. |
| L04 | Closed for the explicitly listed Bash work: persistent shell state and cwd, Ctrl-O hide/resume including running commands and Bash builtins, guarded drafts, native shell-exit behavior, asynchronous completion, safe shutdown, and command execution alongside file operations. Other shell variants are tracked separately below. |
| L05 | Closed: independent file-operation workers capture their own inputs, options, pane identity, progress, cancellation and answers. Background questions wait for Show; pause/abort affect only the selected job. Copies, the persistent shell and editing run alongside other jobs; exit cancels and joins paused/waiting workers. Exact bytes and concurrent UI outcomes are checked. |
| L06 | Closed locally: native Append/Reget choices stage regular-file publication, validate revisions and resume prefixes, retain cancellation/progress and move fallback, and omit unsupported archive/remote/symlink/special append. Native reference, exact bytes, failure safety and actual collision UI are verified. |
| L07 | Closed for local workflows: source filtering and destination transformation captures/case conversion, native directory Dive placement and revision-checked staged stable-symlink copying. Model and actual dialog outcomes preserve original files/referents. Regex is ECMAScript rather than full PCRE; stable links are local-only. |
| L08 | Closed: guarded per-directory recursive decisions and native Yes/No/All/None/Abort controls, operation-wide All/None, safe No defaults and partial-result/selection retention. Model cases and two actual All/None workflows pass; skipped nonempty directories are neither entered nor removed. |
| L09 | Closed: local sparse copies retain logical bytes and supported holes; FIFOs and permitted device nodes are recreated without reading them as streams; sockets fail explicitly. Existing atomic publication, revision, metadata, hardlink, cancellation and error handling remain guarded. Non-root checks cover device permission failures; creating device nodes still requires normal Linux capabilities. |
| L10 | Closed: files above 64 MiB require approval before contents are read, including queued CLI files, Open/history, manager and Find/Diff editing. Decline/failure retains drafts and other arguments. Exactly 64 MiB opens normally; large backups use streaming copies and revision checks instead of the former 16 MiB rejection. |
| L11 | Closed: native first-change `.#filename` locks, owner/stale-lock handling, Grab/Ignore/Escape decisions, lock-safe deferred actions/macros, transactional Save As and owned-lock cleanup. Save/close release locks without removing another session's replacement. Readonly archives and transient diff buffers do not lock host paths. |
| L12 | Closed: dirty buffers and undo/redo survive codepage changes, encoded raw-byte searches map back to document positions, and marks/cursor/exact saves remain consistent. Invalid historical bytes fail explicitly during undo/redo without consuming history. Refined model cases and the real editor workflow pass. |
| L13 | Closed: growing/truncated/replaced local sources refresh; streamed/fd input spools privately with terminal keyboard separation, range navigation and exit before EOF. Streams are raw/read-only, staged hex edits retain source-conflict guards, and transformed snapshots require explicit refresh. |
| L14 | Closed for the former 64 MiB parsed/formatted/advanced-search cap: private seekable data, immutable owned mappings, streaming conversion and exact source/presentation offsets handle larger views without arbitrary whole-file RAM allocation. Four focused boundary/parsed/search/mapping cases pass. Real disk/`off_t`/provider limits, libc regex workspace and its 2,147,483,647-byte subject-offset bound remain explicit. |
| L15 | Closed: command-position PATH/builtin/keyword completion and variable/user/local-host completion share native list/common-prefix behavior with filenames. Quotes, cursor suffixes, selection, UTF-8 and archive namespaces are preserved; completed archive URIs are accepted by F4. No shell evaluation/hooks or network lookup. |
| L16 | Closed: purpose-separated dialog histories persist atomically in a private independent file, including filename/directory/copy and search/replace/goto inputs. Restart, draft/cursor restore, cleanup confirmation and failed-accept/retry behavior are verified. Arbitrary menu answers, secret inputs and key queries are not automatically enrolled. |
| L17 | Closed: named entries/groups in the native hotlist format, hierarchical navigation, add/group/move/remove controls, safe persistence and flat-bookmark preservation. Cancels and failed writes retain data; canonical archive URIs, C-x h and configurable HotListAdd share the same flow. |
| L19 | Closed for implemented commands: MC-format resources, overlays, key aliases/terminal decoding, all 16 contexts, manager/editor extended maps, action rebinding/removal, and actual dispatch across the four frontends and focused controls. `-K` / `--keymap` and `--nokeymap` are launch-only; unavailable commands fail explicitly rather than invoking another action. |
| L22 | Closed for the bounded native syntax format: external registry/definitions, PCRE2 registry matching, native byte patterns, contexts/keywords, color inheritance, include/define handling and transactional reload. Native empty-keyword and context-edge forms, literal byte offsets and actual rendering match focused MC 4.8.30 controls. Input, include/macro-depth and pattern-atom limits remain explicit; no resource collection or unlimited/undocumented-input equivalence is claimed. |
| L25 | Closed: independently written contextual help has linked topics, an index, back navigation, scrolling and mouse controls. Manager/dialog drafts, editor/custom keys, raw key capture and dirty-viewer return are checked. MC `.hlp` import and full-text help search are not implemented. |
| L27 | Closed: whole-document and current-word checking, dictionary selection, native suggestions/Add word/Replace/Skip/Cancel controls, and persisted personal dictionaries through standard `aspell(1)`. Byte offsets are validated, replacements are undoable, marks/search settings remain separate, and errors retain the draft and retryable dialog. |
| L29 | Closed for standard local cpio formats: newc/CRC, odc and V7 binary little/big-endian indexing/publication, safe links and special kinds, nested snapshots, ordinary viewing/extraction and captured-parent return. Streamed data replaces the former byte caps; natural header field ranges and explicit metadata limits remain. Ambiguous PWB/vendor extensions are not claimed supported. |
| L30 | Closed for local single-file transformations: native `source/name://suffix` paths, original standard compression converters and trusted local `/1` registry helpers, read-only provider snapshots, revision-aware caching and captured-namespace resolution. Viewer/editor/diff and actual F5 Source-mask copying retain provider identity; readonly F6 fails before writes. Inputs/outputs are currently bounded to 64 MiB; non-local helper flags remain outside the paused remote scope. |
| L33 | Closed for the currently supported local providers: cached revisions, active-provider retention, unused timeout, private cache creation, native Active VFS list and timeout setting, and logical URI/alias preservation. Changed containers open a new snapshot; remote options stay outside the current scope. |
| L34 | Closed: independent viewer/diff sessions appear alongside editor buffers in the native screen list and next/previous cycling. Dirty bytes, search/cursor/bookmarks, streams, comparison/editor return identity, save/link/conflict decisions and final standalone exits retain their own state. Labels fit the actual dialog content width without hiding filenames. |

Standalone frontends are **4/4**. **Twenty-four of the 34 baseline work packages are
closed; 10 baseline packages remain open**. This is a feature backlog, not an
overall parity percentage. Additional gaps discovered afterward are listed
separately, rather than silently changing the baseline denominator.
The broader command-line option set is still L03, not implicitly completed by
the new frontends.

The normal Release suite passed **24/24** entries in the non-root, no-mount,
no-network disposable environment. This includes the standalone-editor cases,
eight frontend cases, twelve spelling cases, and existing local regressions.
Installed MC controls confirmed unnamed startup, New, zero/explicit line
positions, literal colon filenames, and spelling-dialog controls. Actual
installed Aspell checks also verified exact replacement bytes and a personal
word recognized by fresh checker processes without changing the document.

The comparison found and fixed another UI issue: an unnecessary horizontal
frame gave a fully fitted editor header one extra column, clipping the final
codepoint digit with long filenames. A dedicated long-name regression and the
existing editor parity walk now retain the complete readout.
Follow-up also exposed a lazy Tree search race: a query entered before matching
children arrived could remain on an earlier ancestor. Unmatched active modal
queries are now retried when a directory load publishes children, without
undoing subsequent manual navigation. A create-then-rescan regression checks
the accepted directory, and the sidebar-selection/cache workflow passes.
The container runners use Docker's init process to reap orphaned terminal-test
children rather than exhausting their PID limit on a long sequence of sessions.

### Concurrent jobs, independent screens and help

File operations now run as separate jobs rather than sharing one worker and
answer slot. The focused workflow checks simultaneous collision questions,
isolated pause/abort/Show answers, another copy, shell commands and editing,
then exact bytes and shutdown with a paused/waiting job. Background completion
and questions leave foreground screens and dialog drafts intact.

Viewer and diff sessions have stable identities in the screen list and native
cycling. Their focused checks retain independent dirty state, bookmarks, cursor
and search settings, stream input and temporary data. Editing returns to the
comparison that opened it; stale-source and linked-save decisions keep their
existing guards. The screen list now budgets its 42 content columns rather than
counting the dialog borders as text space.

F1 opens the current context without closing the underlying screen or dialog.
Links, index/back navigation, scrolling and mouse actions use original English
help. Raw key queries still receive F1. The manager/dialog and editor/keymap
workflows pass; viewer-help return is covered inside the existing screen case
rather than another separate test.

The shared Release/Werror build passed. The bounded runtime checks passed as
UID1001 in the existing no-host-mount disposable environment. Only cases blocked
by selector clipping were repeated after its corrections; no full-suite rerun
was added.

### Skin collection

The additional skin request expands the built-in collection from two to sixteen:
classic blue, grayscale, Amber, Cobalt, Contrast, Ember, Forest, Graphite, Green,
Ice, Midnight, Ocean, Paper, Plum, Rose, and Sand. Three are light palettes;
Amber and Green also tint syntax colors. The grayscale default is unchanged.

Panes, Tree, menus, dialogs, hints, key bars, viewer, diff, editor selections,
and syntax use the selected palette instead of fixed gray colors. Appearance
keeps the native skin list, immediate preview, OK/Cancel, and shadow controls.
Cancel restores both the saved skin and any launch-only override. `-S` /
`--skin` works in all four executables without replacing the saved preference.
Unknown names fail explicitly. Capability-specific caches retain valid palette
references when the terminal changes its reported color depth.
The legacy `theme=classic` setting is explicitly mapped to its previous
grayscale appearance. It is not an invalid new skin name: rejecting it prevented
older saved configurations from starting the release. Startup and normal exit
are checked without changing that saved value.

Classic blue's marked active selection now uses black text on cyan instead of
the previous low-contrast yellow; inactive selection uses light text on blue.
The active marked and inactive text contrast ratios improve from 1.31:1 and
3.33:1 to 6.54:1 and 5.43:1 respectively.

The normal Release suite passed **25/25** entries. The skin workflows check all
sixteen palettes in panels, viewer, dialogs and editor, including marked and
inactive selections, readable syntax, unchanged document bytes, saved setup,
preview cancellation, all four CLI frontends, and transient overrides. Sampled
normal text, selections, dialog text and syntax meet a 4.5:1 contrast check.
Sixty-four real terminal captures were rendered into panel/editor and dialog
galleries and inspected.

This completes the extra built-in-skins request, not L23 in full. MC-format skin
files and filename-highlighting compatibility remain open.

### Menu skin corrections

Follow-up feedback exposed states the first skin walk had not explicitly
checked. Dropdown selection was still an inversion of the normal menu color,
so several dark skins showed nearly the same pale highlight instead of their
selected accent. Shortcut letters inherited the normal text color, the F2
menu inherited dialog colors, and focused controls/list rows used the same
plain inversion.

Menu selection, mnemonic, dialog-focus and error-focus colors now have explicit
bindings. Existing grayscale colors are retained. Main/editor dropdowns, F2
entries, popup lists, active inputs, buttons, radio choices and checkboxes use
the intended colors while retaining focus metadata and shortcuts. Captured
RGB checks cover Midnight, Forest and Paper menu states, not just labels.

### Persistent Bash

The approved Bash workflow is now implemented with a PTY and separate framed
control messages. Variables, aliases, functions, the PID, existing prompt
hooks/status, logical cwd and unusual path bytes survive screen changes.
Early Ctrl-O during startup retains a queued command; running builtins can be
hidden without losing input. Completion does not steal a manually navigated
pane. A paused file copy remains paused while Bash runs, and resumes with
byte-identical results.

Startup/IPC errors restore terminal handling. Request-channel writes do not
raise SIGPIPE, shutdown does not wait on a background child's open PTY, and
private PTY markers keep command output ahead of pause-after-run prompts.
The running-command case does not pause merely because the user hid it.
`-U`/`-u` overrides are launch-only; `use_subshell` is saved independently.

The expanded normal run covered 26 CTest entries. It exposed one command-output
ordering race in the existing pause workflow; after the fix, all 18 Bash
workflows, the 36-case follow-up suite and terminal restoration smoke check pass.

### Local providers, keys, large files and copying

Tar/cpio browsing, range viewing and extraction are now reachable from Enter and
native `/utar://` / `/ucpio://` paths. Pane exit, saved paths, histories, shell
cwd and panel synchronization preserve the provider instead of interpreting a
virtual member as a host pathname. Relative copy destinations retain that same
namespace and readonly checks run before touching a destination. Editor Save
reports readonly archives; Save As can publish a guarded local copy without
changing the source. Extracted files retain requested modes, modification times,
safe symlinks and hardlink groups.

That initial delivery made two local VFS families available in bounded form.
The later archive work below adds nested archives, sparse/special members,
older cpio formats and streamed data beyond the former byte caps. Standard GNU
tar `./` prefixes still work; traversal and ambiguous canonical names are
rejected. The single-file transformation family is delivered separately;
source-based extfs now exists, and the read-only ext2 image extension below is
partial L32 support rather than native block-device undelete.

The hex viewer now highlights the matching byte in both hex and ASCII columns.
Only the input column receives the terminal cursor; mouse/nibble placement,
partial rows and horizontal cropping retain the corresponding highlight.

The broad normal run passed 35/37 entries. The affected frontend/keymap cases
passed after correcting relative CLI paths, settings-action dispatch and
controller expectations. Later archive namespace fixes were checked only in
their affected cases; no second full run is claimed or required for this batch.
Future work uses a build and a few relevant checks, not broad repeated suites.

### Expanded archives and external helpers

Nested tar/cpio sources now retain the actual provider, normalized member path
and revision in their cache keys and return paths. GNU sparse tar variants,
odc and V7 binary little/big-endian cpio preserve their data/kinds, including
sparse holes and safe FIFO extraction. Private seekable storage replaces the
former 64 MiB encoded/256 MiB expanded caps. Member/path/metadata/extent and
natural format/disk limits remain enforced. GNU `D` directories and local PAX
`GNU.dumpdir` now support read-only browsing and exact member copying. Restore
names/renames are validated metadata, not destination operations; omitted prior
backup contents are not invented. The native MC control navigated directories
but returned next-header bytes for the regular member, so mcpp's actual payload
was checked against GNU tar instead of reproducing that bug.
Ambiguous PWB/vendor cpio, writable incremental archives, multivolume tar and
unsupported PAX extensions remain explicit
format limits; the zstd command path was not runtime-checked in this environment.

`image/undel://member` now provides read-only deleted regular inode recovery from
stable ext2 images, with actual source-provider identity/revision, explicit
unrecoverable reads and real-image root exits. Missing pointers are not treated
as holes, and the source image is never modified. The member colon remains part
of editor filenames; explicit line suffixes follow it. Native MC rejected both
tested regular-image URI forms, so native block-device naming/root/metadata
parity is not claimed. **L32 remains partial** for native device recovery and
other unsupported filesystem/inode features.

Native source-based extfs helpers now provide listing, copyout and accepted
copyin/rm/mkdir/rmdir through guarded private candidates. Actual extension
`%cd`, Quick cd, viewing, copying and root-parent navigation pass. Local
source-less `+` helpers now have separate captured mount and working directories,
read-only list/copyout and retained member bytes. Native bare/prefixed URIs and
root exits use real directory origins, never fabricated container files.
The protocol has no global content revision; bounded caches do not refresh
already retained bytes. `run`, special nodes, link creation/linked-member
replacement, metadata changes and native rename remain unsupported;
**L31 remains partial**. Named
ownership and the listing's single timestamp are metadata, not host credentials
or independent atime/ctime. Container/member/listing/time/memory bounds are
documented in the README.

Recursive copies validate the real backing-provider chain before creating a
destination; virtual absolute symlinks cannot silently become host links.
Fallback archive/extfs moves verify the copied source revision before removal.
The focused mutation case proves a newer source and the valid copied destination
both survive. Shared Release/Werror, archive formats/snapshots, three extfs
selectors and the reserved archive/extfs UI workflows pass; no full suite or
native-reference rerun was added.

### Native resource work

Native `mcpp/ini` and `panels.ini` now apply supported local options while
preserving comments, unknown settings and existing flat-config preferences.
New installations use the native files; existing flat setups retain their
format unless a native `ini` is supplied. Launch overrides remain transient.
Private saves are atomic per file, not crash-atomic across both files, and MC's
own configuration directory is never changed. Three small model cases and
configured startup/Save Setup/restart passed the initial delivery. The follow-up
applies saved listing/Tree/Quick view/Info displays, physical focus, independent
layout drafts, clipboard/status/paragraph/codepage preferences, date formats and
remembered viewer data offsets. Only one auxiliary panel and the supported
codepages are accepted. The focused startup/save/restart workflow passes Tree
focus and CLI precedence, layout apply/Cancel, escaped dates, CP1251 data,
clipboard/status/paragraph preferences and a 45-byte viewer data position.
**L18 remains partial** for other unimplemented options;
unsupported settings are diagnosed and preserved instead of silently accepted.
Four further keys now affect the real controls: double-click milliseconds,
tagged-directory F3 confirmation, `file`-based type guessing and the copy/move
attribute-preservation default. Native controls established their meanings;
absent keys retain legacy behavior and invalid values preserve prior settings.
History limits, editor size thresholds and selection flags now reach their
actual history, editor-candidate and selection controls. Zero history disables
use/recording without erasing disk data; `1K` means 1024 bytes and zero threshold
warns for nonempty files. Absent keys keep the prior limits and 64 MiB approval
behavior; declining or stale approval retains the current draft. Native
selection-mask choices apply to real matches. `[Colors]` now retains base and
exact-terminal definitions and terminal forcing; effective order is base,
exact `TERM`, environment, CLI, with transient overlays excluded from saving.
This completes the enumerated eight-key follow-up, not all L18 compatibility.
Compact editor status and the cell-only right-margin preference are now
implemented with legacy-appearance defaults when absent. Margin paint preserves
glyphs and cursor/selection/search styling and uses native `editrightmargin`.
Native clipboard store/paste commands now use explicit local-directory,
private raw-file/stdout contracts. Absent/empty settings retain internal
behavior; empty paste output retains the cache. Unlike native MC's silent
fallback, configured failures surface before editor mutation. Commands use
closed stdin and a two-second watchdog, with the existing clipboard size cap.
Other unmapped local options, including output lines and display-codepage
settings, remain diagnosed and preserved.
Native selection-key preferences now distinguish literal `+/-` from group
selection using the whole command draft; keypad/Alt actions remain independent
of that draft. Explicit mcpp keymaps retain precedence over the observed native
override quirk. Native Home/End mode visits visible midpoint/edge before the
directory edge without changing arrow/page algorithms.
Shell-display preferences clear the alternate panel buffer before restored IO,
not the saved primary output. Output-only mode without a subshell consumes one
logical key and restores terminal state; supported existing shells keep their
state and resume regardless of the output-start flag.

Local user/editor menus now load native conditions, command blocks, prompts,
pane/editor macros and view/cd directives, while retaining legacy menu rows.
Expansion now includes explicit raw `%0`, quoted tagged lists, backticks, ANSI
quotes, arithmetic and heredocs. Ordinary values remain literal rather than
becoming shell source. Named homes and CDPATH use a captured provider/directory;
relative archive paths do not search the host filesystem. **L20 remains partial**
against the original complete-expansion target: ordinary arithmetic values are
decimal or hexadecimal literals only, and unsafe or unsupported heredoc forms
fail explicitly. Escaped nested backticks and mixed modern substitutions now
preserve literal macro data with the unchanged 128-level scanner bound; raw
`%0` remains trusted and unrescanned.
Standard `case` lists inside modern command substitutions now
balance patterns, alternatives, nested cases and Bash fallthrough while keeping
ordinary macro values literal. Requirements and expansion use the same scanner.
Nested extended case patterns also work with an explicit extglob-enabled Bash
runner; mcpp does not change the selected shell's options.

External `Syntax` registries and definitions now feed the actual editor chooser
and color renderer, with transactional reload, saved selections and legacy-file
preservation. Colorless keywords inherit their context colors, and empty
`\[\]` repeat markers retain surrounding literal patterns, matching native
MC 4.8.30. Registry expressions now use PCRE2. No further directive or flag
omissions were found in the installed format. Repetition-only keywords now
match the cached native control, including zero-byte matches that color and
advance at least one byte. **L22 is closed for the bounded native format**.
Empty compiled keywords follow native absolute-offset, priority and line-tail
behavior; edge-star contexts retain original marker dispatch and NUL/right-word
boundary consumption. Empty context delimiters are inactive, and successful
context borders always consume input. Native caches and matching actual UI
cover the previously rejected forms without changing file bytes.
Aggregate input/include/macro/atom bounds remain explicit. Native MC 4.8.30 controls
confirm the existing ASCII-only case folding and byte-based sets, including
partial UTF-8-byte matches; those are baseline behavior, not compatibility gaps.
The editor now keeps UTF-8 characters visible across partial-byte syntax spans,
with the leading byte's style matching the native control. The focused model,
native comparison and corrected mcpp comparison pass without changing file
bytes. Unknown directives fail explicitly. Context `spellcheck` now emits
color-independent eligible byte ranges and filters whole-document and
continuation results. Its focused workflow
preserves UTF-8 offsets, explicit word checking, marks, Undo and saved bytes.
Delimiter changes that newly expose text require another whole-document check.
No MC resource collection is copied or bundled.
Separate native manual-spelling observations do not establish equivalence with
mcpp's context-filtered whole-document workflow; L27 is unchanged by this patch.

Native skin selection, color aliases/attributes/inheritance and filename/type
highlighting now affect the real UI, with Appearance preview/cancel/persistence
and transactional reload. Original palettes and legacy aliases remain intact;
unmatched native groups preserve builtin type colors. **L23 remains partial**
for remaining widget glyphs and some auxiliary native widget-color roles.
Single/double border, sort and editor window glyphs are now supported without
rewriting file text or overriding explicit CLI ASCII frames.
Filename expressions now use PCRE2. Four focused cases passed; final preservation
guards were recompiled and the small model rechecked, without another UI sweep.

Native extension resources now match Open/View/Edit and execute through the
shared directive/macro engine, preserving legacy rules, dirty buffers and
archive provider boundaries. Expressions now use PCRE2. **L21 remains bounded**
by explicit local-copy requirements for external Open/Edit and unsupported
archive actions plus the documented macro restrictions. Native View filters
now stream a regular selected virtual source into a private basename-preserving
checkout, override only `%f`/`MC_EXT_FILENAME`, and keep original `%p`/`%d` and
metadata identity. Dirty/unsupported-source/mark/argument checks precede copying;
the command uses checkout cwd, source revisions are rechecked, and no copyback
occurs. Output retains its own screen-owned file after checkout cleanup.
Symlinks and active virtual marks remain an explicit selected-only bound.
The failed native prompt-interpolation control is recorded as such; mcpp
literal argv preservation is verified independently rather than claiming that
failed native argv control passed.
Extension View, user-menu `%view` and Filtered
view now stream output to private files without the former 16/64 MiB stdout
caps. Each screen retains its own read-only output; failed commands do not
open partial output. Stderr remains bounded. Missing helpers, storage errors
and invalid resources remain visible errors, not reasons to execute a legacy
fallback.

### Command line, regular expressions and single-file transformations

The four frontends now share CLI parsing and launch-only mouse, ASCII-frame,
monochrome/color, color-role and subshell controls. Informational options return
without opening the UI, and literal-colon/process-cwd handling and saved setup
remain intact. A titled-help ASCII corner defect was fixed without changing
ordinary file-text glyphs. **L03 remains partial** for renderer-specific native
backend controls and unavailable color-role fields; non-UTF-8 terminal handling
remains L24.

CLI overrides now reach the already-rendered popup normal/title, editor gutter,
dialog/error title and error-focus pairs. Inactive menus accept their complete
pair while omitted fields preserve the current background and disabled contrast.
Native skins and CLI overrides share color/attribute parsing, including hex RGB
and `inverse`. The earlier `errdhotfocus` alias still paints the whole focus;
Unrendered mnemonic roles remain explicit errors. Native statusbar pairs now
style existing viewer/editor/Quick View/diff status text, not progress dialogs.
Native editor desktop, inactive/active frame and move/resize/corner roles now
have independent consumers, preserving text, status, hitboxes and geometry.
The independent `commandlinemark` pair now styles selected manager input across
focus changes without altering normal text or the three dialog-input roles.
Dialog input fields now use full `input`, `inputmark` and
`inputunchanged` pairs, including pristine blank fill and blurred selection.
Handled interaction clears pristine state rather than comparing the current
bytes to the original; command input remains excluded.
Formatted viewer/Quick view text now consumes `viewbold` and
`viewunderline`, and original help headings/title/navigation use `helpbold` and
`helpitalic`. Full pairs and explicit attributes are respected; missing roles
retain prior emphasis, search colors win over nroff styles, and selected help
links keep their styling. Native INI `[Colors]` uses the same existing parser
and supported-role set rather than adding unrendered no-op roles.

PCRE2 is an explicitly approved dependency. Native menu conditions, extension
rules, syntax-registry expressions and filename highlighting now use its Unicode,
lookaround and property support. Compile errors identify byte offsets, and
matching has explicit work/depth/memory limits. Four tiny cases cover these
consumers, invalid bytes, diagnostics and transactional resource preservation;
the directly changed menu model also passes. These changes do not close the
other documented L20-L23 restrictions.

Single-file converters preserve the source provider, revision and logical URI,
use private source/output files and retain independent cached snapshots. Native
controls and three focused model cases pass; the existing local case also
checks compressed virtual and ordinary local viewing. Actual frontend checks
cover SFS viewing/comparison and F5 copying from a relative operand. The copied
12 bytes, original destination base, unrelated marks and pre-write readonly F6
rejection were checked directly. Only the deferred ASCII and new copy portions
were repeated after their fixes; no full suite was rerun.

At that checkpoint, local tar, cpio and single-file transformation families
were present with documented bounds, while extfs and ext2 undelete were absent.
The later helper and read-only image deliveries above extend that snapshot.

### Large viewer data and skin inspection

Parsed/formatted views and advanced searches no longer require the former
64 MiB materialized string. Private seekable snapshots and streaming adapters
preserve byte/codepage/nroff mapping, search direction and source safeguards.
Only owned immutable data is mapped; live file truncation cannot invalidate a
mapped source. The shared Release/Werror build and four focused boundary,
parsed, search and mapping cases pass. The libc POSIX-regex subject limit remains
2,147,483,647 bytes; upstream provider limits are not silently removed.

A separate read-only inspection of frozen `cb38117` visually covered all sixteen
built-in skins at 110x32 truecolor. Controls/errors, editor menus/syntax, hex/ASCII
cursor highlights and help looked correct in the captured states. It found two
issues: changed-row comparison searches used the wrong search colors in all
skins, and disabled dropdown labels had low contrast in Default/Grayscale.

Both are now corrected: search styling takes priority over hunk styling;
disabled main/editor menu labels use the full native inactive-menu role without
forced faint text, with readable Default/Grayscale fallback colors. Four focused
terminal situations verify exact search colors on changed/unchanged rows and
inactive-label contrast of at least 4.5:1 without faint styling. A related CLI
override check preserves menu backgrounds. Other terminal sizes/capabilities
were not swept again. External skins received the follow-up inspection below.

That follow-up reproduced startup failures for all 36 installed external skin
paths on `0ce334f`: glyph metadata was parsed as colors and the native `inverse`
attribute was rejected. The loader now separates that metadata and accepts the
attribute; remaining unsupported widget keys are explicitly warned. F2 uses native
`popupmenu` normal/title/selection roles separately from main/editor dropdowns,
and fallback syntax colors follow the actual editor background. The shared
Release/Werror build, original native-resource model and a real popup-role
workflow pass. All 36 external paths now start and complete Appearance preview,
Cancel and saved-selection restart on the exact shared Release binaries, with
no application stderr. Their existing captures were visually reviewed in three
disjoint groups without repeating completed checks.

The parallel review found two additional role-use defects: editor gutters used
generic whitespace/dim colors instead of `editlinestate`, and dialog/error
captions ignored `dtitle`/`errdtitle`. Both are corrected, including backgrounds
and explicit attributes. Original regressions and focused real-skin checks
confirm `modarin256-thin`'s full gutter and `nicedark`'s non-bold captions.
Weak contrast authored in external files was not silently recolored.

Native border/sort/window glyph support also passes three model groups and a
small actual-App workflow covering preview/Cancel, generated controls, literal
Unicode preservation and CLI ASCII precedence. All 36 installed skin files still
load with glyph-aware parsing. History/hidden-file/scrollbar and other unrendered
glyph roles remain explicitly warned; L23 is not claimed complete. The visual
results apply to the captured 110x32 truecolor environment, not every terminal
mode or unknown user-provided skin.

### Initial UI translations

A bounded original GNU `.mo` reader now loads user catalogs once, without
installed locale data, new dependencies, environment changes or imported MC
resources. Explicit enum labels cover main manager captions, native
input/message buttons, exit choices and Quit/Error titles; no arbitrary string,
filename or file content can be looked up. All rejected catalog/label and
unsupported plural/context entries produce startup diagnostics; missing labels
retain later-language or English fallback.

Revision-0 UTF-8/ASCII catalogs now support exact context and checked plural
rules for typed Delete-caption/count messages. Locale fallback, eager catalog
validation and explicit expression/form/index/placeholder limits remain;
runtime arithmetic errors keep English and report once instead of once per
render. Untranslated Delete questions retain their exact previous wording,
and filenames/buttons stay literal.
Most UI text, message bodies, help, other contextual/plural message IDs and
localized accelerators remain untranslated. **L26 remains partial.**
The next caption subset adds typed IDs for 62 distinct manager pulldown rows
and seven editor menubar names, with shared Left/Right panel entries and
separators excluded from lookup. Original English arrays, row/category/action
identity, shortcuts, disabled entries and dispatch are unchanged; translated
UTF-8 labels do not enter the byte-indexed English mnemonic renderer.

### Additional compatibility gaps

| ID | Remaining work | Evidence |
|---|---|---|
| L35 | Partial: remaining persistent shell variants | Bash, dash and `sh` resolving to either now retain the selected executable, native startup, state, cwd, drafts and running commands. A cwd error discards the rejected command rather than executing it later on resume. Four small owned-container workflows cover Bash, sh/dash, async builtins/shutdown, and identity/error recovery. Persistent ash, ksh, zsh, tcsh and fish remain unsupported. |

## Audit fix delivery

The original 41 findings (A01-A16, B01-B11, C01-C10, H01-H03 and U01) were fixed
in `13c9c34`. The six follow-up items D01-D05 and U02 are also resolved by the
delivery below. The later screenshot/code walk against `e28a00d` recorded
E01-E05 and shared limitation U03. The original regression results do not imply
complete MC parity. The subsequent 100-case investigation below records new
F01-F07 and shared limitation U04. F01-F07 are now fixed by the seven-finding
delivery below. The later walk against `4333955` recorded G01-G05 and shared
limitation U05, plus additional text-viewer evidence for E02.
The current fix batch closes E01-E05, G01-G05 and U03-U05; shared limitations
are treated as deliberate usability improvements, not retroactively reclassified
as mcpp-only regressions.
The eight broader capability groups remain a separate feature backlog; the
explicitly paused providers and spelling remain paused.
The source excerpts and line numbers in the historical audit sections describe the old
`14b7a82` baseline, not the corrected code.

## Current audit fixes and native-control rework

The thirteen findings below have implementation and regression coverage.
The broader capability backlog is separate and is not marked complete.

| Findings | Corrected behavior |
|---|---|
| E01 | Hex Save asks for detach / preserve links / cancel, including Save-on-close. Model guards reject an unconfirmed linked save. The preserve choice writes the existing inode explicitly and non-atomically. |
| E02-E03 | Hex searches start at the current byte, text repeats respect manual navigation, and cancelled or unsuccessful wrap attempts retain the visible cursor/viewport state. |
| E04-E05 | Filename cells retain distinguishing tails, and the last hex byte reports 100%. |
| G01 | Search hits have their own range and highlight. Persistent linear/rectangular marks, F8 targets, and undo/redo state remain independent. |
| G02 | Text scrolling retains a filled final page instead of advancing into a blank EOF viewport. |
| G03 | Long-parent Find labels retain the differing parent tails and complete result identity. |
| G04-G05 | Editor categories and enabled dropdown items accept their mnemonics; outside clicks dismiss manager dropdowns without activating the clicked pane. |
| U03 | CR-only Save As and later edits retain logical lines rather than collapsing the document. |
| U04 | Question-mark masks consume a Unicode character rather than one UTF-8 byte; invalid filename bytes remain individually distinguishable. |
| U05 | A successful text search reveals a far hit in wrapped and horizontally scrolled views. |

After the user rejected simplified replacement dialogs, twelve complete native
menu inventories were captured from MC 4.8.30 and compared with mcpp. Labels,
order, separators, and printed shortcuts match. The new generic option-form
implementation was removed rather than presented as native parity.

Listing format now uses the native radio choices, Brief column count, user
format, and user mini-status controls. Long is a full-width single-row listing,
not a two-line mcpp layout. Sort order offers all nine native fields and exactly
Executable first, Case sensitive, and Reverse; the former made-up
Directories first sort control is gone. Mix all files is in Panel options.
Native format tags, alignments, widths, frame/column selection, and expansion
are parsed independently in C++.

Configuration, Layout, Panel options, Confirmation, Appearance, Learn keys,
Chattr, Advanced chown, and editor Save mode have reference-derived controls
with working local bindings. Layout uses actual row/column split sizes and
functional visibility controls. File / Filtered view executes the entered
command; Compare files uses the two selected files rather than inventing a
filename-selection form. External panelize has its command list and
Panelize/Remove/Add new/Cancel actions, with saved named commands.
Left/Right / Panelize restores a previous panelized listing.

Editor insert/save-block prompts, codepage chooser, hotkey macro dialogs,
record/repeat count, literal-key query, immediate date/time insertion,
marked-block sort, paragraph format, and arrow-driven window geometry no
longer use the rejected replacement options. Rectangular transforms retain
their surrounding bytes and undo as one edit. Managed clipfile defaults do not
insert or overwrite clipboard headers as document text.

Intentional differences and remaining work are explicit:

- Safe save remains the default instead of native Quick save. Linked
  preservation and stale-file safeguards remain mandatory.
- Gray/default-blue skins and the built-in syntax set are independently
  implemented subsets, not copies of MC resources.
- The FTXUI renderer still requires a UTF-8 terminal. Legacy Display bits
  choices are unavailable; pane/editor data codepages do not change the
  terminal encoding.
- Editor codepage changes preserve physical bytes and saving uses the selected
  encoding, but changing codepage with undo history or hexadecimal search in
  that decoded view requires reopening. This limitation is reported, not
  silently treated as conversion or a failed match.
- Complete user-menu/configuration resource syntax compatibility remains outside
  this independently implemented control set. The local General/Tree/diff/viewer
  work listed as unfinished in `dfd3e52` is completed below.

Native/source comparison artifacts are retained in the session's
`native-menu-rework` directory, including all twelve inventories, paired
listing/sort/editor captures, exact filesystem results, and normal-suite logs.
The normal Release suite passed all 20 CTest entries, including the 32-case
existing UI audit and 36 focused audit/control regressions. These are regression
results, not a claim that every MC feature has passed parity.
Tests run as UID 1001 in an owned disposable Docker container with no host
mounts and no network. No sanitizers, optional profiling, package installations,
or new project libraries were used. The pinned FTXUI source stays unchanged:
the build compiles a checked build-directory copy of its input normalizer to
separate physical Ctrl-Space from redraw events and honor the Escape timeout.

## Completion of the remaining local controls

This delivery finishes the four groups explicitly left open in `dfd3e52` and
the remaining local editor Mail menu entry. Remote/archive/spelling pauses and
the unrelated parked persistent-shell files remain unchanged.

| Group | Implemented behavior and outcome checks |
|---|---|
| Editor General | Actual native radio/checkbox/input groups, including both automatic wrapping modes, native half-tab handling, indentation backspace, independent tab/trailing markers, save confirmation/position, cursor-after-insertion, nonpersistent selection, virtual cursor and column marks, and grouped undo/redo. Wrapping and replacement are atomic undoable edits; virtual motion does not invent file bytes. |
| Modal Tree | Shared hierarchical lazy Tree, visible-name search, expand/parent/sibling navigation, Rescan/Forget, private persistent cache, and Copy/Move/Rmdir return context. Cancel preserves listings, marks, and an existing sidebar's selection. Quoted cache paths round-trip whitespace/control characters; unsafe cache symlinks are rejected. |
| Diff/merge | Side-by-side alignment, hunk navigation, directional staged merge and undo, native diff Options, search/codepages, split/symbol/number controls, editor round-trips and Save/Discard/Cancel. Linked-save decisions remain mandatory. A stale or unwritable destination retains the merged draft and external contents. |
| Viewer | Functional Raw/Parse and nroff formatting, bold/underline spans, native four-type Search with backwards/whole-word/all-codepage behavior, codepages, bookmark slots, ruler, history, and guarded adjacent-file navigation. Parsed bytes cannot be hex-saved over a compressed original. Saving raw hex edits from a formatted view uses physical source bytes, not the presentation. |
| Mail | Native To/Subject/Copies to controls and standard `mail(1)` argv/regular-file-stdin execution. Exact argv/body contracts, literal shell metacharacters, failed command retry, and unchanged unsaved document bytes are checked without sending mail outside the disposable environment. |

Paired native MC 4.8.30 and current mcpp captures cover General, modal Tree,
side-by-side diff, Diff Options, viewer Search/nroff, and Mail. Differences in
palette, safer atomic-save defaults, following the active directory on Tree
entry, and explicit resource limits are not presented as missing controls.
General's former independent indentation input and combined whitespace checkbox
were removed; old regression controllers now operate the actual native controls.

Raw range viewing remains suitable for large local files. Parsed/formatted views
and advanced/all-codepage searches use an explicit 64 MiB buffer limit; an
overstrike-heavy nroff line is also bounded. Transform errors preserve raw data
and drafts rather than returning a success-shaped empty view.

The normal suite now includes the 18-case `mcpp_native_completion_ui` entry in
addition to the earlier audit and operation checks. Successful captures and exact
outcomes are retained under the session's `native-local-completion` artifacts.
The final normal Release run passed all 21 CTest entries, including all 18 new
completion workflows, 36 follow-up controls and 32 original audit UI cases.
These are functional local regressions, not a claim of complete MC resource-file
or legacy non-UTF-8-terminal compatibility.

Core fixes now have bounded normal regressions for directory merge policy,
source-cycle rejection, copied timestamps/hardlink sets/permissions, pre-publication
Skip, invalid confirmation settings, persistent marks, scoped/zero-width/UTF-8
search and replacement, linked saves, access ACLs/user attributes, read-only
protection, line endings, directional/content comparison, partial Find errors,
staged hex save, and useful undo depth on a 16 MiB document.

The original batch added 19 terminal audit cases, including filesystem outcomes
and Cancel/error paths. The follow-up adds six more, bringing that suite to 25.

| Findings | Delivered changes |
|---|---|
| A01-A16 | Safe copy traversal and metadata, explicit linked saves, responsive transfer controls, directional comparison modes, matching/search, commander cd/exit, editor keys and save/create workflows, viewer controls, partial-error handling and whole-editor Quit. |
| B01-B11 | Per-file merge decisions, stable marked permission targets, retained save metadata, persistent marks, scoped/zero-width replacement and captures, read-only protection, same-inode copies, word movement and delta-based undo. |
| C01-C10 | Clickable keybars and modal controls, replaceable defaults, command caret/dragging, viewer wheel, Find View/Edit return context, identifiable long filenames, stable viewports, auxiliary-list navigation and short-terminal forms. |
| H01-H03 | Invalid confirmation settings fail explicitly, automatic configuration-save errors reach stderr/nonzero exit, and character search/replacement respects UTF-8. |
| U01 | Tree single-click selection, double-click opening and wheel navigation; this is a deliberate usability improvement over the reference behavior. |

The complete normal container suite passed all 19 CTest entries. The Release
binary also passed the core audit regressions and all 19 new terminal audit
cases. The older audit descriptions remain below as historical evidence, not
current functionality claims.

## Follow-up fix delivery

All six items from the `7a4370e` walk (D01-D05 and U02) are resolved in `e28a00d`.

| Findings | Delivered behavior |
|---|---|
| D01 | Copy and Move keep their source identity in the fitted title and OK/Cancel outside a height-bounded options body. Wheel, paging and focus navigation reveal options without hiding the actions. |
| D02 | Main menus use a height-bounded scrolling body, keep their selected action visible and accept clicks only inside the visible menu body. |
| D03-D04 | The hex wheel scrolls the viewport immediately and preserves the cursor's relative row. Hex percentage describes the active byte rather than the end of the displayed range. |
| D05 | Go to offers logical line, percentage, decimal-offset and hexadecimal-offset modes through the existing Left/Right selector pattern. Lines start at 1, percentages accept 0-100, and 100% fills the final page. Invalid input retains its value and mode for correction. |
| U02 | Clicking a hex digit or ASCII byte positions the corresponding cursor/nibble. This is a deliberate usability improvement over the tested MC behavior. Padded cells do not select nonexistent bytes; horizontal cropping is accounted for, and edits remain staged until Save. |

Normal Release verification covered the core models, guarded audit regressions,
viewer navigation, panel prompts, operation UI/controls and all 25 terminal
audit cases. The new cases cover both Copy and Move at 40x12, main menus at
80x12, hex wheel/progress, pointed byte/nibble/ASCII edits, horizontal cropping,
all four Go to modes, and correction after invalid percentage input.
Successful terminal captures are retained in this session's
`ui-fixes-20261018` artifacts. No new dependencies, sanitizers or optional
profiling were used.

## UI and usability follow-up (`4333955`)

The previous fix delivery closed F01-F07, not the entire backlog. E01-E05
remain open: linked-file hex saves, search origin, failed-wrap positioning,
listing filename identification and final-byte progress. U03/U04 and the
sort/hotlist capability gaps were not fixed by that delivery either.

This round investigated **12 paired workflows** against the current Release and
installed MC 4.8.30, with 24 canonical application records and 18 additional
fresh-profile confirmation attempts. It found **five new mcpp bugs**, a shared
MC limitation, and a text-mode extension of existing E02. It did not find 12
passes, and it did not change product code.

### New confirmed mcpp findings

| ID | Priority | Failure and consequence | MC control and current code |
|---|---|---|---|
| G01 | P2 | Searching replaces a persistent block with the search hit. F8 then deletes the hit rather than the block the user marked. With a rectangle, the mark also changes from `C` to `B`. | UX-08/UX-09 saved exact differing bytes: mcpp removed `NEEDLE`; MC retained the original mark and removed `KEEP` or its marked columns. `EditorModel::find()` (`src/editor.cpp:1948-1956`) writes the found range into `selection_begin_`/`selection_end_` and clears column mode. |
| G02 | P2 | Ordinary Down navigation can scroll a text viewer into a completely blank EOF viewport, even when the entire short file already fits. It also scrolls the final page of a longer file away. | UX-03/UX-04: MC retains the three visible lines or the final page containing `LAST`; mcpp shows no file rows. `ViewerModel::scroll_lines()` (`src/viewer.cpp:197-218`) advances to `offset_ == size_`, leaving no rows for `lines()` to render. |
| G03 | P2 | Find results with the same basename under long differing parents render as identical paths. The user cannot tell which result will be activated. | UX-10: MC's grouped headings visibly retain `alpha` and `beta`; mcpp drops both parent tails. `path_label()` (`src/app.cpp:584`) keeps the beginning of the directory prefix plus the basename, and `render_search_results()` (`:6245`) uses it for each result. This is separate from E04's ordinary listing filename cells. |
| G04 | P2 | Displayed editor-menu mnemonics are ignored. F9, `s`, Enter opens the File dropdown instead of Search. | UX-07: MC opens its Search dialog. The editor renders underlined category letters (`src/app.cpp:4558-4570`), but `handle_editor_menu()` (`:8406`) has no character dispatch. F03 fixed the separate main-menu handler, not this one. |
| G05 | P3 | Clicking outside a main dropdown does not dismiss it, leaving the menu active until keyboard cancellation. | UX-05: a right-pane click closes MC's File menu but leaves mcpp's open. `handle_pulldown()` (`src/app.cpp:9753`) handles menu hits without cancelling an outside left press. The analogous editor outside-click control works in both programs. |

G01 was checked using the actual saved bytes, not selection colors alone.
For the linear fixture `KEEP and NEEDLE and TAIL\n`, F8/Save after searching
produced `KEEP and  and TAIL\n` in mcpp versus ` and NEEDLE and TAIL\n` in MC.
The rectangular fixture likewise removed the search hit only in mcpp, while
MC removed the originally marked columns from both lines.

### Existing finding extended, and shared limitation

- **E02 also affects text repeat search.** UX-11 finds the first `HIT`, manually
  scrolls to `ROW_060`, then repeats. mcpp jumps backwards to `ROW_050`; MC finds
  `ROW_100` ahead. `src/viewer.cpp:829-830` chooses cached
  `next_search_offset_` without accounting for the manually viewed region.
  This is additional coverage of E02, not a sixth new mcpp finding.
- **U05, P2 shared limitation:** successful text search does not reveal a far
  match on a long logical line in either program. UX-01/UX-02 place `NEEDLE`
  at byte 6506; wrapped and unwrapped searches leave it outside the viewport.
  Normal search was explicitly selected, and no error modal remained.
  mcpp resets to the logical line start and horizontal column zero
  (`src/viewer.cpp:894-916`). Reference MC has the analogous line-start path
  in `src/viewer/move.c:396-414`, whose scroll-to-cursor helper (`:73-90`) only
  handles hex mode. This is an improvement opportunity, not a mcpp-only
  parity regression.

### Workflow outcomes and evidence

| Case | Outcome | Observation |
|---|---|---|
| UX-01 | U05 shared | Wrapped far match stays offscreen in both programs. |
| UX-02 | U05 shared | Unwrapped far match stays offscreen in both programs. |
| UX-03 | G02 | Short-file Down scrolling reaches a blank mcpp viewport. |
| UX-04 | G02 | Down after the final page hides all mcpp file rows. |
| UX-05 | G05 | Outside click leaves the main dropdown open only in mcpp. |
| UX-06 | No new failure | Both dismiss the editor menu on an outside click. |
| UX-07 | G04 | Editor mnemonic selects the wrong subsequent menu/action only in mcpp. |
| UX-08 | G01 | Search retargets the persistent linear block; exact F8/Save bytes differ. |
| UX-09 | G01 | Search replaces the rectangle with a linear hit; exact F8/Save bytes differ. |
| UX-10 | G03 | Same-basename Find parents are distinguishable in MC, not mcpp. |
| UX-11 | E02 extension | Text repeat search ignores the manually advanced region in mcpp. |
| UX-12 | No new failure | Both retain the current match and text after cancelling a new query. |

Every new finding was repeated with fresh fixtures/profiles. The canonical
result/action JSON, ANSI/text captures, file metadata and saved byte outcomes
are retained in this session's `usability-followup-4333955/evidence/`.
All 12 paired screenshots and the additional before-delete rectangular-mark
comparison were opened and inspected; their selected sources are recorded in
`screenshot-manifest.json`. `case-results.json` groups the seven new mcpp-only
workflow failures into five bugs rather than counting duplicates; the shared
limitation and existing E02 extension remain separate.

Both binaries ran as UID 1001 in an owned Linux/arm64 disposable container with
no host mounts or network. Test setup copied the installed MC runtime/resources
after its missing-library preflight failure; no project dependencies or packages
were installed. The normal Release SHA-256 remained
`17f624e181db734b9d2300100fb7ed9d2ebf049e4992b8831c6b564b1a4929d9`;
MC 4.8.30's was `35a55f42a97c6685b950539d9ce1bcc9959be1049bc0cbb89d1525025fc4a7c8`.
No sanitizer, profiling or optional stress run was used. The earlier five
E findings and the original 100-case count were not counted again.

## Seven-finding fix delivery

All seven confirmed mcpp bugs F01-F07 from the 100-case investigation are fixed.
The historical findings and baseline line numbers are retained below rather
than rewritten as if the original investigation had passed.

| Finding | Delivered behavior |
|---|---|
| F01 | Quick cd resolves `.`/`..`, nested relative paths and absolute paths from the active pane. Directory spelling is normalized before provider validation so `.` retains both the cwd and a logical symlink leaf; failed navigation leaves the original directory intact. |
| F02 | The command prompt and status escape control characters in paths before fitting/rendering. A newline cwd remains one displayed row, and pointer placement still edits the intended command byte. |
| F03 | F9 followed by a displayed main-menu mnemonic opens that category, case-insensitively. The displayed labels supply the keys; the command draft is neither changed nor executed. |
| F04 | The viewer retains the actual matched byte range and maps it to rendered text. Full-screen and Quick view highlight the occurrence without splitting a displayed glyph; tabs, wrapped rows and horizontal clipping retain the correct span. |
| F05 | Editor word movement retains the tested punctuation stops and stops at the previous EOL when crossing backwards. The complete forward/reverse ASCII/underscore/hyphen/UTF-8 sequence agrees with the recorded MC control. |
| F06 | Shift-click resumes an existing mark without losing its anchor or rectangular mode. Visible horizontal coordinates determine the new endpoint; a subsequent rectangular delete/save changes only the expected columns. |
| F07 | A selection dragged beyond the visible top/bottom moves to the next document row and scrolls it into view. Returning inside uses the new viewport; BOF/EOF remain bounded. |

Seven new cases bring `tests/test_audit_ui.py` to **32 cases**. Focused core and
editor regressions, the guarded audit model checks and all 32 terminal audit
cases passed. The exact seven original reproducers also passed against the
normal Release binary, SHA-256
`17f624e181db734b9d2300100fb7ed9d2ebf049e4992b8831c6b564b1a4929d9`.
Release was rebuilt with warnings as errors and `--parallel 12`; it remains in
`build/release/mcpp`, not installed.

All application/filesystem checks ran as UID 1001 in the owned disposable
container with no host mounts or network. Final action/capture/result files,
byte-outcome assertions, build/test logs and seven inspected paired screenshots
are retained in this session's `f01-f07-fixes` artifacts. Screenshot comparisons
use the retained canonical MC 4.8.30 controls; no new native MC run or complete
parity claim is implied.

This delivery does not close the earlier E01-E05/U03 findings, shared limitation
U04, the missing sort/hotlist capabilities, blocked controls or the unresolved
history decision. The original 100-case totals remain a historical record.

## Investigation of the 100 additional cases

All UI-001 through UI-100 from the
[execution plan and per-case results](mc-parity-ui-test-plan.md) were
investigated against unchanged product code `e28a00d`. This is not a claim of
100 passes: 72 cases completed without a new mcpp fault, seven exposed confirmed
mcpp bugs, six had approved/safety differences, six were blocked by missing
sort UI, five lacked an equivalent MC Tree control, two MC completion controls
remained inconclusive, one history-branch difference needs a decision, and one
Unicode-mask limitation is shared with MC.

The normal Release SHA-256 remained
`c74838f79ff30ee4e8b9fe8516677b4a77f2744fd68378d6aff9a5420e270211`;
MC 4.8.30's was `35a55f42a97c6685b950539d9ce1bcc9959be1049bc0cbb89d1525025fc4a7c8`.
Both applications ran non-root in an owned Linux/arm64 container with no host
mounts or network. MC controls used the installed runtime/resources, with its
internal editor enabled explicitly where needed. No project dependencies,
package installs, sanitizer checks or optional profiling were added.

### Confirmed findings

| ID | Case | Priority | Failure and evidence | Relevant code |
|---|---|---|---|---|
| F01 | UI-009 | P2 | Relative Quick cd uses the wrong base. Entering `.` from fixture B takes mcpp to `/`; MC stays in B. This is the Quick cd dialog, not the previously fixed commander's `cd` builtin. | `src/app.cpp:3686-3689` passes the raw relative value to `Pane::change_directory`; `src/local_provider.cpp:395-419` resolves relative paths against the provider root. |
| F02 | UI-002 | P2 | A newline in the cwd is escaped in the pane title but emitted raw in the command prompt, splitting the prompt across lines. MC's directory display does not create that broken prompt. No command execution was attempted. | Raw path in `src/app.cpp:6752-6755`, passed through `command_text()` (`:492-515`) and `render_input()`'s unsanitized prefix (`:2703`). |
| F03 | UI-048 | P2 | Displayed top-menu mnemonics do not select/open their menus. With a command draft present, F9 then `f` does not open File; MC opens File and the requested Mkdir modal without changing the draft. | `handle_pulldown()` (`src/app.cpp:9707-9803`) handles mouse, arrows and Enter but not mnemonic characters. The top menu still renders mnemonic styling. |
| F04 | UI-067 | P2 | A successful text-viewer search gives no visible match span, so repeated occurrences within one displayed line cannot be distinguished. MC highlights the current `aa` in `aaaaa`. This prevents full visual verification of the planned overlap sequence. | `src/app.cpp:7411-7423` consumes the search result as a boolean; `render_viewer()` (`:3949-4005`) only renders a special cursor/span in hex mode, otherwise plain text. |
| F05 | UI-082 | P2 | Editor word movement skips MC's punctuation and reverse line-end stops. First Ctrl-Right in `foo_bar-baz` stops at byte 8 in mcpp versus byte 7 before the hyphen in MC. Ctrl-Left from the next line also skips the previous EOL stop. | `src/editor.cpp:543-585` skips the word and following separators in one forward command, and continues scanning after crossing a line in reverse. |
| F06 | UI-084 | P2 | Shift-click converts an existing rectangular mark into a linear mark. After horizontal navigation, mcpp shows block flag `B`; MC retains column flag `C`. Subsequent block actions would use a different shape. No deletion/publication was used in this probe. | `src/app.cpp:9180-9198` finishes the mark and calls the linear `begin_mark()` for Shift-click, then derives column mode only from the mouse modifier. |
| F07 | UI-087 | P2 | Dragging a selection beyond the visible editor edge clamps to the last rendered row instead of scrolling. MC reveals another row; mcpp's first visible row stays unchanged. | `editor_mouse_position(..., true)` (`src/app.cpp:9107-9137`) clamps to rendered line boxes; the drag route (`:9151-9175`) moves the cursor without scrolling the viewport. |
| U04 | UI-022 | P2 shared limitation | `?.txt` selects only ASCII `a.txt`, not the one-character Unicode basenames, in both programs. Actual mark totals are one file in each; normal colored MC filenames were not mistaken for marks. This is an improvement opportunity, not a mcpp-only MC regression. | mcpp's byte-oriented matcher (`src/pane.cpp:14-44`) advances one byte for `?`; `select_pattern()` (`:425-443`) uses it directly. |

Each confirmed discrepancy was repeated with fresh fixtures/profiles and
checked against the corresponding code. Paired decisive images are
`case-009.png`, `case-002.png`, `case-048.png`, `case-067.png`, `case-082.png`,
`case-084.png` and `case-087.png` in this session's `parity-ui-100` artifacts;
`case-022.png` and recorded mark totals support U04. Full ANSI/text captures,
action logs, applicable saved bytes/hashes, 200 per-application result records and
inspected contact sheets are retained under `evidence-canonical/` and alongside
`investigation-results.json`.

### Blockers and differences, not extra confirmed bugs

- The Sort order menu is disabled. The backend sort actions do not provide a
  substitute for reaching the feature through the UI; six planned cases are
  recorded as missing capability rather than passed.
- MC Tree fixture ancestry remained unsuitable after bounded setup attempts.
  UI-031 through UI-034 and UI-039 are blocked controls, not evidence of
  parity or newly broken mcpp Tree behavior.
- mcpp has no Add current hotlist action in this UI. Existing-entry navigation
  and missing-entry recovery were investigated with seeded bookmark fixtures;
  adding entries was not falsely counted as implemented.
- MC completion controls UI-043/UI-044 remain inconclusive. The history branch
  sequence in UI-008 ends in different directories; mcpp truncates forward
  history (`src/pane.cpp:315-329`), while MC keeps its unique history list.
  That behavior decision was not silently classified as a bug or approved.
- mcpp's Unicode filename-class matching and publication guards were not made
  worse to imitate MC. The tested MC overwrite/Undo/Redo sequence also produced
  invalid UTF-8 where mcpp preserved the intended bytes; this was recorded as a
  safer difference, not a mcpp failure.

The earlier E01-E05/U03 findings were neither fixed nor re-counted in these
100 cases. That investigation changed documentation only; the subsequent
F01-F07 fixes are documented above.

## Navigation, panes, viewer and editor walk (`e28a00d`)

This 2026-10-18 pass walked the current normal Release binary, rendered and
inspected screenshots, and traced the relevant handlers/models. The executable
SHA-256 was `c74838f79ff30ee4e8b9fe8516677b4a77f2744fd68378d6aff9a5420e270211`.
Actual MC controls used installed MC 4.8.30 with fresh HOME/XDG profiles.
Reference-source controls are identified separately; they are not runtime
claims about that binary.

Five additional failures and one shared editor limitation were recorded.
These remain open; this pass changes the report, not the product.

| ID | Priority | Finding and consequence | Runtime and code evidence |
|---|---|---|---|
| E01 | P1 | Hex Save silently splits hardlinked files. One pathname gets new bytes while its linked alias retains the old contents, without the editor's detach/preserve/cancel choice. | In a non-root disposable container without host mounts, F6 saved `edited.bin` as `ZBCD`, left `linked.bin` as `ABCD`, and changed one inode/link-count-2 group into two inode/link-count-1 files. The dirty indicator cleared without a decision or warning. `ViewerModel::save_changes()` (`src/viewer.cpp:485-498`) directly calls provider write, whose local publication replaces the inode (`src/local_provider.cpp:734-745,885-917`). The editor's link decision is at `src/app.cpp:7924`; the viewer F6 route at `:7570` has none. MC control is source-only here: reference `src/viewer/hex.c:369-404` opens the existing inode with `O_WRONLY`; no MC runtime hardlink-save case was run. |
| E02 | P2 | Hex forward/repeat search ignores the active byte after ordinary cursor movement and can jump backward without a wrap decision. | With matches at bytes 32, 160 and 480, a fresh search from cursor 192 returned 32 in mcpp; MC returned 480. After Ctrl-Home/Down to cursor 224, repeat again returned 32 in mcpp and 480 in MC. MC's search form was explicitly switched to Hexadecimal before comparing. `src/viewer.cpp:795-804` starts fresh search at the viewport offset and repeats at cached `next_search_offset_`; cursor movement (`:390-400`) does not reset that origin. |
| E03 | P2 | A failed wrapped search leaves the hex cursor outside the displayed viewport. | From EOF of a 4096-byte file, search for absent `FF FF`, accept wrap and acknowledge the failure. mcpp displays row `00000000` while its header still reports cursor 4095; no active byte is visible. MC restores the previous EOF viewport/cursor. The wrap route calls `go_to_start()` (`src/app.cpp:7411-7430`), which resets the viewport/search state but not `hex_cursor_` (`src/viewer.cpp:148-155`). |
| E04 | P2 | Detailed file listings discard distinguishing suffixes and extensions, making different entries look identical. | At 100x30, `identical_common_name_prefix_A.txt`, `..._B.py` and `..._C.log` all displayed as `identical_common_name...` in mcpp. MC's fitted names retained the distinguishing suffixes/extensions. The name cell uses head-only truncation (`src/app.cpp:3782-3806`, helper `:464-490`), unlike the fitted pane/header paths. The full current filename can be inspected in the status row, but the list itself cannot be scanned reliably. |
| E05 | P3 | Hex progress still reports 99% at the final byte. This is an uncovered endpoint case after D04's cursor-versus-viewport correction. | Ctrl-End put both programs at byte 4095 of 4096: mcpp showed 99%, MC 100%. `src/viewer.cpp:346-351` divides the active offset by the full size. Reference MC compensates the hex divisor by one (`src/viewer/lib.c:400-422`), consistent with the observed runtime result. |
| U03 | P2 shared limitation | CR-only Save As collapses the document into one raw editing line, making the next F8 remove the entire draft. | Saving `alpha\nbeta\ngamma\n` as CR changed the visible line count from four to one in both programs; the following staged F8 emptied both drafts. No save followed that deletion. mcpp showed concatenated `alphabetagamma`; MC showed `^M` markers. mcpp converts the bytes and reloads them (`src/editor.cpp:1785-1804,1833-1841`) through a parser that splits only on LF (`:395-415`). Retaining logical editing lines after output conversion would be an explicit improvement over this tested MC behavior, not a new mcpp-only regression. |

### Screenshots, coverage and exclusions

The `ui-walk-20261018-round3` session artifacts retain the drivers, text/ANSI
captures, result JSON, isolated hardlink metadata and rendered screenshots.
Inspected images include:

- `hex-save-hardlinks.png`: before/after F6 and the linked-file byte/inode outcome.
- `hex-search-confirmed-first-match.png`: byte 32 versus byte 480 from the same
  active search position.
- `wrap-confirmed-after-failed-wrap.png`: stale hidden cursor versus restored
  EOF position.
- `filename-list-screen.png` and `hex-search-confirmed-last-byte.png`: filename
  identification and the remaining progress endpoint.
- `editor-conversion-after-save-as.png`: the shared CR-format editing limitation.
- `editor-long-before-click.png` and `quick-view-resized.png`: passing controls,
  not additional failures.

The walk also covered Quick cd, directory history back/forward, Quick view
focus/scrolling, pane swapping, resize/return to listings, and Info pane
activation/resize/return. The editor's horizontal scrolling and click-to-place
were compared with MC using a numbered long line: both displayed the same
visible content and saved the inserted character at the expected byte 82.

Directory-history return reset the selected file to `..` in both programs.
That was not counted as an additional mcpp-only defect. Nor were intentional
colors, paused providers or previously recorded disabled-feature groups.
The hardlink publication probe stayed in an owned non-root container without
host mounts; other writes were ordinary saves to new/private editor fixtures.
No sanitizers, optional profiling or new dependencies were used. This is
bounded workflow coverage, not a pass of every MC action or filesystem case.

## Follow-up UI walk: 2026-10-18

This pass used the normal Release binary from product code `7a4370e`, after the
Tree upward-loading fix, and the actual installed GNU MC 4.8.30. Its mcpp SHA-256
was `d5de05345711bbf591d68de892288558c2ff937e1ab45885f41e3d5b559c5048`.
Each terminal used private HOME/XDG directories and owned fixtures. MC runtime
results below are not assertions about the newer reference build.

The walk found four UI defects, one partial viewer feature, and one shared
usability limitation. It originally changed only the report; the findings were
subsequently corrected by the delivery above. The observations and source
anchors below describe `7a4370e`, not the corrected code. P2 denotes a normal
workflow/usability problem; P3 denotes misleading display information without
an observed file change.

| ID | Kind / priority | Finding and consequence | Direct comparison and source evidence |
|---|---|---|---|
| D01 | Defect / P2 | At 40x12, Copy hides its permission/link/background options and both action buttons. Wheel scrolling and eight Tabs do not bring them into view. | MC keeps OK, Background and Cancel visible at the same size. mcpp's six-Tabs/Enter sequence can still cancel: the finding is absent visible/pointer access, not permanently unreachable keyboard actions. `render_operation_dialog()` is a fixed, unframed vertical form (`src/app.cpp:5790-5865`); Move uses the same renderer but was not separately exercised. |
| D02 | Defect / P2 | Main dropdown menus clip their selected bottom action on short terminals. At 80x12, Options > Save setup is invisible even after Up selects it. | MC shows the selected Save setup at that size. Enter executes the hidden mcpp action and creates its private config, proving this is a visibility problem rather than a disabled feature. `render_pulldown()` has no height-bounded `yframe` (`src/app.cpp:5321-5357`); the editor's separate File menu does scroll correctly. |
| D03 | Defect / P2 | In hex mode the wheel moves the byte cursor but leaves the viewport stationary while that cursor is still visible. The first wheel event therefore does not scroll the displayed bytes. | On a 4096-byte file, mcpp advances the cursor to decimal 48 but keeps `00000000` as the first row. MC scrolls its first row to `00000020`. The problem is stationary content, not the different wheel-step sizes. The hex wheel route calls cursor movement rather than viewport scrolling (`src/app.cpp:7402-7453`). Text-mode wheel scrolling works. |
| D04 | Defect / P3 | Hex percentage describes the end of the visible range while the adjacent offset describes the active byte, giving conflicting position information. | On a 261-byte file, mcpp shows `@0/261 100%` at the first byte and still 100% after Down to byte 16. MC shows 0% then 6%. `render_viewer()` computes percentage from `view_end()` in both modes but displays `hex_cursor()` in hex mode (`src/app.cpp:3963-3986`). |
| D05 | Partial feature / P2 | Viewer Go to only accepts byte offsets; normal text-line and percentage navigation are missing. | In fresh MC, Go to offers Line number, Percents, Decimal offset and Hexadecimal offset; entering 20 in its default line mode shows `ROW_019`. mcpp explicitly labels its byte-only prompt and enters the middle of `ROW_000`. This is a missing choice, not incorrect byte-offset parsing (`src/app.cpp:7478-7482`). |
| U02 | Shared limitation / P2 | Clicking a hex byte does not place the edit cursor at that byte. Users must position it with keys before typing. | Clicking the byte at offset 0x10 and typing `a` changes the staged high nibble at the old offset 0 in both tested programs. This is not a new mcpp-only MC regression and no save was issued. mcpp only handles header clicks and wheel events in the viewer body route (`src/app.cpp:7431-7454`); the reference MC mouse callback scrolls rather than mapping a click to a byte (`reference/mc/src/viewer/mcviewer.c:83-180`). Click-to-place editing would be an explicit usability improvement. |

### Walk coverage and controls

- Panels, Tree, Copy/Delete/Mkdir prompts, menus, full-screen viewer and editor
  were opened directly in both programs, including 40x12 and 80x12 layouts.
  The comparison also exercised scrolling, EOF positioning, text/hex display,
  dirty staged hex edits, and viewer Go to.
- Normal viewer EOF and editor document-end screens agreed for the owned long
  text fixture. The editor's Search/Replace actions stay visible at 40x12,
  and its File menu scrolls to Quit. These were not reported as remaining
  failures merely because other forms and menus clip.
- Clicking after the wide character in command text `a界b` and typing `X`
  produced `a界Xb` in both programs. No UTF-8 caret failure was observed there.
- mcpp's Tree loaded the parent/grandparent's immediate folders while retaining
  its existing child branch. The cold MC Tree did not provide an equivalently
  populated ancestry in this profile, so this pass does not claim equivalent
  upward-navigation coverage from that MC control.
- Copy was cancelled, not accepted; delete confirmations were not accepted.
  Hex edits remained staged, and the original byte fixtures and empty copy
  destination were checked unchanged. No destructive/error-injection operations,
  sanitizer checks or optional profiling were run.

Drivers, exact text/ANSI captures, result JSON and the inspected paired PNGs
(`copy-dialog.png`, `main-menu.png`, `hex-wheel.png`, `hex-percentage.png`) are
retained in this session's `ui-walk-20261018` artifacts. The walk is bounded
coverage, not a complete pass of the MC keymap or all enabled actions.

## Historical source audit (`14b7a82`)

The source comparison uses mcpp code at `14b7a82` and MC reference commit
`b2706063b0e3abe9b7a6b7edef0a63e512dfb28c` (2026-10-16; its NEWS identifies
the 4.8.33 series). The installed runtime control is MC 4.8.30, not a build
of that reference commit. Reference-version changes must therefore not be
inferred from the older binary alone.

MC paths below are relative to `reference/mc/`; mcpp paths are relative to
this repository. These sections record the findings before the fix batch above.
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

### Baseline local capability inventory

These are capability gaps, not evidence that already implemented operations fail.
Some overlapping items, including copied hardlink groups, compare methods and
viewer hex editing, were subsequently fixed with the numbered findings.
The remaining gaps are described under Current local functionality below;
a disabled entry is not treated as a completed feature.

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

## Additional audit: operation policy, replacement and save metadata

This extends the source audit against the same product code; the previous
report commit `2f5c6eb` changed documentation only. The following B findings
are additional to A01-A16. Error/destructive cases were reproduced in a
non-root disposable container without host mounts. Editor model probes and
actual terminal workflows are identified separately.

| ID | Priority | Additional finding | Evidence and consequence |
|---|---|---|---|
| B01 | P1 | A directory-merge decision becomes blanket permission to overwrite child files. Even choosing Older can replace a newer destination with an older source and then delete the source tree. | `src/operations.cpp:864-878,938,1213` passes the root's accepted overwrite into the fallback copy; `should_overwrite():326-327` then bypasses every child resolver. A model resolver that would skip children was never called. In the actual F6/Older workflow, `NEWER-TARGET` became `OLDER-SOURCE` and the source tree disappeared. MC keeps the per-file Older check in `src/filemanager/filegui.c:1262-1267`. Permission to merge a directory must not mean All. |
| B02 | P1 | Chmod ignores marked targets and can change `..` instead. Chown has the same target-selection code path. | `src/app.cpp:3165-3225` reads `active().current()` at acceptance rather than the marked entries. With `a.txt` marked and the cursor on `..`, chmod 0700 left the file at 0644 and changed its parent directory from 0755 to 0700. MC uses `panel_get_marked_file()` in `src/filemanager/chmod.c:514` and `chown.c:388`. This is a wrong-target bug, not just a missing bulk-dialog option. Chmod was reproduced; Chown is source-confirmed. |
| B03 | P1 | Saving a file discards its user xattrs and POSIX access ACL, even though the displayed mode bits stay unchanged. | `src/editor.cpp:1554-1580` uses `LocalProvider::write`, which creates a fresh inode, copies mode bits, and renames it into place (`src/local_provider.cpp:664-826`). A real editor save removed both `user.mcpp_audit` and `system.posix_acl_access` while keeping mode 0640. A named user's access grant was lost. MC's default quick-save path writes the existing inode (`src/editor/editcmd.c:210-247`). Preserve appropriate metadata without blindly retaining executable capabilities. |
| B04 | P1 | Ctrl-K destroys the persistent mark before recording Undo. Undo restores the bytes but not the mark, so a subsequent F8 deletes the whole line rather than the restored block. | `src/editor.cpp:1149-1161` calls `clear_selection()` before `record_undo()`. Reproduced with `alpha beta` marked at `alpha`: after Ctrl-K/Undo/F8, mcpp saved only `gamma`; MC saved ` beta` followed by `gamma`. MC deletes through the normal edit/undo machinery in `src/editor/edit.c:1380-1384`. |
| B05 | P2 | Replacement can reject the valid regex match that its own selection-scoped search just found. | `EditorSearch::replacement()` (`src/editor_search.cpp:667-700`) reruns the expression through the rest of the document, not the original search boundary. For `abc def`, `.*` restricted to `[0,3)` matches `abc`; expanding the replacement then throws `selected text no longer matches the expression` despite no edit. Preserve the original match/captures and bounds. MC's replacement consumes the search engine's recorded match (`src/editor/editsearch.c:974`), rather than using mcpp's re-search procedure. |
| B06 | P2 | Valid zero-width regexes are discarded. Finding/replacing a line-start anchor such as `^` cannot work. | `src/editor_search.cpp:397-417` rejects every equal begin/end match; the editor also represents an empty match as no selection. The model returned zero matches for `^` in two nonempty lines. MC's actual Replace accepted `^` and inserted `>` at the first line start; its continuation code explicitly handles zero-length matches (`src/editor/editsearch.c:1000`). |
| B07 | P2 | Editor wildcard matching/replacement does not implement MC's greedy captures, and regex replacement escapes remain literal. | `src/editor_search.cpp:292-362,667-754` matched only `a1b` for `a*b` in `a1b2b`, returned literal `x*` as replacement, and returned bytes `5c6e` for regex replacement `\n`. MC's actual wildcard replacement produced `x1b2`, and regex `\n` produced byte `0a`. Compare MC `lib/search/glob.c:50-126,130-205` and `lib/search/regex.c:711-716`. These are enabled replacement modes, not disabled optional actions. |
| B08 | P2 | Atomic-save permission semantics are not exposed or checked: a file opened with mode 0400 can be changed by an ordinary Save without a read-only warning. | The non-root model probe opened the already-read-only file, inserted `X`, and `save(false)` replaced it successfully. `LocalProvider::write` checks directory publication and mode copying but not file writability (`src/local_provider.cpp:664-826`). MC's default quick save opens the existing file for writing and falls back to Save As on failure (`src/editor/editcmd.c:243-245,443`). This is a missing permission/mode decision, not a claim that Unix forbids an owner from replacing a directory entry. |
| B09 | P2 | Copy onto a different hardlink of the same file silently breaks the link and reports completion instead of identifying the same-file operation. | `src/operations.cpp:525-532` checks path equality for copy, while the stronger identity helper is only used by moves. The model copied `source` onto its hardlinked `alias`; the names ceased to share an inode and the result reported one completed source. MC checks device/inode before overwrite (`src/filemanager/file.c:911-921,2396`). This differs from preserving a group of hardlinks during an ordinary copy. |
| B10 | P2 | Ctrl-Right skips across the newline when the last word ends at EOL, instead of first stopping at the end of that line. | `src/editor.cpp:530-573` unconditionally advances to the next row after reaching EOL. With `alpha\nbeta\n`, Ctrl-Right then `X` produced `alpha\nXbeta\n` in the model; MC's same keys produced `alphaX\nbeta\n`. The UI routes Ctrl-Right directly to this model method. |
| B11 | P2 limitation | Near the supported file-size limit, the undo budget retains only a few keystrokes rather than a useful history. | `src/editor.cpp:1474-1510` stores entire documents per edit and limits all snapshots to 64 MiB. On an allowed 16 MiB document, six inserted characters left only three undo steps. MC records edit actions rather than whole-document snapshots and defaults to a much larger action history (`src/editor/edit.c:108,1359-1375`). This is an observable capacity limitation, not a profiling or micro-optimization finding. |

### Additional shared hazard, not an MC-only miss

**H03, P1: regex and wildcard character searches can split UTF-8 and save
invalid text.** Searching `.` in regex mode or `?` in wildcard mode against
`é` selected only byte `c3`. Replacing one match with `X` saved
`58 a9 0a` (`X`, a stray UTF-8 continuation byte, newline). Both editor model
probes and actual mcpp F4/One/F2 workflows reproduced this.

The regex path does not select a UTF-8 locale, unlike the normal literal
search's `Utf8CType` guard; wildcard matching advances bytes directly
(`src/editor_search.cpp:214-278,292-362,366-429`). Hex-mode byte editing is not
the issue: these are the character-oriented regex/wildcard modes.

The installed MC 4.8.30 control also produced those invalid bytes in the
tested profiles. Its result is therefore not a correctness oracle, and this
is not claimed as a runtime-certified difference from reference MC 4.8.33.
The reference search engine has explicit UTF-8 versus raw-codepage paths
(`lib/search/regex.c:790-823`). mcpp should protect valid UTF-8 independently
of reproducing an older MC hazard.

### Control outcomes and limits of this pass

- MC controls confirmed the Ctrl-K/Undo mark difference, zero-width
  replacement, greedy wildcard capture replacement, regex escape expansion,
  and the word-motion line boundary.
- The selection-scoped greedy MC control also consumed a boundary byte.
  That result is not a reason to expand mcpp's selection. B05 is the
  independently reproduced inconsistency between mcpp's own match and its
  replacement validation.
- Whole-word matching beside a nonbreaking space and editing at a CRLF line
  end showed comparable limitations in the tested MC setup. They were not
  counted as additional MC parity misses.
- The temporary probes, exact saved bytes, UI captures, and metadata results
  are retained in the session's `mc-source-audit/more` artifacts. This pass
  changed the report, not the product, and used no sanitizers or profiling.

## UI and usability audit

This pass exercised visible controls rather than inferring usability from
keyboard-only tests. Normal terminal probes used the unchanged product code
from `14b7a82`; the later commits up to `467cefc` added audit reports.
Screenshots were rendered and inspected, and control outcomes were compared
with MC 4.8.30 and the reference source where applicable.

Related failures are grouped by interaction, not counted once per button.
The following ten C findings are additional to A/B/H findings.

| ID | Priority | UI/usability failure | Evidence and interpretation |
|---|---|---|---|
| C01 | P2 | The function-key bars look clickable but ignore mouse activation. | Main Help, editor Save and viewer UnWrap clicks did nothing, while the corresponding keyboard actions work. The keybars are rendered as elements without button hit routing in `src/app.cpp:3430-3530,3990-4040,6106-6195`. MC's actual editor keybar opened Save, and its buttonbar routes clicks in `lib/widget/buttonbar.c:219-235`. |
| C02 | P2 | Basic dialog behavior is inconsistent: visible No/Cancel/OK buttons ignore clicks, Space does not activate focused buttons, and F10 is not a reliable modal cancel. | Clicks were ignored on delete, dirty-editor close, replace confirmation and error acknowledgement. Space failed on delete/dirty-close; F10 failed on delete/generic Save As. Clicking No left the default Yes focus unchanged; no destructive acceptance was issued by the probe. Save confirmation already supports mouse/Space/F10, so this is inconsistent within mcpp as well as unlike MC's standard widgets. See `handle_delete_dialog`, `handle_editor_close_dialog`, `handle_editor_replace_confirm`, `handle_prompt`, and the editor-error dispatch in `src/app.cpp`. MC mouse Cancel closed the dirty-editor dialog. |
| C03 | P2 | Prefilled fields are not initially selected for replacement. Starting to type a new name appends it to the existing value. | In F12 Save As, typing `renamed.txt` produced `/.../original.txtrenamed.txt`; the MC field became just `renamed.txt`. `TextInputState::reset()` at `src/app.cpp:707-715` clears the anchor and puts the caret at the end. MC's first-edit handling deletes the initial marked region (`lib/widget/input.c:1112-1118`). Generic prompts and copy destinations need deliberate initial-value selection, without changing normal history/cursor editing. |
| C04 | P2 | Clicking the command line does not position the caret or select text, although dialog inputs support those mouse operations. | After typing `abcdef`, clicking between `b` and `c` and typing `X` produced `abcdefX`, not `abXcdef`. `command_input` is rendered through the shared input renderer, but the panel mouse path does not call `input_mouse()` for it (`src/app.cpp:6176-6188,9287-9470`). MC's input widget sets the caret and drag mark in `lib/widget/input.c:906-939`. |
| C05 | P2 | Full-screen viewer mouse-wheel scrolling is a no-op. | Wheel Down over the viewer left `ROW_000` at the top; Arrow Down moved to `ROW_001`. Quick view and the editor already handle the wheel, making the same file behave differently between views. `handle_viewer()` in `src/app.cpp:6572-6660` has no mouse path. MC handles viewer scroll events in `src/viewer/mcviewer.c:169-184`. This is distinct from the previously recorded missing keyboard aliases. |
| C06 | P2 | Search results advertise normal View/Edit actions but do not implement them. | The results screen keeps an enabled-looking F3 View/F4 Edit keybar; pressing either leaves the results screen unchanged. `handle_search_results()` at `src/app.cpp:8665-8696` only supports arrows, Enter and leaving. Users must navigate to the file's directory to inspect it. MC exposes View-F3/Edit-F4 in `src/filemanager/find.c:192-193,1596-1620`. Implement the workflow and return to the same result/cursor afterward; merely retaining an unrelated main keybar is misleading. |
| C07 | P2 | Long paths obscure the identifying filename, including during an unsaved-file decision. | At 100 columns the editor header showed only a common directory prefix. With a long path, neither the header nor the Close-file dialog showed a recognizable part of the basename. The tested MC header retained a distinguishing `~saved-document.txt` suffix even though its close-dialog text was also clipped. mcpp uses a full-path text element with `flex` in `render_editor()` and a nonwrapping full-path line in `render_editor_close_dialog()`. Viewer and pane titles also favor a leading path prefix. Prioritize the basename/distinguishing suffix and make the full path available separately. |
| C08 | P2 | File-pane scrolling recenters prematurely instead of keeping the view stable while the cursor is still visible. | With an 80-file list at 100x30, fifteen Down presses made mcpp's first visible file `item_004`; MC still showed `item_000`. `render_pane()` rebuilds a focused `yframe` without a retained top row (`src/app.cpp:3250-3414`). MC adjusts its top row by as little as necessary (`src/filemanager/panel.c:1313-1351`). The earlier editor viewport correction did not change listing panes. |
| C09 | P2 | Auxiliary lists are keyboard fragments rather than fully interactive lists. | Clicking a different buffer-list entry did not select it; Enter kept the current empty buffer. `handle_editor_list()` at `src/app.cpp:7428-7512` has no mouse or PageUp/PageDown handling. Hotlist and search-result list handlers likewise lack normal pointer selection/scroll behavior (`:8622-8696`). MC's listbox supports mouse selection, scrolling and page commands (`lib/widget/listbox.c:300-303,510-566`). Buffer-list clicking was reproduced; the related list routes were source-checked. |
| C10 | P2 | Short-terminal Search/Replace forms hide actions and options without a visible scrolling cue or mouse access to the hidden area. | At 40x12, initial F7/F4 screens omitted OK, Cancel and lower options. Wheel Down did not move the form. Repeated Tab did reveal the buttons, so they are not permanently unreachable: this is a discoverability and mouse-access failure, not absence of all scrolling. The form has a focus-following `yframe` (`src/app.cpp:4289-4410`) but the mouse handler does not scroll it. Keep primary actions visible or provide explicit scrolling and usable pointer/page controls. |

### U01: Tree pointer behavior is a shared limitation, not a new MC regression

Single-clicking `beta` while `left` was selected left `left` selected, and
wheel scrolling changed neither the selection nor the view. The implementation
focuses the pane on a single click and selects/opens a node only on a double
click (`src/app.cpp:9340-9394`).

The reference MC tree does essentially the same thing and leaves wheel handling
as a TODO (`src/filemanager/tree.c:1212-1265`). Its behavior is not a reason to
call this usable: single-click selection and wheel navigation would be reasonable
improvements. They should be explicit choices beyond MC parity, not falsely
reported as existing behavior. The previous description saying a single click
"focuses a node" overstated the implementation.

### UI audit controls and next fixes

- MC controls confirmed clickable keybars and Cancel buttons, initial replacement
  of a prefilled filename, and stable pane scrolling.
- A one-field Save As prompt remained usable at 40x12. It was not counted as a
  failure merely because its source requests a width of 72. The layout finding
  concerns longer forms, hidden actions, and absent pointer scrolling.
- Search and Replace did scroll when keyboard focus reached hidden buttons.
  The report does not equate an initially clipped screenshot with permanently
  inaccessible controls.
- Exact input events, before/after text, ANSI captures, inspected PNGs and result
  JSON are retained in the session's `mc-ui-audit` artifacts.

Prioritize reliable visible controls and modal dismissal first, then input
defaults/caret positioning and search-result inspection, followed by filename
legibility, stable scrolling, list navigation and constrained layouts. Reuse
consistent keyboard/mouse activation rules across dialogs instead of treating
each text-rendered button as an unrelated special case. Tree pointer changes
need an explicit usability decision. No product behavior was changed by this
audit.

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

## Historical local functionality snapshot (`e28a00d`)

This snapshot predates the current audit fixes and native-control rework above.
Its missing-feature lists describe that older baseline, not the current binary.

### Panels and menus

mcpp supports two local listings, parent navigation and history, sorting,
hidden files, case-sensitive filename filters, masks, selection, Quick view,
live Info, Tree, pane swapping, hotlist, and filename-search panelization.

Tree opens at the chosen pane's cwd. Enter opens the selected directory in
the opposite listing and expands it. Right expands/moves to a child; Left
selects the parent. Space toggles, `+` expands, and `-` collapses. A single
click focuses the Tree pane and selects the node; a double-click opens it.
The wheel moves the selection. Visiting a partially loaded ancestor lists its
immediate folders asynchronously, preserving the selected path and expanded
descendants without recursively scanning unvisited siblings.
Explicit collapse controls, Enter expansion, and Tree pointer improvements are
deliberate mcpp behavior, not an assertion that all MC Tree modes use the same
controls.

Unavailable listing formats, multi-column layouts, split/resizing controls,
directory-size calculation, codepage selection, full help, key learning, and
custom keymaps remain gaps. Grayscale chrome is intentional.

Dialog text inputs support cursor/word editing, Shift selection, mouse positioning
and dragging, kill/yank, and session history. Filename/directory and operation
destination inputs provide path completion with ambiguous-match cycling.
Editor search/replacement and numeric options use the same editing controls.
Prefilled defaults are selected for replacement without changing the editing
behavior of history and command drafts. Search/Replace buttons stay visible
on short terminals while their option body scrolls.
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
at once and job history is session-local. Initial measurement is an estimate;
unreadable entries reach the actual operation's Retry/Skip controller rather
than bypassing it. Recursive-delete choices, append/reget, and resuming a
partially copied file remain gaps; Retry restarts the failed file.
Copied modification times and hardlink groups are retained. Creation modes
respect umask when Preserve permissions is off. Broader copy-metadata options,
sparse layout, and every failure/rollback combination are not claimed complete.

Find and filename panelization are asynchronous and cancellable. They are
not an external-command panelizer. Find retains accessible results and reports
unreadable paths; result View/Edit returns to the same list position. Directory
comparison offers Quick, Size only and Thorough, with directional marks.
Saved external panelize commands, advanced Find options/context and diff/merge
remain unavailable.

### Viewer

The viewer has its own full-screen header/keybar, bounded range reads,
text/hex display, wrap, horizontal scroll, Home/End, mouse wheel,
line/percentage/decimal/hex Go to, forward literal/hex search, and repeat search.
Hex/ASCII edits are staged until Save,
with explicit save/discard/cancel decisions. Raw/nroff, format, bookmarks,
backward/regex search, next/previous file, codepages, and screen selection
remain unavailable.

Repeat search asks before wrapping. Standalone `--view` Quit exits rather
than returning to panels. Hex view uses aligned 16-byte rows.
Hex wheel scrolling moves the displayed rows immediately; the percentage follows
the active byte. Hex/ASCII clicks position the byte/nibble cursor, including
after horizontal cropping.
In the tested far-right unwrapped search, neither program automatically
revealed the match. That is not counted as a mcpp-only defect.

### Editor

mcpp has switchable buffers, per-buffer editing/search state, persistent file
history, linear/rectangular blocks, persistent clipboard, overwrite,
undo/redo, preferred display columns, horizontal scrolling, and
normal/regex/wildcard/hex search with replacement options.

Atomic saves reject external changes, preserve user attributes/access ACLs and
ownership where permitted, and retain a regular-file symlink when writing its
target. Linked files offer detach/preserve-links/cancel; preserving links is an
explicit in-place, non-atomic write. Read-only files are not silently replaced.
New pathname editing, confirmed Save As replacement and unchanged/LF/CRLF/CR
line endings are supported. Delta-based undo retains useful history for large
documents. Files larger than 16 MiB remain rejected rather than using MC's
large-file confirmation workflow.

Escape, F10 and File > Quit check inactive dirty buffers as well as the current
one. Cancel retains every buffer, including after an earlier No decision.
File > Close closes only the current buffer.

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
The commander command line separately handles literal `cd`, quoted paths,
`cd -` and `exit` within mcpp.

Configuration persists panel paths and common preferences, editor options,
history, and hotlist. Full per-pane preferences, skins, localization,
extension handlers, and MC's complete options dialogs remain absent.

Only the local Linux provider is enabled. Archive/extfs, FTP, SSH/SFTP/FISH,
Azure, undelete, and VFS cache controls are not part of this completed local
review.

## Coverage limits

The [100 additional parity and UI cases](mc-parity-ui-test-plan.md) now have
per-case investigation results. They are not 100 passes; the six earlier E/U
findings and existing regressions remain outside that count.

### Original audit-fix verification (`13c9c34`)

| Build | Functional checks |
|---|---|
| Debug | 19/19 CTest entries passed, including guarded core audit regressions and the 19-case terminal audit suite |
| Release | Core audit regressions and all 19 terminal audit cases passed |

Both normal builds use strict warnings. All destructive/error-injection checks
used non-root disposable containers without host mounts. No sanitizers or
optional profiling were run for this fix batch.

The committed model and terminal regressions reproduce concrete failures,
not every MC action or filesystem condition. They do not establish behavior
for millions of entries, every Unicode sequence, resize/signal storms,
crash durability on every filesystem, arbitrary special devices, or all
metadata combinations. Some local provider and viewer work is synchronous;
remote-latency behavior has not been evaluated.

Passing these checks or matching selected screenshots is evidence for the
listed workflows only. Deferred providers and advanced MC features should
not be presented as implemented.
