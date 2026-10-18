# 100 additional MC parity and UI cases

Status: **all 100 case IDs investigated; not 100 passes**. Prepared on
2026-10-18 against repository `8a40e78`; investigation used plan commit
`540e28b`, unchanged product code `e28a00d`, and installed GNU MC 4.8.30.
Per-case outcomes are recorded below.
The subsequent F01-F07 fix delivery is recorded separately below; the original
investigation outcomes have not been relabelled as passes.

These are 100 additional workflow cases, counted once even when both programs
run them. Use paired MC controls where an equivalent exists; explicitly
approved mcpp-only interactions remain UI coverage, not a claim of MC parity.
Existing model coverage can support a case, but does not replace its required
UI/MC comparison. The existing 25-case audit UI suite, other
committed terminal regressions and already walked happy paths were reviewed
before selecting these cases.

E01-E05 and U03 from [the current audit](mc-gap-analysis.md) are **not counted**
among these 100. Their fixes need their own regression checks. Do not count
re-running those six findings, changing terminal size alone, or capturing
another screenshot as an additional case.

## Scope and execution rules

- Linux local files only. No archive/VFS, Azure, SSH/SFTP, spelling, persistent
  shell, unavailable editor actions, full help/keymap implementation or other
  explicitly deferred features.
- Use normal builds, existing Python/tmux helpers and standard Linux tools.
  No sanitizers, optional profiling, stress loops or new project dependencies.
  Keep mcpp in its build directory; do not install it.
- Pin the actual executable hashes, MC version, locale and terminal dimensions
  at execution time. The reference MC checkout is a separate source control,
  not a substitute for running the installed version.
- Give each program an independent fixture copy and fresh HOME/XDG directories.
  Use equal-length roots such as `UI-001/tool-a` and `UI-001/tool-b` so path
  length does not create a false layout difference.
- Calibrate the actual MC focus, search mode and button choices before comparing
  actions. Use equivalent keyboard actions when an approved mcpp mouse
  improvement has no MC equivalent.
- For Tree cases, first establish equivalent visible fixture branches through
  MC's own UI. A cold cache showing only `/` is not a valid comparison control.
  Fixture/cache preparation is setup, not another counted case.
- Compare exact file/cursor/selection outcomes and meaningful visible controls,
  not colors or pixel-identical chrome. Preserve mcpp's approved grayscale,
  safer publication/conflict checks, Tree improvements and hex click placement.
  Do not reproduce an unsafe MC result just to obtain parity.
- Wait for observable state before the next action. Do not replace this with
  arbitrary sleeps, accept a transient old status as completion, or derive
  mouse coordinates before a preceding key batch has finished.
- No tests run as root. Every destructive, external-mutation or error-injection
  case uses an owned disposable container without host mounts.

### Lanes, fixtures and dimensions

| Symbol | Meaning |
|---|---|
| R | Read-only navigation or draft-only interaction with owned fixtures. Fresh host profiles are allowed; no file-operation acceptance or injected filesystem failure. |
| C | Disposable non-root container, no host mounts. Required for publication, destructive operations, injected errors and external filesystem changes. |
| N / S / X / W | 100x30 / 64x16 / 40x12 / 140x40. Default is N unless a row says otherwise. |
| T, L, R, O | Fresh fixture root, left directory, right directory and a sibling directory inside T but outside L. These are paths, not real user directories. |
| Listing | 80 numbered files, known contents, deterministic size/time ties and a few real/symlink directories. Add only the names specified by a case. |
| Tree | Three levels, two same-named leaves under different parents. For in-flight checks reuse the bounded wide-tree fixture already used by `test_tree_workflows.py`; do not enlarge it indefinitely to force a timing window. |
| Text | Short lines, empty lines, a 120-line document, tabs at width 8 and a 2048-column line. Specific Unicode samples are identified by code point. No grapheme-cluster editing claim. |
| Bytes | A 1027-byte binary fixture with known byte values. UI-080 additionally uses a bounded 512 KiB file; this is a byte-preservation case, not a throughput measurement. |

P1 prioritizes wrong-target writes, unwanted publication and draft/data loss.
P2 covers navigation, matching, focus and other normal usability failures.

A current test image without MC cannot provide a runtime MC control for C
cases. Prepare an approved test-only MC-capable image first, or mark the
comparison blocked. Never move a destructive MC control to the host, silently
install dependencies, or count source inspection as a runtime pass.

## Cases

### 1. Navigation and directory recovery: UI-001 through UI-010

| ID | Lane / priority | Fixture and actions | Required observation |
|---|---|---|---|
| UI-001 | R / P2 | Quick cd into a directory whose basename ends with two literal spaces; go to its parent. | Cwd retains the exact filename bytes; parent navigation selects the intended child, without trimming its name. |
| UI-002 | R / P2 | Enter and leave a directory with a newline in its basename using the listing, not a shell command. | Correct cwd; escaped display does not create extra rows, fake prompts or a wrong selection. |
| UI-003 | R / P2 | Navigate through `a..b/./child/../child` with Quick cd, then use Parent. | Component normalization preserves `a..b`; actual cwd and selected child agree with the calibrated MC control. |
| UI-004 | R / P2 | Enter `L/link -> O/target`, then Parent and history-back. | Record logical/physical cwd behavior and selected path against MC; no fabricated nested link path or wrong target. |
| UI-005 | C / P2 | Quick cd through a dangling directory symlink while two files are marked. | Explicit failure; prior cwd, cursor and marks remain intact and usable after acknowledgement. |
| UI-006 | C / P2 | Rename the current directory externally; request listing refresh, then recover through Parent. | No crash or silent switch to an unrelated directory; error and recovery paths preserve a truthful displayed cwd. |
| UI-007 | C / P2 | Remove the current empty directory externally; refresh, dismiss the error and navigate to its surviving parent. | Manager stays responsive and recovers to a real directory without stale selectable entries. |
| UI-008 | R / P2 | Visit A, B, C; go back to B; visit D; exercise back/forward. | Capture the complete history sequence and compare branch behavior with MC; headers and actual cwd never disagree. |
| UI-009 | R / P2 | Visit A then B; Quick cd to `.` three times; use history-back once. | Same-directory navigation does not trap history in duplicate B entries; resulting cwd agrees with MC. |
| UI-010 | C / P2 | Enter a selected child, rename it externally, then return to the parent listing. | Listing shows the new name and a visible, valid selection/fallback, not an actionable stale child path. |

### 2. Listing, sorting and filtering: UI-011 through UI-020

| ID | Lane / priority | Fixture and actions | Required observation |
|---|---|---|---|
| UI-011 | R / P2 | Sort `A.txt`, `a.txt`, and upper/lower non-ASCII names ascending, descending and ascending again. | Exact ordered paths match MC; the cursor remains on the same file rather than a case-folded alias. |
| UI-012 | R / P2 | Sort by extension with `a.tar.gz`, `b.gz`, a dotfile and an extensionless file. | Extension grouping/ties agree with MC; parent entry and directory handling remain consistent. |
| UI-013 | R / P2 | Size-sort zero/one/two-byte files and equal-size twins; reverse twice with one twin selected. | Order/ties are reproducible and selection still identifies the same physical path. |
| UI-014 | R / P2 | Sort files with equal displayed timestamps but distinct supported subsecond modification times. | Compare actual ordering with MC and record filesystem time resolution; no false failure if the fixture cannot retain that resolution. |
| UI-015 | C / P2 | Select and mark a file; append to it externally and change its timestamp; refresh. | Size/time update, while current path, marks and selected-byte total remain correct. |
| UI-016 | R / P2 | Apply `*.log`, toggle hidden files twice, and remove the filter. | Visible entries are the expected filter/hidden intersection; counts and selection do not refer to invisible stale rows. |
| UI-017 | R / P2 | Apply a filter matching no files in a directory containing real and symlink subdirectories. | Expected directories and parent remain navigable; no invalid cursor or phantom regular-file action. |
| UI-018 | C / P2 | Delete the selected middle file externally; refresh and invoke View on the resulting selection. | Valid visible fallback; no stale path or previously cached file is opened under the new selection. |
| UI-019 | R / P2 | Panelize two different `same.txt` files; sort the panelized list, select each and View it. | Sorting preserves absolute source identity; each activation opens its distinct contents. |
| UI-020 | R / P2 | List short mixed-width names including U+754C; resize N to S and back with the wide name selected. | Glyphs, highlight, separators and size/time columns stay aligned; the selected path does not change. |

### 3. Marks, masks and comparison: UI-021 through UI-030

| ID | Lane / priority | Fixture and actions | Required observation |
|---|---|---|---|
| UI-021 | C / P1 | Mark different sets in L/R, sort both, swap panes twice, and open/cancel Copy. | The advertised source set, totals and destination belong to the visible active pane and its original marks. |
| UI-022 | R / P2 | Select with `?.txt` among one-character ASCII, U+00E9, U+754C and two-character basenames. | Compare Unicode wildcard character counting with MC; no partial-byte or extra-name match. |
| UI-023 | C / P1 | Extend selection across the first file and `..`; open/cancel a marked-item operation. | Parent directory never becomes an operation source or inflates the mark count. |
| UI-024 | C / P1 | Mark three files, hide one through a filter, then open/cancel Copy before clearing the filter. | Mark count and actual preview source set agree; hidden paths are not silently substituted or duplicated. |
| UI-025 | R / P2 | Mark a real directory, preview a file in Quick view, return to listing and invert marks. | Inversion acts on the intended listing and preserves the real-directory rule without changing the passive pane. |
| UI-026 | R / P2 | Mark a file with Insert, add the same file through a mask twice, then unselect it once. | No duplicate marks or double-counted bytes; unselection removes exactly one logical entry. |
| UI-027 | C / P1 | Start Compare with different pre-existing mark sets in both panes; cancel its method chooser. | Neither mark set nor cursor changes; a cancelled comparison cannot prepare a different copy source set. |
| UI-028 | C / P1 | Compare same-named file/directory type mismatches under each supported method. | Record MC direction/type handling; never present a real directory as an ordinary file synchronization source. |
| UI-029 | R / P2 | Compare `a.txt` on one side with `A.txt` on the other under a case-insensitive quick-search profile. | Comparison still uses exact Linux pathname identity; quick-search folding does not leak into comparison. |
| UI-030 | C / P2 | Thorough-compare regular-file symlinks with equal targets; change one target and compare again. | Marks track the documented followed-file comparison, not link spelling alone or stale cached contents. |

### 4. Tree, Quick view and Info: UI-031 through UI-040

| ID | Lane / priority | Fixture and actions | Required observation |
|---|---|---|---|
| UI-031 | R / P2 | While A is visibly expanding, select sibling B and request its expansion; let work finish. | Compare with sequential MC actions; the second request is handled or explicitly rejected, never falsely shown as completed. |
| UI-032 | C / P2 | Rename the selected Tree leaf externally; rescan its surviving parent with another branch expanded. | Selection falls back truthfully; renamed node appears and the unrelated expanded branch is retained. |
| UI-033 | R / P2 | Populate two branches containing `cache`; use repeated Tree prefix search. | Search cycles distinct nodes and the full selected path identifies the correct parent branch. |
| UI-034 | R / P2 | Swap panes during one visible Tree rescan, then select/open a retained node after completion. | Late results attach to the correct pane; node activation uses its displayed path, not an old row index. |
| UI-035 | R / P2 | Preview a regular file, then select a directory and a dangling file symlink in turn. | Quick view does not keep advertising the prior file's contents as the new selection. |
| UI-036 | R / P2 | Set Quick view to hex, navigate inside file A, then select file B of a different size. | B's identity and bytes replace A; mode handling is calibrated against MC and no old offset exceeds B. |
| UI-037 | R / P2 | With Quick view visible, edit a draft from the listing; cancel one dirty-close decision, then discard it. | Cancel retains the draft; final return preserves the original listing selection and Quick view source. |
| UI-038 | R / P2 | In Info mode alternate a regular file, symlink, directory and parent selection. | Metadata and displayed source identity update together; information from a former selection is not reused. |
| UI-039 | R / P2 | Leave Tree visible, start Find from the listing, then cancel results and refocus Tree. | Original Tree selection/expansion and listing cwd survive the auxiliary mode round-trip. |
| UI-040 | C / P1 | Cycle a passive pane through Tree, Info, Quick view and listing; open/cancel Copy from the active pane. | Retained passive listing/destination and active source marks remain correct after all mode changes. |

### 5. Menus, inputs and modal routing: UI-041 through UI-050

| ID | Lane / priority | Fixture and actions | Required observation |
|---|---|---|---|
| UI-041 | R / P2 | Put a command draft and caret in the middle; open/change/cancel main menus. | Exact draft, caret and selection return without command execution or accidental deletion. |
| UI-042 | R / P2 | Open a file whose literal name contains a backslash and double quote through View file. | Prompt text is interpreted as a pathname, not shell quoting; the exact intended file opens. |
| UI-043 | R / P2 | Complete a pathname prefix containing literal brackets and spaces, then activate it. | Completion treats filename characters literally and opens the expected candidate, not a glob-expanded sibling. |
| UI-044 | R / P2 | Start ambiguous completion, cycle once, edit a prefix character and complete again. | Editing invalidates the old cycle; candidates and untouched suffix correspond to the new value. |
| UI-045 | R / P2 | Bracketed-paste a valid multiword UTF-8 path into a file prompt and accept. | Entire path arrives once; escape delimiters are not inserted and no partial character is lost. |
| UI-046 | R / P2 | In a path field containing a wide character, Ctrl-K at its boundary and Ctrl-Y, then accept. | Kill/yank restores the exact UTF-8 bytes and intended path, with a correct visible caret. |
| UI-047 | C / P1 | Open Copy at N, resize to X, click an old button coordinate, then the actual Cancel button. | Only the currently visible hit target acts; stale geometry cannot publish a copy. |
| UI-048 | R / P2 | With a command draft present, use the displayed top-menu mnemonic for File, then choose an enabled prompt-opening action. | Correct menu/modal opens; mnemonic input is not inserted into or executed as the command draft. |
| UI-049 | R / P2 | Open editor Help while a dirty draft has a mark and horizontal scroll; close Help. | Same buffer, draft, mark and viewport return; Help dismissal cannot close the editor or lose the draft. |
| UI-050 | C / P2 | Build separate View file and Mkdir prompt histories; recall each after cancelling an edited entry in the other. | Histories remain action-specific and cancelled drafts do not become an unrelated operation's accepted pathname. |

### 6. Find, panelization and hotlist: UI-051 through UI-060

| ID | Lane / priority | Fixture and actions | Required observation |
|---|---|---|---|
| UI-051 | R / P2 | Find names with a Greek Unicode character class and include nonmatching Latin and multicharacter names. | Exact matching path set agrees with calibrated MC glob behavior; class matching does not split UTF-8. |
| UI-052 | R / P2 | Find a literal `a[b].txt` using an escaped filename pattern alongside `ab.txt`. | Only the literal bracketed filename matches; escaping survives the prompt and search worker. |
| UI-053 | C / P1 | Find two same-basename files, View A, return, Edit B, save a marker and return again. | Only B changes; result selection/marks and manager cwd still identify B's full source path. |
| UI-054 | C / P2 | Remove a selected Find hit externally, then activate View and acknowledge failure. | Explicit error returns to usable results; no unrelated file opens and the manager does not exit. |
| UI-055 | C / P2 | Rename a result's parent while its viewer is open, then close the viewer and navigate results. | Return context survives; subsequent stale-path activation reports truthfully rather than changing to an unrelated cwd. |
| UI-056 | R / P2 | Open populated Find results from a manager session, press F10, then continue pane navigation. | F10 dismisses that modal/results context as calibrated with MC; it does not unexpectedly terminate the manager. |
| UI-057 | R / P2 | Add hotlist paths ending in the same leaf name under two different parents; activate each at S. | Selected full path and resulting cwd remain unambiguous even when displayed labels are fitted. |
| UI-058 | C / P2 | Delete a bookmarked directory externally, attempt activation, dismiss the error and select another bookmark. | Prior cwd is retained on failure and the hotlist remains usable. |
| UI-059 | R / P2 | Panelize a mask with zero results; use navigation/cancel controls to recover a normal listing. | No phantom current entry, wrong-file View or trapped empty pane; recovery behavior is compared with MC. |
| UI-060 | C / P2 | Refresh a panelized list after one source is removed and another changes size, with a third marked. | Surviving absolute paths, metadata and marks remain consistent; no cwd-relative replacement is substituted. |

### 7. Text viewer: UI-061 through UI-070

| ID | Lane / priority | Fixture and actions | Required observation |
|---|---|---|---|
| UI-061 | R / P2 | View tabs ending at a wrap boundary; wrap/unwrap, page forward and back at X. | Tabs do not duplicate or skip adjacent bytes; visible rows agree with calibrated MC tab/wrap behavior. |
| UI-062 | R / P2 | Put a two-column U+754C glyph at the last available text column and page through it. | No half-glyph, duplicated character or off-by-one following row; byte position remains valid. |
| UI-063 | R / P2 | Stop on a continued row halfway through a long wrapped line; toggle wrap twice. | Logical text position is preserved or follows the documented MC rule; no jump to an unrelated file region. |
| UI-064 | R / P2 | Scroll horizontally in unwrapped text; resize N to X to N without moving vertically. | Same logical text row and usable horizontal position return, with no dropped middle characters. |
| UI-065 | R / P2 | Search for an ASCII needle crossing a 256 KiB read boundary in a bounded text fixture. | One correct match with original byte coordinates; neither boundary truncation nor duplicate match. |
| UI-066 | R / P2 | Split a valid UTF-8 search character across that read boundary and search case-insensitively. | Match covers the complete original character, without invalid-UTF-8 failure or shifted byte range. |
| UI-067 | R / P2 | Search `aa` in `aaaaa` and repeat without manual movement. | Record exact overlap/continuation offsets against MC; no invented match or premature end before the calibrated continuation rule. |
| UI-068 | R / P2 | Open a search prompt after unwrapped horizontal/vertical navigation, edit it and cancel. | Original viewport and query state remain; cancellation alone does not move to an unseen match. |
| UI-069 | R / P2 | Page through forty empty LF lines between two named markers, including a final line without LF. | Empty rows are counted and traversed; paging neither stops at blanks nor skips a marker. |
| UI-070 | R / P2 | Inspect the full filename from a deeply scrolled viewer, edit the inspection field and cancel it. | Neither pathname nor viewport changes; an inspection prompt cannot rename or reopen a different file. |

### 8. Hex viewer and byte editing: UI-071 through UI-080

| ID | Lane / priority | Fixture and actions | Required observation |
|---|---|---|---|
| UI-071 | C / P1 | Edit only a high nibble, open dirty-close confirmation, cancel, finish the low nibble and Save. | Pending nibble/cursor and staged data survive cancellation; exact intended byte is published once. |
| UI-072 | C / P1 | Edit a low nibble, switch to ASCII with Tab, type one byte, switch back and Save. | Column/nibble transitions affect only the intended bytes and do not reuse a stale half-byte state. |
| UI-073 | C / P1 | Leave a half-edited byte at the right edge, resize N to X to N, finish it and Save. | Same byte/nibble remains active and no clipping-induced wrong-target edit reaches disk. |
| UI-074 | C / P1 | Stage several hex edits, switch to text and back, then choose Save. | Both displays reflect the staged bytes; no implicit publication, lost edit or different file length. |
| UI-075 | R / P2 | From byte zero, hex-search for `00 FF 41` spanning a 16-byte row boundary. | Correct raw-byte match, including NUL/FF; matching is not UTF-8 text decoding. |
| UI-076 | C / P2 | Stage a new byte pattern absent from disk, explicitly return to byte zero and search it before Save. | Search sees staged data without publishing it; the disk remains original until the explicit Save. |
| UI-077 | C / P1 | Overwrite the last real byte through ASCII navigation in a partial final row and Save. | Only that byte changes; the cursor cannot append bytes or create padding past EOF. |
| UI-078 | C / P1 | Stage an edit in a file opened read-only, attempt Save, acknowledge the error and cancel dirty close. | Explicit error, unchanged file and retained dirty draft/cursor; no false saved indicator. |
| UI-079 | C / P1 | Stage an edit, externally change another byte without changing file length, then Save. | Revision conflict is explicit and external bytes are not silently overwritten. Record MC's outcome separately if it is less safe. |
| UI-080 | C / P1 | Edit two distant bytes in a 512 KiB binary file across read/write windows and Save. | Exact two-byte diff and original file length; all intervening NUL/high bytes remain unchanged. |

### 9. Editor navigation, geometry and focus: UI-081 through UI-090

| ID | Lane / priority | Fixture and actions | Required observation |
|---|---|---|---|
| UI-081 | R / P2 | Move vertically through tabbed/wide-character lines separated by a short line. | Preferred display column returns correctly, with valid byte boundaries and calibrated MC cursor positions. |
| UI-082 | R / P2 | Ctrl-Left/Right through `foo_bar-baz` and a non-ASCII word near an LF boundary. | Word stops agree with MC and never land inside UTF-8 or skip a line boundary unexpectedly. |
| UI-083 | R / P2 | Keep a multiline linear mark active while resizing N to X to N. | Anchor/cursor and selected bytes stay unchanged; highlight agrees with the actual marked range. |
| UI-084 | R / P2 | Keep a rectangular mark active, scroll horizontally and extend it from a newly visible cell. | Same intended display-column boundaries; offscreen content is not substituted for the visible extension target. |
| UI-085 | C / P1 | Click inside an expanded tab, insert a marker and Save. | Saved byte placement/tab treatment agrees with the calibrated MC control; no wrong-neighbor insertion. |
| UI-086 | C / P1 | Toggle the line-number gutter, click immediately after a wide glyph, insert a marker and Save. | Gutter/display width is excluded from the byte target; exact expected UTF-8 bytes are saved. |
| UI-087 | R / P2 | Drag a linear mark below the visible last row and back into the editor. | Compare MC autoscroll/clamping; actual selection always matches the visible endpoint and never jumps to an unrelated line. |
| UI-088 | R / P2 | At a large horizontal offset, toggle line numbers twice at S. | Cursor byte, preferred column and a visible caret are preserved as the text width changes. |
| UI-089 | R / P2 | Open a scrolled editor menu, resize to X, switch categories and dismiss through the new visible controls. | Menu hit targets/focus follow the new layout; clicks cannot edit the document underneath. |
| UI-090 | R / P2 | Backward-search a UTF-8 literal from EOF through tabbed lines with horizontal scrolling. | Exact original match boundaries and a visible cursor agree with calibrated MC behavior; no partial-byte selection. |

### 10. Editor blocks, undo and save recovery: UI-091 through UI-100

| ID | Lane / priority | Fixture and actions | Required observation |
|---|---|---|---|
| UI-091 | C / P1 | Move a marked linear block to a point inside its own range, then Undo/Redo and Save. | Overlap handling matches the calibrated MC command; no duplicated, dropped or unrelated bytes. |
| UI-092 | C / P1 | Copy a column block and paste it into short destination lines containing tabs. | Exact byte/display-column result agrees with MC; real tabs/spaces outside the destination are preserved. |
| UI-093 | C / P1 | Delete a rectangular block spanning long, empty and short lines, then Undo and Save. | Empty/short rows are not deleted as whole lines; Undo restores all original bytes and the mark. |
| UI-094 | C / P1 | Overwrite a multibyte character adjacent to LF, continue at EOL, then Undo/Redo and Save. | Whole code points and line separators are handled consistently; no partial UTF-8 or unintended newline loss. |
| UI-095 | C / P1 | Regex Replace All with captures and newline insertion, then one Undo and Redo. | Entire replacement operation has coherent undo grouping and exact final bytes; original mark bounds are not overrun. |
| UI-096 | C / P2 | Save a checkpoint, edit again, Undo exactly to the saved bytes and request Close. | Compare dirty flag/close prompt with MC and verify exact saved-byte equality; do not infer cleanliness from a screenshot alone. |
| UI-097 | C / P1 | Rename the edited file externally before Save, acknowledge failure and recover through Save As. | Draft survives, no unrelated replacement is overwritten, and recovery publishes only to the chosen new path. |
| UI-098 | C / P1 | Open a nonexistent file for editing; create that destination externally before its first Save. | Existing external contents are not silently clobbered; conflict/cancel retains the new draft. |
| UI-099 | C / P1 | Save As through a symlink resolving to the currently edited regular file, then reopen both names. | Correct alias/target revision checks, preserved symlink and intended bytes; no unintended second file or stale forced overwrite. |
| UI-100 | C / P1 | Quit several dirty buffers, save an earlier one, hit a read-only failure in another, cancel, recover with Save As and retry Quit. | All unsaved drafts survive cancellation; already-saved buffers stay saved and only explicit later decisions close the session. |

## Evidence and result classification

For every case retain:

1. Exact fixture recipe, executable identities, terminal geometry, settings and
   input sequence, including MC mode/focus calibration.
2. Before/action/after ANSI and text captures plus rendered screenshots of the
   decisive states. Open and inspect the images rather than merely counting
   generated PNGs.
3. Machine-readable selected paths, cwd, cursor/mark/viewport where relevant.
   For C cases add before/after bytes or hashes and relevant inode/link/mode/
   revision metadata for touched paths, not the entire fixture tree.
4. A runtime comparison and targeted code trace for any discrepancy. Label
   `bug`, `shared limitation`, `approved difference`, `missing capability`,
   `blocked control` or `inconclusive`; only the applicable category counts as
   a parity failure. Mark an approved interaction with no MC equivalent as
   UI-only coverage, with the reason, rather than inventing a runtime control.

An implemented UI result needs both reachable controls and the correct
underlying outcome. A matching screenshot does not prove correct saved bytes;
matching bytes do not prove a visible or clickable control.

## Execution sequence and stopping rules

- First verify the six currently open findings separately when their fixes
  arrive; those reruns stay outside this 100-case count. Independent cases
  need not wait for unrelated fixes.
- Prepare and confirm both runtime lanes. Run P1 cases first, then the remaining
  cases by section in bounded batches of 5-10.
- Use fresh profiles and fixture copies for each case. Reuse a running terminal
  only within that case's stated sequence, never across independent cases.
- On a failure, retain evidence, repeat the minimum reproduction once with a
  fresh profile and inspect the relevant code. Do not expand into stress or
  sanitizer work. Mark a timing-dependent case blocked/inconclusive if its
  observable gate cannot be reached reliably.
- Stop the affected batch on wrong-target publication or lost drafts. Preserve
  the owned container/evidence until exported, then remove only that container.
- Add a focused permanent regression for each confirmed bug when it is fixed.
  Do not mark a case passed while its required MC control, screenshot inspection or
  required filesystem assertion is missing.

The initial status was planned. The following investigation record supersedes
that status without changing the case definitions or implying complete parity.

## Investigation results

All 100 IDs received an investigation and have 200 canonical per-program
records. Setup corrections and confirmation reruns are not extra cases.
`completed-no-new-failure` means the stated checks and inspected captures did
not reveal a new mcpp fault; it is not a claim of pixel-identical MC behavior
or coverage beyond the recorded workflow.

| Outcome | Cases |
|---|---:|
| Completed, no new mcpp failure observed | 72 |
| Confirmed mcpp bugs | 7 |
| Missing mcpp capability blocked the full case | 6 |
| Approved differences / safer mcpp behavior | 6 |
| MC Tree control blocked | 5 |
| Inconclusive MC completion control | 2 |
| Other parity difference requiring a decision | 1 |
| Shared MC limitation | 1 |
| **Total** | **100** |

The seven mcpp findings F01-F07 and shared limitation U04 are documented
with source anchors in [the gap analysis](mc-gap-analysis.md). The earlier
E01-E05/U03 findings remain separate and open. The investigation itself did not
change product code; the following delivery fixes F01-F07.

### F01-F07 fix follow-up

All seven confirmed mcpp bugs from this investigation are now fixed:

| Original case | Finding | Follow-up outcome |
|---|---|---|
| UI-009 | F01 | Relative Quick cd retains the active cwd, including a logical symlink path. |
| UI-002 | F02 | Newline cwd is escaped in both prompt and status; command caret clicks remain correct. |
| UI-048 | F03 | Lowercase/uppercase main-menu mnemonics open the requested category/modal without changing the draft. |
| UI-067 | F04 | The actual text occurrence is highlighted; continuation moves the highlight. |
| UI-082 | F05 | All seven recorded forward stops and seven reverse stops match MC. |
| UI-084 | F06 | Horizontal Shift-click preserves the rectangle's anchor/shape and exact column-delete/save bytes. |
| UI-087 | F07 | Selection drag scrolls at both edges, returns to the correct visible row and clamps at EOF. |

The 32-case terminal audit suite, focused core/editor regressions and guarded
audit model checks passed. All seven original reproducers were repeated
successfully against the rebuilt normal Release; paired screenshots were
inspected against the retained MC 4.8.30 controls. Detailed delivery and evidence
are in [the gap analysis](mc-gap-analysis.md#seven-finding-fix-delivery).

This follow-up did not re-execute the remaining 93 original cases or resolve
the missing capabilities, blocked/inconclusive controls, shared limitations or
history behavior decision. The original categories below describe `e28a00d`.

### Environment and evidence

- All application runs, including the write/error cases, used a disposable
  Linux/arm64 container with no host mounts, no network and an unprivileged
  user. Existing installed MC binaries/resources and their missing system
  runtime files were copied for the control; no project dependencies or
  packages were installed.
- mcpp SHA-256: `c74838f79ff30ee4e8b9fe8516677b4a77f2744fd68378d6aff9a5420e270211`.
  MC 4.8.30 SHA-256: `35a55f42a97c6685b950539d9ce1bcc9959be1049bc0cbb89d1525025fc4a7c8`.
- Fresh private profiles, C.UTF-8 and independent fixtures were used. Cases
  not depending on a large listing used only their named entries; this was
  not an exhaustive expansion of every shared fixture preset.
- MC manager-to-editor controls explicitly enabled its built-in editor/viewer
  after an initial external-editor setup failure. Native column marking and
  tagging were calibrated separately; literal-key and wrong-mode setup
  artifacts were excluded. MC marks were checked using actual totals and
  mark attributes, not ordinary colored filenames.
- Every canonical case retains action logs, before/action/after ANSI/text
  captures, runtime observations and applicable byte/metadata outcomes.
  Paired decisive screenshots, 20 contact sheets and corrected control sheets
  were opened and inspected. Source inspection confirmed the reported faults.
- Canonical evidence is in this session's `parity-ui-100/evidence-canonical/`;
  `investigation-results.json`, `capture-manifest.json`, `case-NNN.png` and
  the drivers are alongside it. Older setup captures are not the result set.
- The disabled Sort order entry blocked UI-011 through UI-014, UI-019 and
  UI-021. No model/config substitute was counted as a UI sort pass.
- MC Tree ancestry could not be established equivalently for UI-031 through
  UI-034 and UI-039; these remain blocked, not passing.
- Hotlist test entries were seeded as fixtures in mcpp because Add current is
  not exposed in its UI. UI-057/UI-058 test existing-entry navigation/error
  behavior, not an unimplemented Add operation.
- MC completion interaction was not resolved sufficiently for UI-043/UI-044.
  UI-008 has a real history-branch difference but no imposed behavior decision.
  No sanitizers, optional profiling or stress expansion was used.

### Per-case record

| Case | Outcome | Observation |
|---|---|---|
| UI-001 | completed-no-new-failure | Trailing-space directory entered; parent navigation retained exact filename identity. |
| UI-002 | confirmed-bug | F02: raw newline in cwd splits the command prompt; pane title itself is escaped. |
| UI-003 | completed-no-new-failure | Dot components normalize without treating a..b as a parent component. |
| UI-004 | completed-no-new-failure | Symlink entry, parent and history remained valid; logical spelling was retained. |
| UI-005 | completed-no-new-failure | Failed Quick cd kept the marked files and original directory. |
| UI-006 | completed-no-new-failure | Renamed cwd reports/recoveries differ, but both returned to a real, truthfully displayed directory. |
| UI-007 | completed-no-new-failure | Removed cwd did not crash either manager; parent recovery remained usable. |
| UI-008 | parity-difference | History branching differs: the same back/forward sequence ends at D in mcpp and B in MC. |
| UI-009 | confirmed-bug | F01: relative Quick cd '.' resolves at provider root rather than the active pane. |
| UI-010 | completed-no-new-failure | Renamed child appears; mcpp chooses a valid parent fallback and MC selects the renamed child. |
| UI-011 | missing-capability | Disabled Sort order blocks name-sort UI comparison. |
| UI-012 | missing-capability | Disabled Sort order blocks extension-sort UI comparison. |
| UI-013 | missing-capability | Disabled Sort order blocks size-sort/tie UI comparison. |
| UI-014 | missing-capability | Disabled Sort order blocks subsecond modification-sort UI comparison. |
| UI-015 | completed-no-new-failure | Refresh updates size/time while the selected marked file remains visible. |
| UI-016 | completed-no-new-failure | Filter/hidden intersections and clearing were captured; no stale target observed. |
| UI-017 | completed-no-new-failure | No matching files leaves real/symlink directories and parent navigation. |
| UI-018 | completed-no-new-failure | Deleted selected file is not reopened from cache; valid fallback policies differ. |
| UI-019 | missing-capability | Panelized identities were prepared, but disabled sorting blocks the planned sort/activation sequence. |
| UI-020 | completed-no-new-failure | Mixed-width names remain aligned and selected through resize. |
| UI-021 | missing-capability | Marked sets prepared; unavailable sort UI blocks the full planned ownership sequence. |
| UI-022 | shared-limitation | U04: both programs mark only ASCII a.txt for ?.txt; actual mark totals confirm one file. |
| UI-023 | completed-no-new-failure | Parent entry is excluded from the marked Copy preview. |
| UI-024 | completed-no-new-failure | Filtered Copy previews name the retained visible source set consistently. |
| UI-025 | completed-no-new-failure | Marked real directory survives inversion after Quick view. |
| UI-026 | completed-no-new-failure | Overlapping mask additions/removal do not leave duplicate marks. |
| UI-027 | completed-no-new-failure | Cancelling Compare preserves pre-existing marks in both panes. |
| UI-028 | completed-no-new-failure | Both mark the file sides and skip directory sources; MC mark attributes and totals confirm this. |
| UI-029 | completed-no-new-failure | Case-different Linux names remain distinct comparison entries. |
| UI-030 | completed-no-new-failure | Changed symlink referents are detected; subsecond age handling differs from MC's second resolution. |
| UI-031 | blocked-control | MC Tree fixture ancestry could not be populated equivalently; no paired pass claimed. |
| UI-032 | blocked-control | mcpp rescan captured; equivalent MC Tree ancestry remains blocked. |
| UI-033 | blocked-control | mcpp duplicate-leaf search captured; equivalent MC Tree ancestry remains blocked. |
| UI-034 | blocked-control | mcpp in-flight pane swap captured; paired MC Tree setup remains blocked. |
| UI-035 | completed-no-new-failure | Directory/dangling-link selection no longer shows the previous regular-file preview. |
| UI-036 | completed-no-new-failure | Switching source files replaces the hex preview identity and resets position. |
| UI-037 | completed-no-new-failure | Dirty edit Cancel retains draft; discard returns to Quick view without publication. |
| UI-038 | completed-no-new-failure | Info source metadata changes with file/link/directory/parent selection. |
| UI-039 | blocked-control | Tree/Find return captured, but equal populated MC ancestry was not established. |
| UI-040 | completed-no-new-failure | Auxiliary-mode cycle returns a Copy preview with the correct active source and destination. |
| UI-041 | completed-no-new-failure | Menu cancellation preserves the command draft. |
| UI-042 | completed-no-new-failure | Literal quote/backslash pathname opens the intended file. |
| UI-043 | inconclusive-control | mcpp literal completion works; MC completion acceptance was not calibrated sufficiently for a pass. |
| UI-044 | inconclusive-control | mcpp invalidates the edited completion cycle; MC completion-popup editing remains inconclusive. |
| UI-045 | completed-no-new-failure | Bracketed paste opens the complete UTF-8 path without delimiters. |
| UI-046 | completed-no-new-failure | UTF-8 field kill/yank restores the intended pathname. |
| UI-047 | completed-no-new-failure | Old coordinates after resize do not publish; actual Cancel remains usable. |
| UI-048 | confirmed-bug | F03: displayed File mnemonic does not open its menu; MC opens the requested modal. |
| UI-049 | completed-no-new-failure | Help round-trip retains dirty draft, mark and viewport; disk stays unchanged. |
| UI-050 | completed-no-new-failure | Cancelled edited Mkdir recall does not contaminate View file history. |
| UI-051 | approved-difference | mcpp matches the intended Unicode class names; MC overmatches gamma. |
| UI-052 | completed-no-new-failure | Escaped filename pattern returns the literal bracketed name. |
| UI-053 | completed-no-new-failure | Only the second same-basename result is saved; the first stays unchanged in both controls. |
| UI-054 | completed-no-new-failure | Stale result View reports an error while retaining results; mcpp error is nonmodal. |
| UI-055 | completed-no-new-failure | Parent rename returns to retained results; subsequent stale activation reports failure. |
| UI-056 | completed-no-new-failure | Results F10 returns to a usable manager instead of exiting. |
| UI-057 | completed-no-new-failure | Seeded same-leaf hotlist entries activate distinct full paths at narrow width. |
| UI-058 | completed-no-new-failure | Missing bookmark reports failure without changing the original cwd. |
| UI-059 | completed-no-new-failure | Empty panelization recovers to a usable normal listing. |
| UI-060 | completed-no-new-failure | Panelized refresh removes the deleted source, updates size and retains the surviving mark. |
| UI-061 | completed-no-new-failure | Tab wrap boundaries and paging retain the adjacent marker. |
| UI-062 | completed-no-new-failure | Wide-character boundary survives paging without a partial glyph. |
| UI-063 | completed-no-new-failure | mcpp preserves the continued-row anchor; MC resets to hard-line start. Both meet the stated alternatives. |
| UI-064 | completed-no-new-failure | Unwrapped horizontal position survives narrow/wide resize. |
| UI-065 | completed-no-new-failure | ASCII search finds the original cross-window needle. |
| UI-066 | completed-no-new-failure | Case-insensitive UTF-8 search finds the original split-window character. |
| UI-067 | confirmed-bug | F04: text search provides no visible match span; MC highlights the actual occurrence. |
| UI-068 | completed-no-new-failure | Cancelled search retains the exact text viewport. |
| UI-069 | completed-no-new-failure | Paging traverses blank LF rows and reaches/returns from both markers. |
| UI-070 | approved-difference | mcpp-only full-filename inspection leaves source and viewport unchanged. |
| UI-071 | completed-no-new-failure | Completed nibble survives cancelled dirty-close and saves exactly once. |
| UI-072 | completed-no-new-failure | Hex/ASCII transitions save exactly the intended three bytes. |
| UI-073 | completed-no-new-failure | Half-byte survives resize; MC uses the actual last column of its adaptive row. |
| UI-074 | completed-no-new-failure | Mode round-trip retains staged edits and does not publish until Save. |
| UI-075 | completed-no-new-failure | Raw NUL/FF pattern matches across a common 16-byte row at width 80. |
| UI-076 | completed-no-new-failure | Search sees staged bytes while disk stays unchanged. |
| UI-077 | completed-no-new-failure | Final ASCII overwrite preserves file length. |
| UI-078 | completed-no-new-failure | Read-only Save fails explicitly and retains draft; disk bytes remain original. |
| UI-079 | approved-difference | mcpp rejects stale publication; MC merges changed bytes. Both preserve the external byte. |
| UI-080 | completed-no-new-failure | 512 KiB Save has exactly the two expected changed bytes. |
| UI-081 | completed-no-new-failure | Preferred display column through tabs/wide glyph and a short line agrees after index normalization. |
| UI-082 | confirmed-bug | F05: word-right skips the hyphen stop and word-left skips the previous EOL stop. |
| UI-083 | completed-no-new-failure | Linear mark/cursor survive narrow/wide resize. |
| UI-084 | confirmed-bug | F06: Shift-click changes an existing rectangular mark into a linear mark. |
| UI-085 | completed-no-new-failure | Tab-cell click saves the same surrounding byte sequence as MC. |
| UI-086 | completed-no-new-failure | Gutter/wide-glyph click saves the exact expected UTF-8 sequence. |
| UI-087 | confirmed-bug | F07: drag beyond viewport clamps at the last rendered row instead of scrolling. |
| UI-088 | completed-no-new-failure | Gutter toggles preserve cursor byte and visible caret. |
| UI-089 | completed-no-new-failure | Resized editor-menu interaction leaves document bytes unchanged. |
| UI-090 | completed-no-new-failure | Backward UTF-8 match has the same original byte position as MC. |
| UI-091 | completed-no-new-failure | Overlap move/Undo/Redo preserves original bytes in both programs. |
| UI-092 | completed-no-new-failure | Calibrated column paste produces identical tab/short-line bytes in both programs. |
| UI-093 | completed-no-new-failure | Column Delete/Undo restores exact original bytes, including the empty row. |
| UI-094 | approved-difference | mcpp preserves expected UTF-8; this MC overwrite/Undo/Redo control saves invalid UTF-8. |
| UI-095 | completed-no-new-failure | Capture/newline Replace All has exact grouped Undo/Redo bytes. |
| UI-096 | completed-no-new-failure | Both show a modified-close prompt after Undo reaches saved bytes; shared behavior recorded. |
| UI-097 | approved-difference | mcpp reports the missing old path and recovers via Save As; MC can recreate the old pathname. |
| UI-098 | approved-difference | Appeared target is protected: mcpp refuses overwrite; MC presents a warning choice. |
| UI-099 | completed-no-new-failure | Save As preserves the symlink and changes the intended referent. |
| UI-100 | completed-no-new-failure | Earlier save persists; readonly draft survives failure, Save As recovery and final Quit. |
