#include "mcpp/help.hpp"

#include <algorithm>
#include <stdexcept>

namespace mcpp {
namespace {

HelpBlock paragraph(const std::string_view text) {
    return {text, std::nullopt, false};
}

HelpBlock heading(const std::string_view text) {
    return {text, std::nullopt, true};
}

HelpBlock link(const HelpTopic topic, const std::string_view label) {
    return {label, topic, false};
}

}  // namespace

std::span<const HelpPage> help_pages() {
    using enum HelpTopic;
    // Independently written mcpp documentation; no external help resources.
    static const std::vector<HelpPage> pages{
        {index, "Help index", {
            paragraph("Choose a topic with Tab or Shift-Tab, then Enter. F3 returns to the previous page; F10 closes help."),
            link(browser, "Using this help browser"),
            link(manager, "File manager"),
            link(panels, "Panel navigation and display"),
            link(selection, "Marking and filename masks"),
            link(command_line, "Command line and menus"),
            link(copy_move, "Copying and moving"),
            link(delete_files, "Deleting files and directories"),
            link(jobs, "Progress and background jobs"),
            link(find_files, "Finding files and panelizing"),
            link(tree, "Directory tree"),
            link(hotlist, "Directory hotlist"),
            link(viewer, "File viewer"),
            link(viewer_search, "Viewer search and positions"),
            link(viewer_edit, "Hexadecimal editing"),
            link(quick_view, "Quick view and information panels"),
            link(editor, "Text editor"),
            link(editor_blocks, "Editor blocks and clipboard"),
            link(editor_search, "Editor search and replacement"),
            link(editor_save, "Saving and closing editor files"),
            link(editor_windows, "Editor buffers and windows"),
            link(diff, "Comparing and merging files"),
            link(dialogs, "Dialogs and text inputs"),
            link(settings, "Display and saved preferences"),
            link(keymaps, "Configurable keys"),
            link(archives, "Read-only local archives"),
            link(permissions, "Permissions, ownership and links"),
        }},
        {browser, "Using help", {
            paragraph("Help stays inside mcpp. It does not open a web browser, run commands or fetch anything from the network."),
            heading("Moving around"),
            paragraph("Up and Down scroll one line. PageUp and PageDown scroll a page; Space also pages down and b pages up. Home and End reach the start and end of the current topic. The mouse wheel scrolls."),
            paragraph("Tab selects the next link. Shift-Tab or Alt-Tab selects the previous link. Enter or Right follows the selected link; clicking a link opens it too."),
            paragraph("F2 opens the index. F3 or Left goes back, restoring the page position and selected link. F1 opens this page. F10 or Escape closes help, not the file or dialog underneath."),
            heading("Context and keys"),
            paragraph("Help opens at a topic for the current panel, editor, viewer, comparison or dialog. Your unfinished input, marks and open windows stay in place. Background work can continue while you read."),
            paragraph("Printed keys describe the default bindings. A custom keymap can replace Help and the browser actions in its help section. Key-learning and literal-key prompts still capture keys instead of opening help."),
            link(index, "Help index"),
            link(keymaps, "Configurable keys"),
        }},
        {manager, "File manager", {
            paragraph("The two panels keep separate directories and cursor positions. Tab switches the active panel. The highlighted entry and any marked entries determine which files a command uses."),
            heading("Everyday commands"),
            paragraph("Use arrows, Home, End and paging to move through a listing. Enter opens a directory or a supported archive. Ctrl-PageUp goes to the parent; Ctrl-PageDown enters the selected directory."),
            paragraph("F2 opens the user menu. F3 views the selected file and F4 edits it. F5 copies, F6 renames or moves, F7 creates a directory and F8 deletes the selected targets."),
            paragraph("F9 opens the menus. Underlined letters choose categories or enabled menu items. F10 quits after any required confirmation. Help does not accept a confirmation for you."),
            heading("Targets and destinations"),
            paragraph("Insert marks or unmarks an entry. File commands use marked entries when there are any, otherwise the current entry. Check the source and destination shown in a dialog before accepting it."),
            paragraph("The command line runs local shell commands. Ctrl-O switches to the shell and back. A hidden command line cannot run an invisible draft."),
            paragraph("Local files and supported read-only archives are available. Remote providers, including SSH/SFTP and Azure storage, are paused."),
            link(panels, "Panel navigation and display"),
            link(selection, "Marking and filename masks"),
            link(copy_move, "Copying and moving"),
            link(command_line, "Command line and menus"),
            link(archives, "Read-only local archives"),
        }},
        {panels, "Panel navigation and display", {
            paragraph("Tab switches panels. Ctrl-U swaps them. Ctrl-R explicitly reloads the current listing; Alt-. toggles hidden files."),
            paragraph("Ctrl-S or Alt-S starts incremental filename search. Continue typing to refine it; Escape leaves the search. Directory history and Quick cd are available from the Command menu."),
            paragraph("Panel menus choose listing format, sorting, filtering, a tree, Quick view or Info. Listing format supports Full, Brief, Long and User defined. Sort settings include field, case, reverse order and executable-first handling."),
            paragraph("A panel filter hides nonmatching entries; it is not a recursive disk search. Find file can instead put a result set into a panel."),
            paragraph("Panel options controls such things as hidden/backup files, marking, navigation, display units and saved panel setup. Rescan bypasses fast reload."),
            link(selection, "Marking and filename masks"),
            link(find_files, "Finding files and panelizing"),
            link(tree, "Directory tree"),
            link(quick_view, "Quick view and information panels"),
            link(settings, "Display and saved preferences"),
        }},
        {selection, "Marking and filename masks", {
            paragraph("Insert toggles the current entry's mark. Shift-Up and Shift-Down mark while moving. A file command uses marked entries before the current entry."),
            paragraph("+ asks for a case-sensitive selection mask; - removes matching marks. * inverts files, including directory symlinks, without changing marks on real directories."),
            paragraph("Read the dialog's target count and source before copying, moving, deleting or changing metadata. Cancel leaves the pending command unaccepted."),
            paragraph("A skipped or incompletely processed source keeps its mark. The operation result distinguishes skipped items from completed work."),
            link(copy_move, "Copying and moving"),
            link(delete_files, "Deleting files and directories"),
            link(permissions, "Permissions, ownership and links"),
        }},
        {command_line, "Command line and menus", {
            paragraph("Type a command in the manager command line and press Enter to run it in the active local directory. Ctrl-O shows or hides the shell. With Bash, commands share a persistent interactive shell."),
            paragraph("The input supports cursor/word movement, selection, kill/yank and history. Alt-Tab completes local paths, shell command names, environment variables, users and hosts without running shell completion hooks or making network lookups."),
            paragraph("F9 opens the main menus; the underlined letters activate categories and enabled items. F2 opens a local user menu when one is configured. Editor menus have their own file and block actions."),
            paragraph("Menu and extension commands can run external programs. Missing tools and unsupported command forms report an error rather than silently choosing a different action."),
            paragraph("Commands from an archive use the archive's containing local directory. External file commands need a local copy of an archive member."),
            link(dialogs, "Dialogs and text inputs"),
            link(keymaps, "Configurable keys"),
            link(archives, "Read-only local archives"),
        }},
        {copy_move, "Copying and moving", {
            paragraph("F5 copies and F6 moves or renames. The dialog retains its source targets while you edit the destination and options. Tab changes focus; PageUp/PageDown or the wheel scrolls the options body."),
            paragraph("A relative destination starts in the active panel's directory. An existing directory receives the source names. A single-file destination can be an exact new name; a trailing slash asks for a directory."),
            paragraph("Source masks can filter names and transform destination names. Shell-style masks and ECMAScript regular expressions are supported, not complete PCRE. Dive controls placement into an existing destination directory."),
            paragraph("Preserve options affect metadata. Stable symlinks can adjust local relative links to keep their referent. Sparse files and supported special nodes are copied without treating a FIFO as a byte stream; sockets are rejected."),
            heading("Conflicts and errors"),
            paragraph("Overwrite questions offer per-file or remaining-file choices. Append and Reget appear only for eligible local regular files. Reget checks that the smaller destination matches the source prefix before resuming."),
            paragraph("Retry, skip, ignore-all or abort handles a reported error. Skip is not successful completion and keeps the source marked. Cancellation requests a stop; already completed work is not undone."),
            paragraph("Archives are read-only: you can copy members out, not move them away or write into the archive. Remote transfers are not implemented."),
            link(jobs, "Progress and background jobs"),
            link(selection, "Marking and filename masks"),
            link(archives, "Read-only local archives"),
        }},
        {delete_files, "Deleting files and directories", {
            paragraph("F8 starts deletion of marked entries, or the current entry if nothing is marked. Check the displayed targets before accepting. Escape cancels the pending dialog."),
            paragraph("Deleting a nonempty directory can ask again before descending. Yes and No apply to that directory. All and None apply to the remaining nonempty directories in that operation; Abort stops the operation."),
            paragraph("None does not protect selected ordinary files or empty directories from the same operation. Skipped directories keep their selection marks. Safe delete starts confirmations on No."),
            paragraph("Errors can be retried, skipped, ignored for the remaining errors, or aborted. Cancellation cannot restore files that have already been removed. There is no trash or undelete operation."),
            link(jobs, "Progress and background jobs"),
            link(selection, "Marking and filename masks"),
        }},
        {jobs, "Progress and background jobs", {
            paragraph("Copy, move and delete report progress while working. A cancellation request stops further work when the worker reaches a safe check; it does not roll back completed changes."),
            paragraph("Ctrl-X then j opens Background jobs. The list includes recent results and offers the controls applicable to the selected job, including show, pause/resume and abort."),
            paragraph("A background job may need a conflict or error decision. Read the source, destination and requested choice before answering; opening help does not answer or dismiss it."),
            paragraph("Completion does not accept your editor, viewer or prompt. When returning to panels, failures remain visible until dismissed. Skipped files are incomplete work, not silent success."),
            link(copy_move, "Copying and moving"),
            link(delete_files, "Deleting files and directories"),
            link(dialogs, "Dialogs and text inputs"),
        }},
        {find_files, "Finding files and panelizing", {
            paragraph("The Command menu's Find file action, also Alt-?, searches below the chosen directory. Enter the requested filename/content criteria in the dialog; this is separate from incremental panel search."),
            paragraph("Find results can be viewed with F3 or edited with F4. Returning retains the selected result. Panelize, also Alt-P in the results, makes the result set a panel listing."),
            paragraph("The panel's Panelize action restores the last panelized listing; it does not start a new search. External panelize runs an editable local command and checks its returned pathnames before replacing a listing."),
            paragraph("Ctrl-C requests cancellation of an active search. A result set describes the files found at search time; files can change afterwards."),
            link(panels, "Panel navigation and display"),
            link(dialogs, "Dialogs and text inputs"),
            link(viewer_search, "Searching within a viewed file"),
            link(editor_search, "Editor search and replacement"),
        }},
        {tree, "Directory tree", {
            paragraph("Arrows navigate directories. Right expands a branch or enters it; Left collapses it or selects its parent. Space, + and - toggle, expand and collapse branches."),
            paragraph("In the directory chooser, Enter chooses the highlighted directory and Escape cancels. A tree panel can open a selected directory in the opposite listing. Clicking selects; double-clicking opens there."),
            paragraph("F2 rescans the selected branch. The modal tree also offers F3 to forget cached entries and F4 to switch linear/sibling navigation. Ctrl-S starts tree search."),
            paragraph("Ancestors load their immediate children in the background. Unvisited subtrees are not scanned recursively. Cached branches and selection are retained when possible."),
            link(panels, "Panel navigation and display"),
            link(copy_move, "Copying and moving"),
        }},
        {hotlist, "Directory hotlist", {
            paragraph("Ctrl-\\ opens the directory hotlist. Select an entry to visit it, or a group to browse its contents. Ctrl-X then h starts adding the current location."),
            paragraph("The dialog offers adding entries and groups, moving entries between groups and removing them. A removal uses the configured confirmation. Edit the label and path before accepting an entry form."),
            paragraph("Entries are stored in mcpp's private hotlist file. Archive entries retain their full VFS location. Cancelled forms and failed writes keep the existing data and pending input."),
            link(dialogs, "Dialogs and text inputs"),
            link(archives, "Read-only local archives"),
        }},
        {viewer, "File viewer", {
            paragraph("F3 opens the current file. Arrows, paging, Home and End move through it. The wheel scrolls. F10, F3, Escape or q leaves the viewer after any required save decision."),
            paragraph("F2 toggles text wrapping; in hexadecimal mode it toggles editing instead. F4 switches between text and hexadecimal display. F5 goes to a line, percentage or byte offset. F7 searches."),
            paragraph("F8 changes parsing and F9 changes nroff handling. Alt-E chooses a data codepage, not a terminal encoding. Click the filename header to see or enter a full path."),
            paragraph("End or further downward movement can observe newly appended data; Ctrl-L explicitly refreshes. A redirected input stream stays inside the viewer. Command-filter output is read-only."),
            paragraph("Hexadecimal edits are staged until explicitly saved. Leaving a dirty viewer asks what to do rather than discarding the edits silently."),
            link(viewer_search, "Viewer search and positions"),
            link(viewer_edit, "Hexadecimal editing"),
            link(quick_view, "Quick view"),
            link(dialogs, "Dialogs and text inputs"),
        }},
        {viewer_search, "Viewer search and positions", {
            paragraph("F7 opens the file search dialog. It offers normal text, regular expression, hexadecimal bytes and wildcard modes, with the displayed case, direction, whole-word and charset options."),
            paragraph("/ starts forward search and ? starts backward search. n repeats a viewer search and N repeats in the opposite direction. Shift-F7 also repeats. Search errors keep the dialog available for correction."),
            paragraph("F5 opens Go to. Choose a line number, percentage, decimal offset or hexadecimal offset as offered. The current file and position stay open while you read help."),
            paragraph("The hexadecimal search input describes encoded file bytes, not the displayed character spelling. Text regular expressions use the implementation's supported expression rules, not complete PCRE."),
            link(viewer, "File viewer"),
            link(viewer_edit, "Hexadecimal editing"),
            link(dialogs, "Dialogs and text inputs"),
        }},
        {viewer_edit, "Hexadecimal editing", {
            paragraph("In the full viewer, F4 selects hexadecimal display and F2 toggles hex editing. Tab switches between hex digits and ASCII bytes. Click a displayed byte to position the edit cursor."),
            paragraph("Edits remain staged in memory. F6 explicitly saves them. Leaving with staged changes offers Save, Discard and Cancel; Cancel retains the file, edits and position."),
            paragraph("Save checks whether the file changed and respects read-only providers and linked-file protections. A failed save retains the edits. Quick view and command-filter output are read-only."),
            paragraph("Hex editing changes bytes, not parsed text characters. Check the displayed offset and byte values before saving."),
            link(viewer, "File viewer"),
            link(viewer_search, "Viewer search and positions"),
        }},
        {quick_view, "Quick view and information panels", {
            paragraph("Ctrl-X then q toggles Quick view; Ctrl-X then i toggles Info. Tab can focus a Quick view panel. It displays the file selected in the other listing."),
            paragraph("Focused Quick view supports scrolling, F2 wrapping, F4 text/hex, F5 Go to, F7 search, F8 parsing and Alt-E codepage selection. F3 opens the full viewer."),
            paragraph("Quick view is read-only. File-operation keys do not turn it into an editable listing. Info displays file and filesystem details rather than a second selection list."),
            link(viewer, "File viewer"),
            link(viewer_search, "Viewer search and positions"),
            link(panels, "Panel navigation and display"),
        }},
        {editor, "Text editor", {
            paragraph("F4 opens the current file for editing. mcppedit starts the same editor directly; a new unnamed buffer does not create a file until saved."),
            heading("Editing"),
            paragraph("Arrows and paging move the cursor. Home/End reach line ends; Ctrl-Home/Ctrl-End reach document ends. Insert toggles insert/overwrite. Ctrl-U undoes and Alt-R redoes."),
            paragraph("F3 starts or finishes a block. Shift movement marks text, and Shift-F1 marks columns. F5 copies the block, F6 moves it and F8 deletes the block or current line."),
            paragraph("F7 searches, Shift-F7 repeats and F4 replaces. Alt-L goes to a line. F2 saves; F12 saves under another name. F9 opens editor menus."),
            heading("Display and tools"),
            paragraph("Ctrl-S toggles syntax colors and Alt-N toggles line numbers. Editor options include tab spacing, indentation and whitespace display. Alt-E changes the file data codepage while retaining the draft."),
            paragraph("Ctrl-Q captures a literal key. Ctrl-R starts or stops macro recording; its key prompt captures the chosen hotkey. Help does not intercept those key-capture prompts."),
            paragraph("F10 or Escape requests quitting the editor's buffers. Dirty buffers get a decision rather than being silently lost. In standalone mode, closing the last buffer exits the process."),
            link(editor_blocks, "Blocks and clipboard"),
            link(editor_search, "Search and replacement"),
            link(editor_save, "Saving and closing files"),
            link(editor_windows, "Buffers and windows"),
            link(settings, "Editor preferences"),
        }},
        {editor_blocks, "Editor blocks and clipboard", {
            paragraph("F3 starts or finishes a linear block. Shift-F1 starts or finishes a column block. Shift movement marks text; mouse dragging marks a linear block and Alt-drag marks columns."),
            paragraph("F5 copies a marked block to the cursor and F6 moves it. F8 deletes a marked block, or the current line when no block is marked. These operations change the draft and can be undone."),
            paragraph("Ctrl-Insert stores the block in the editor clipboard; Shift-Delete cuts it and Shift-Insert pastes it. The clipboard persists across editor sessions and preserves column-block shape."),
            paragraph("Help leaves the existing mark, cursor and buffer intact. A block is not a manager file-selection mark."),
            link(editor, "Text editor"),
            link(editor_search, "Search and replacement"),
            link(editor_save, "Saving and closing files"),
        }},
        {editor_search, "Editor search and replacement", {
            paragraph("F7 opens Search and F4 opens Replace. Tab moves through text fields, search mode, options and buttons. Escape cancels without applying replacement text."),
            paragraph("Normal, regular expression, hexadecimal and wildcard search are available. Use the shown case, direction, whole-word and selection options. Shift-F7 repeats the previous search."),
            paragraph("Replacement can ask for each match or apply to all matches in its chosen scope. Wildcards capture matching text; regular-expression replacements support the documented escape forms used by the editor."),
            paragraph("Hexadecimal search matches encoded file bytes and maps results to document positions. An invalid expression or unrepresentable codepage conversion reports an error without replacing the draft."),
            paragraph("The expression engine is not complete PCRE. Check a small replacement before applying it throughout a document."),
            link(editor, "Text editor"),
            link(editor_blocks, "Blocks and clipboard"),
            link(dialogs, "Dialogs and text inputs"),
        }},
        {editor_save, "Saving and closing editor files", {
            paragraph("F2 saves the current buffer. F12 opens Save as. An unnamed buffer requests a filename. Relative editor paths use the edited file's directory, or the buffer's starting directory."),
            paragraph("Save as can choose the offered line-ending mode. Editor Save mode controls the available save/backup behavior. Saving retains the selected data encoding."),
            paragraph("A save checks for external changes and handles linked files explicitly. A lock conflict, write failure or incompatible conversion reports an error and keeps the draft."),
            paragraph("Closing or quitting with changes asks whether to save, discard or cancel. Cancel returns to the pending buffer or decision. Help does not save, discard, accept an overwrite or release a lock."),
            paragraph("Archive members are read-only. Save as can write a local copy; ordinary Save cannot modify the archive."),
            link(editor, "Text editor"),
            link(editor_windows, "Buffers and windows"),
            link(dialogs, "Dialogs and text inputs"),
            link(archives, "Read-only local archives"),
        }},
        {editor_windows, "Editor buffers and windows", {
            paragraph("The editor can keep several buffers open. Use its Window menu to switch, list, move, resize or maximize the available editor windows. Opening an already open file selects its buffer."),
            paragraph("Each buffer retains its cursor, viewport, marks and draft. The editor history can reopen earlier locations. Help is an overlay: it does not open a file buffer or change a window's identity."),
            paragraph("Alt-` opens the available screen list. Screen and buffer controls are distinct: choose the displayed entry before switching or closing anything."),
            paragraph("Closing the final standalone editor buffer exits mcppedit. Quitting with unsaved buffers visits the required save decisions."),
            link(editor_save, "Saving and closing files"),
            link(editor_blocks, "Blocks and clipboard"),
        }},
        {diff, "Comparing and merging files", {
            paragraph("The comparison displays two local text files and their differences. n or Enter selects the next hunk; p selects the previous one. Arrows, paging, Home and End move through the display."),
            paragraph("F5 copies the displayed left hunk to the right. Shift-F5 copies right to left. These are staged merges; F2 saves them. Ctrl-Z undoes a merge and Alt-R redoes it."),
            paragraph("F4 edits the displayed left file; Shift-F4 edits the other side. Save a staged merge before opening that file in the editor. Returning rebuilds the comparison."),
            paragraph("F7 searches, Shift-F7 repeats and F9 opens comparison options. Ctrl-U swaps the displayed sides. Check the side names before merging; left and right refer to the current display."),
            paragraph("F10, Escape or q requests closing. Unsaved merged files require Save, Discard or Cancel decisions. Standalone mcppdiff exits instead of returning to manager panels."),
            paragraph("Comparisons and merges require local text files. Archive members need a local copy. This is not a remote comparison or a general binary merge tool."),
            link(editor_save, "Save protections"),
            link(dialogs, "Dialogs and text inputs"),
            link(viewer_search, "Search modes"),
        }},
        {dialogs, "Dialogs and text inputs", {
            paragraph("Tab and Shift-Tab move between fields and buttons. Enter accepts an input or activates the focused button; Space activates a focused button or toggles an offered choice. Escape cancels where cancellation is available."),
            paragraph("Inputs support Left/Right, Home/End, Ctrl-Left/Ctrl-Right, selection with Shift, and Delete/Backspace at the cursor. Ctrl-U clears a dialog field, Ctrl-K kills to the end, Ctrl-W kills the previous word and Ctrl-Y yanks."),
            paragraph("Alt-P/Alt-N recall input history while preserving an unfinished draft. Named histories can persist across sessions. Alt-Tab completes applicable local paths; a further press can show the choices."),
            paragraph("Relative paths use the dialog's stated context: manager paths normally start in the active panel, while editor paths start in the file's directory. Completion does not execute commands or contact remote systems."),
            paragraph("Opening help leaves the dialog, focus, selection and input in place. Closing help returns to that same pending decision. Key-learning, macro-hotkey and literal-key prompts capture keys instead of invoking Help."),
            link(keymaps, "Configurable keys"),
            link(editor_save, "Editor save decisions"),
            link(copy_move, "Copy/move decisions"),
        }},
        {settings, "Display and saved preferences", {
            paragraph("The Options menu provides Configuration, Layout, Panel options, Confirmation, Appearance, Display bits and Learn keys. Use the controls actually shown; not every native MC setting has a matching mcpp feature."),
            paragraph("Appearance selects an independently authored skin and optional dialog shadows. A preview can be cancelled. Layout changes panel arrangement and visible bars; panel options changes listing/navigation behavior."),
            paragraph("The terminal renderer requires UTF-8. Display bits reports this fixed terminal mode. A pane, viewer or editor codepage chooser interprets file data, not filenames or terminal output."),
            paragraph("Editor options controls tabs, indentation, whitespace and other supported editing preferences. Codepage changes preserve dirty text and history or report a conversion error without replacing them."),
            paragraph("Save setup writes the supported preferences. Command-line overrides are not saved. mcpp does not provide all MC features or resources, and remote work remains paused."),
            link(keymaps, "Configurable keys"),
            link(dialogs, "Dialogs and text inputs"),
            link(editor, "Text editor"),
        }},
        {keymaps, "Configurable keys", {
            paragraph("The keys printed in help are defaults. A keymap supplied with -K/--keymap, a saved setting, MCPP_KEYMAP or the usual mcpp.keymap search can replace action bindings. --nokeymap disables loading one."),
            paragraph("Resources use sections such as filemanager, editor, viewer, diffviewer, dialog and help. Each action takes semicolon-separated keys. Replacing an action replaces its default list; an empty list unbinds it. Untouched actions keep their defaults."),
            paragraph("Bind Help in the screen's section, or dialog for dialog help. In the help section, supported actions include Help, Index, Back, Quit, Up, Down, PageUp, PageDown, Top, Bottom, LinkNext, LinkPrev and Enter."),
            paragraph("ExtendedKeyMap enters a one-key prefix map for the manager or editor. Escape cancels the prefix. Learned terminal sequences are translated before action lookup."),
            paragraph("Literal input and key-learning prompts keep their own key capture. Editor macro hotkeys are checked before ordinary configured dispatch. Browser navigation does not record edits or run editor macros."),
            paragraph("Invalid resources identify their file and location. An unavailable action reports an error rather than running an unrelated command."),
            link(browser, "Using this help browser"),
            link(dialogs, "Dialogs and text inputs"),
        }},
        {archives, "Read-only local archives", {
            paragraph("Enter opens supported tar/cpio files as read-only panels. Navigate their directories normally. Ctrl-PageUp at the archive root returns to the containing local directory."),
            paragraph("F3 and copying out read the archive member, not a similarly named host path. Editing can retain a draft, but Save cannot write back into the archive; Save as can publish a local copy."),
            paragraph("Native VFS paths use /utar:// or /ucpio:// after the container filename. Ctrl-X then a lists active cached archives. Existing users of an archive keep their snapshot when the container changes."),
            paragraph("Supported formats include common ustar/GNU/PAX tar and newc/CRC cpio. Compressed files require the applicable already installed decompressor. Missing tools and unsupported formats report errors."),
            paragraph("Reading is bounded to 64 MiB encoded, 256 MiB expanded, 100,000 members and 4,096 bytes per member path. Nested archives, sparse/special members and other VFS families are not supported."),
            paragraph("External file commands and comparisons require a local copy. Remote providers remain paused."),
            link(copy_move, "Copying archive members out"),
            link(editor_save, "Saving a local editor copy"),
        }},
        {permissions, "Permissions, ownership and links", {
            paragraph("The File menu provides permissions, ownership, advanced ownership, attributes and link commands. They use marked targets when there are any; check the displayed source and count."),
            paragraph("Permission/owner dialogs expose the supported local filesystem values. Applying them still requires ordinary Linux permissions and filesystem support. Errors do not imply the other targets were changed successfully."),
            paragraph("Hard links and symbolic links are different operations. Relative symbolic links are resolved from the new link's directory. Stable-link handling during copying can retain a local relative link's original referent."),
            paragraph("Read-only archives cannot accept metadata or link changes. Help does not apply a pending change; close it to return to the same dialog."),
            link(dialogs, "Dialogs and text inputs"),
            link(copy_move, "Copying and moving"),
            link(archives, "Read-only local archives"),
        }},
    };
    return pages;
}

const HelpPage& help_page(const HelpTopic topic) {
    const auto pages = help_pages();
    const auto found = std::ranges::find(pages, topic, &HelpPage::topic);
    if (found == pages.end()) throw std::invalid_argument("unknown help topic");
    return *found;
}

HelpBrowser::HelpBrowser(const HelpTopic topic) : topic_(topic) {
    layout();
    select_visible();
}

HelpBrowser::Position HelpBrowser::position() const {
    return {topic_, anchors_.at(first_), selected_};
}

std::size_t HelpBrowser::last_top() const noexcept {
    return lines_.size() > rows_ ? lines_.size() - rows_ : 0U;
}

bool HelpBrowser::visible(const std::size_t index) const {
    const auto& item = links_.at(index);
    return item.last_line >= first_ && item.first_line - std::min(item.first_line, first_) < rows_;
}

void HelpBrowser::layout() {
    lines_.clear();
    anchors_.clear();
    links_.clear();
    const auto& blocks = help_page(topic_).blocks;
    for (std::size_t block = 0; block < blocks.size(); ++block) {
        const auto& source = blocks[block];
        if (block != 0U && (!source.target || !blocks[block - 1U].target)) {
            lines_.push_back({"", std::nullopt, false});
            anchors_.push_back({block - 1U, blocks[block - 1U].text.size() + 2U});
        }
        const auto text = source.target ? "> " + std::string(source.text) : std::string(source.text);
        const auto selected = source.target ? std::optional(links_.size()) : std::nullopt;
        const auto first = lines_.size();
        std::size_t offset = 0;
        while (offset < text.size()) {
            auto length = std::min(columns_, text.size() - offset);
            if (length < text.size() - offset && text[offset + length] != ' ') {
                const auto space = text.rfind(' ', offset + length);
                if (space != std::string::npos && space > offset) length = space - offset;
            }
            lines_.push_back({text.substr(offset, length), selected, source.heading});
            anchors_.push_back({block, offset});
            offset += length;
            while (offset < text.size() && text[offset] == ' ') ++offset;
        }
        if (source.target) links_.push_back({*source.target, first, lines_.size() - 1U});
    }
    if (lines_.empty()) {
        lines_.push_back({"", std::nullopt, false});
        anchors_.push_back({});
    }
}

void HelpBrowser::restore(const Position& saved) {
    first_ = 0;
    for (std::size_t index = 0; index < anchors_.size(); ++index) {
        const auto& anchor = anchors_[index];
        if (anchor.block > saved.anchor.block ||
            (anchor.block == saved.anchor.block && anchor.byte > saved.anchor.byte)) break;
        first_ = index;
    }
    first_ = std::min(first_, last_top());
    selected_ = saved.selected;
    select_visible();
}

void HelpBrowser::resize(const std::size_t columns, const std::size_t rows) {
    const auto saved = position();
    const auto width = std::max<std::size_t>(1U, columns);
    rows_ = std::max<std::size_t>(1U, rows);
    if (columns_ != width) {
        columns_ = width;
        layout();
    }
    restore(saved);
}

void HelpBrowser::select_visible() {
    if (selected_ && *selected_ < links_.size() && visible(*selected_)) return;
    selected_.reset();
    for (std::size_t index = 0; index < links_.size(); ++index) {
        if (visible(index)) {
            selected_ = index;
            return;
        }
    }
}

void HelpBrowser::scroll(const int direction, const std::size_t amount) {
    if (direction < 0) first_ -= std::min(first_, amount);
    else if (direction > 0) first_ += std::min(last_top() - first_, amount);
    select_visible();
}

void HelpBrowser::page(const int direction) {
    scroll(direction, std::max<std::size_t>(1U, rows_ - 1U));
}

void HelpBrowser::top() {
    first_ = 0;
    select_visible();
}

void HelpBrowser::bottom() {
    first_ = last_top();
    select_visible();
}

void HelpBrowser::reveal_selected() {
    if (!selected_) return;
    const auto& item = links_.at(*selected_);
    if (item.first_line < first_) first_ = item.first_line;
    else if (item.first_line - first_ >= rows_) first_ = item.first_line - rows_ + 1U;
    first_ = std::min(first_, last_top());
}

void HelpBrowser::next_link(const bool backwards) {
    if (links_.empty()) return;
    if (selected_) {
        selected_ = backwards ? (*selected_ + links_.size() - 1U) % links_.size()
                              : (*selected_ + 1U) % links_.size();
    } else if (backwards) {
        selected_ = links_.size() - 1U;
        for (std::size_t index = links_.size(); index != 0U; --index) {
            if (links_[index - 1U].first_line < first_ + rows_) {
                selected_ = index - 1U;
                break;
            }
        }
    } else {
        selected_ = 0;
        for (std::size_t index = 0; index < links_.size(); ++index) {
            if (links_[index].last_line >= first_) {
                selected_ = index;
                break;
            }
        }
    }
    reveal_selected();
}

bool HelpBrowser::follow() {
    return selected_ && follow_link(*selected_);
}

bool HelpBrowser::follow_link(const std::size_t index) {
    if (index >= links_.size()) return false;
    selected_ = index;
    open(links_[index].target);
    return true;
}

bool HelpBrowser::back() {
    if (history_.empty()) return false;
    const auto saved = history_.back();
    history_.pop_back();
    topic_ = saved.topic;
    layout();
    restore(saved);
    return true;
}

void HelpBrowser::open(const HelpTopic topic) {
    static_cast<void>(help_page(topic));
    if (topic == topic_) {
        top();
        return;
    }
    if (history_.size() == 128U) history_.erase(history_.begin());
    history_.push_back(position());
    topic_ = topic;
    first_ = 0;
    selected_.reset();
    layout();
    select_visible();
}

HelpTopic HelpBrowser::topic() const noexcept { return topic_; }
std::span<const HelpLine> HelpBrowser::lines() const noexcept { return lines_; }
std::span<const HelpLink> HelpBrowser::links() const noexcept { return links_; }
std::size_t HelpBrowser::first_line() const noexcept { return first_; }
std::size_t HelpBrowser::page_rows() const noexcept { return rows_; }
std::optional<std::size_t> HelpBrowser::selected_link() const noexcept { return selected_; }
bool HelpBrowser::can_go_back() const noexcept { return !history_.empty(); }

}  // namespace mcpp
