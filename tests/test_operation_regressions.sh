#!/usr/bin/env bash
set -euo pipefail

binary="${1:?usage: test_operation_regressions.sh /review/bin/mcpp [case ...]}"
shift
if [[ "$binary" != /review/bin/mcpp ]] ||
   [[ "${MCPP_DISPOSABLE_TESTS:-}" != 1 ]] ||
   [[ "${TMPDIR:-}" != /review/runtime ]] ||
   [[ ! -f /.dockerenv ]] ||
   [[ ! -f /review/.operation-regressions-disposable ]]; then
    printf 'Refusing operation/error-injection tests outside the disposable runner.\n' >&2
    exit 2
fi

root=/review
runner=
current=
output=
offset=0
background_bytes="${MCPP_BG_BYTES:-1073741824}"
[[ "$background_bytes" =~ ^[0-9]+$ ]] &&
    ((background_bytes >= 268435456 && background_bytes <= 2147483648)) || exit 2
mkdir -p "$root/cases" "$root/evidence" "$root/homes"

stop_session() {
    exec 3>&- || true
    if [[ -n "$runner" ]]; then
        if kill -0 "$runner" 2>/dev/null; then
            kill "$runner" 2>/dev/null || true
        fi
        wait "$runner" 2>/dev/null || true
        runner=
    fi
    if [[ -n "${name:-}" ]]; then
        rm -f -- "$root/evidence/$name/input"
    fi
}

snapshot() {
    local stage="$1"
    shift
    {
        for path in "$current" "$@"; do
            printf 'ROOT %s\n' "$path"
            find "$path" -printf \
                '%y mode=%m uid=%U gid=%G size=%s inode=%i links=%n mtime=%T@ path=%p target=%l\n' |
                LC_ALL=C sort
            find "$path" -type f -exec sha256sum '{}' + | LC_ALL=C sort
        done
    } >"$root/evidence/$name/$stage.fs.txt"
}

finish() {
    local status=$?
    stop_session
    if ((status != 0)) && [[ -n "$current" ]]; then
        snapshot failure || true
        tail -c 4000 "$output" >&2 || true
    fi
    return "$status"
}
trap finish EXIT

fail() {
    printf '%s: %s\n' "$name" "$*" >&2
    return 1
}

make_case() {
    name="$1"
    current="$root/cases/$name"
    mkdir -m 700 "$current"
    mkdir "$current/src" "$current/dst" "$current/expected"
    mkdir -p "$root/evidence/$name"
    output="$root/evidence/$name/terminal.ansi"
    printf 'SOURCE\n' >"$current/src/a.txt"
    cp "$current/src/a.txt" "$current/expected/source"
    chmod 751 "$current/src/a.txt"
}

start_session() {
    local home="$root/homes/$name"
    mkdir -p "$home"/{config,cache,data}
    mkfifo "$root/evidence/$name/input"
    local command
    printf -v command \
        'stty cols 112 rows 32; exec env HOME=%q XDG_CONFIG_HOME=%q XDG_CACHE_HOME=%q XDG_DATA_HOME=%q TMPDIR=/review/runtime TERM=xterm-256color LANG=C.UTF-8 LC_ALL=C.UTF-8 %q %q %q' \
        "$home" "$home/config" "$home/cache" "$home/data" \
        "$binary" "$current/src" "$current/dst"
    timeout 120s script -qefc "$command" /dev/null \
        <"$root/evidence/$name/input" >"$output" 2>&1 &
    runner=$!
    exec 3>"$root/evidence/$name/input"
    offset=0
    wait_log 'Hint:'
}

checkpoint() {
    offset="$(wc -c <"$output")"
}

has_log() {
    tail -c "+$((offset + 1))" "$output" |
        sed -E 's/\x1B\[[0-?]*[ -/]*[@-~]//g' |
        grep -aF -- "$1" >/dev/null
}

wait_log() {
    local needle="$1"
    local deadline=$((SECONDS + 60))
    until has_log "$needle"; do
        if ! kill -0 "$runner" 2>/dev/null || ((SECONDS >= deadline)); then
            fail "missing terminal state: $needle"
            return 1
        fi
        sleep 0.05
    done
}

send() {
    printf '%b' "$1" >&3
}

replace_destination() {
    local previous="$1"
    local destination="$2"
    local index
    for ((index = 0; index < ${#previous}; ++index)); do
        send '\177'
    done
    printf '%s' "$destination" >&3
}

quit_session() {
    send '\033[21~'
    wait "$runner"
    runner=
    exec 3>&-
    rm -f -- "$root/evidence/$name/input"
}

wait_publication() {
    local deadline=$((SECONDS + 90))
    until [[ -f "$current/dst/a.bin" ]] &&
          [[ "$(stat -c %s "$current/dst/a.bin")" == "$background_bytes" ]]; do
        ((SECONDS < deadline)) || { fail 'background copy timed out'; return 1; }
        sleep 0.05
    done
    sleep 1
    cmp -s "$current/src/a.bin" "$current/dst/a.bin"
}

copy_exact() {
    make_case copy-exact
    snapshot before
    start_session
    send '\033[B\033[15~'
    wait_log 'Source mask:'
    replace_destination "$current/dst" "$current/dst/renamed.txt"
    checkpoint
    send '\r'
    wait_log 'Copy complete'
    [[ -f "$current/dst/renamed.txt" && ! -L "$current/dst/renamed.txt" ]]
    cmp -s "$current/expected/source" "$current/dst/renamed.txt"
    [[ "$(stat -c %a "$current/dst/renamed.txt")" == 751 ]]
    snapshot after
    quit_session
}

copy_relative() {
    make_case copy-relative
    snapshot before
    start_session
    send '\033[B\033[15~'
    wait_log 'Source mask:'
    replace_destination "$current/dst" relative.txt
    checkpoint
    send '\r'
    wait_log 'Copy complete'
    cmp -s "$current/expected/source" "$current/src/relative.txt"
    [[ ! -e "$current/dst/relative.txt" ]]
    snapshot after
    quit_session
}

move_directory() {
    make_case move-directory
    mkdir "$current/dst/bucket"
    printf 'KEEP\n' >"$current/dst/bucket/KEEP"
    cp "$current/dst/bucket/KEEP" "$current/expected/keep"
    snapshot before
    start_session
    send '\033[B\033[17~'
    wait_log 'Source mask:'
    replace_destination "$current/dst" "$current/dst/bucket"
    checkpoint
    send '\r'
    local deadline=$((SECONDS + 60))
    until has_log 'Move complete'; do
        if has_log 'Destination exists'; then
            checkpoint
            send '\r'
        fi
        ((SECONDS < deadline)) || { fail 'move did not finish'; return 1; }
        sleep 0.05
    done
    [[ -d "$current/dst/bucket" && ! -e "$current/src/a.txt" ]]
    cmp -s "$current/expected/keep" "$current/dst/bucket/KEEP"
    cmp -s "$current/expected/source" "$current/dst/bucket/a.txt"
    snapshot after
    quit_session
}

symlink_overwrite() {
    make_case symlink-overwrite
    rm "$current/src/a.txt"
    mkdir "$current/targets"
    printf 'NEW-TARGET\n' >"$current/targets/new"
    printf 'OLD-TARGET\n' >"$current/targets/old"
    chmod 600 "$current/targets/new"
    chmod 640 "$current/targets/old"
    cp "$current/targets/new" "$current/expected/new"
    cp "$current/targets/old" "$current/expected/old"
    ln -s ../targets/new "$current/src/link"
    ln -s ../targets/old "$current/dst/link"
    local before_new before_old
    before_new="$(stat -c '%d:%i:%a:%u:%g:%h:%y' "$current/targets/new")"
    before_old="$(stat -c '%d:%i:%a:%u:%g:%h:%y' "$current/targets/old")"
    snapshot before
    start_session
    send '\033[B\033[15~'
    wait_log 'Source mask:'
    send '\t\t \r'
    wait_log 'Destination exists'
    checkpoint
    send '\r'
    wait_log 'Copy complete'
    [[ "$(readlink "$current/dst/link")" == ../targets/new ]]
    cmp -s "$current/expected/new" "$current/targets/new"
    cmp -s "$current/expected/old" "$current/targets/old"
    [[ "$before_new" == "$(stat -c '%d:%i:%a:%u:%g:%h:%y' "$current/targets/new")" ]]
    [[ "$before_old" == "$(stat -c '%d:%i:%a:%u:%g:%h:%y' "$current/targets/old")" ]]
    [[ -z "$(find "$current" -name '.mcpp-*' -print)" ]]
    snapshot after
    quit_session
}

background_setup() {
    make_case "$1"
    rm "$current/src/a.txt"
    truncate -s "$background_bytes" "$current/src/a.bin"
    chmod 751 "$current/src/a.bin"
    printf 'CHOICE\n' >"$current/src/z-choice.txt"
    printf 'ORIGINAL\n' >"$current/dst/note.txt"
    snapshot before
    start_session
    if [[ "$1" == background-notice ]]; then
        local parent child
        notice_pid=
        for parent in $(cat "/proc/$runner/task/$runner/children"); do
            for child in $(cat "/proc/$parent/task/$parent/children"); do
                if [[ "$(cat "/proc/$child/comm")" == mcpp ]]; then
                    notice_pid="$child"
                    break
                fi
            done
        done
        [[ -n "$notice_pid" ]] || { fail 'application process not found'; return 1; }
        notice_threads="$(find "/proc/$notice_pid/task" -mindepth 1 -maxdepth 1 -type d | wc -l)"
    fi
    send '\033[B\033[2~\033[15~'
    wait_log 'Source mask:'
    checkpoint
    send '\t\t\t\t \r'
}

open_note() {
    [[ ! -e "$current/dst/a.bin" ]] ||
        fail 'copy completed before navigation; increase MCPP_BG_BYTES'
    send '\t\033[H\033[B'
    send "$1"
    wait_log ORIGINAL
    [[ ! -e "$current/dst/a.bin" ]] ||
        fail 'copy completed before modal opened; increase MCPP_BG_BYTES'
}

background_editor() {
    background_setup background-editor
    open_note '\033OS'
    printf 'DIRTY-' >&3
    wait_publication
    printf 'AFTER-' >&3
    checkpoint
    send '\033[21~'
    wait_log 'Save before close?'
    send 'c'
    checkpoint
    send '\033[21~'
    wait_log 'Save before close?'
    checkpoint
    send 'y'
    wait_log 'Editor closed'
    printf 'DIRTY-AFTER-ORIGINAL\n' >"$current/expected/edited"
    cmp -s "$current/expected/edited" "$current/dst/note.txt"
    snapshot after
    quit_session
}

background_notice() {
    background_setup background-notice
    open_note '\033OS'
    printf 'DIRTY-' >&3
    local deadline=$((SECONDS + 90))
    until [[ -n "$(find "$current/dst" -maxdepth 1 -name '.mcpp-write-*' -print)" ]]; do
        ((SECONDS < deadline)) || { fail 'copy staging file not found'; return 1; }
        sleep 0.05
    done
    mkdir "$current/dst/a.bin"
    printf 'KEEP\n' >"$current/dst/a.bin/KEEP"
    until [[ -z "$(find "$current/dst" -maxdepth 1 -name '.mcpp-write-*' -print)" ]] &&
          [[ "$(find "/proc/$notice_pid/task" -mindepth 1 -maxdepth 1 -type d | wc -l)" -le "$notice_threads" ]]; do
        ((SECONDS < deadline)) || { fail 'failed copy worker did not finish'; return 1; }
        sleep 0.05
    done
    printf 'AFTER-' >&3
    send '\033OQ'
    printf 'DIRTY-AFTER-ORIGINAL\n' >"$current/expected/edited"
    until cmp -s "$current/expected/edited" "$current/dst/note.txt"; do
        ((SECONDS < deadline)) || { fail 'editor input/save lost after background error'; return 1; }
        sleep 0.05
    done
    checkpoint
    send '\033[21~'
    wait_log 'Background [Esc]: Error:'
    [[ "$(cat "$current/dst/a.bin/KEEP")" == KEEP ]]
    [[ "$(stat -c %s "$current/src/a.bin")" == "$background_bytes" ]]
    [[ -z "$(find "$current/dst" -name '.mcpp-write-*' -print)" ]]
    snapshot after
    checkpoint
    send '\033'
    wait_log 'Hint:'
    quit_session
}

background_viewer() {
    background_setup background-viewer
    open_note '\033OR'
    wait_publication
    checkpoint
    send '\033[21~\033[18~'
    wait_log 'New directory'
    checkpoint
    printf 'viewer-survived\r' >&3
    wait_log 'Directory created'
    [[ -d "$current/dst/viewer-survived" ]]
    [[ "$(cat "$current/dst/note.txt")" == ORIGINAL ]]
    snapshot after
    quit_session
}

background_prompt() {
    background_setup background-prompt
    send '\t\033[18~'
    wait_log 'New directory'
    printf 'prompt-survived' >&3
    [[ ! -e "$current/dst/a.bin" ]] ||
        fail 'copy completed before prompt opened; increase MCPP_BG_BYTES'
    wait_publication
    checkpoint
    send '\r'
    wait_log 'Directory created'
    [[ -d "$current/dst/prompt-survived" ]]
    snapshot after
    quit_session
}

background_swap() {
    make_case background-swap
    rm "$current/src/a.txt"
    truncate -s "$background_bytes" "$current/src/a.bin"
    chmod 751 "$current/src/a.bin"
    printf 'CHOICE\n' >"$current/src/z-choice.txt"
    printf 'KEEP\n' >"$current/dst/keep.txt"
    snapshot before
    start_session
    send '\033[B\033[2~\t\033[B\033[2~\t\033[15~'
    wait_log 'Source mask:'
    checkpoint
    send '\t\t\t\t \r'
    [[ ! -e "$current/dst/a.bin" ]] ||
        fail 'copy completed before swap; increase MCPP_BG_BYTES'
    send '\025'
    wait_log 'Copy complete'
    cmp -s "$current/src/a.bin" "$current/dst/a.bin"
    checkpoint
    send '\033[F\033[19~\r'
    wait_log 'Delete complete'
    [[ ! -e "$current/src/z-choice.txt" && -f "$current/src/a.bin" ]]
    checkpoint
    send '\t\033[H\033[B\033[19~\r'
    wait_log 'Delete complete'
    [[ ! -e "$current/dst/keep.txt" && -f "$current/dst/a.bin" ]]
    cmp -s "$current/src/a.bin" "$current/dst/a.bin"
    snapshot after
    quit_session
}

panelized_skip() {
    make_case panelized-skip
    rm "$current/src/a.txt"
    mkdir "$current/src/a" "$current/src/b"
    printf 'FIRST\n' >"$current/src/a/same.txt"
    printf 'SECOND\n' >"$current/src/b/same.txt"
    cp "$current/src/a/same.txt" "$current/expected/first"
    snapshot before
    start_session
    # Up wraps to Rescan; the preceding enabled row is Panelize by name.
    send '\033[20~\r\033[A\033[A\r'
    wait_log 'Panelize name pattern'
    checkpoint
    send '\025*.txt\r'
    wait_log '2 panelized result(s)'
    send '\001\033[15~'
    wait_log 'Source mask:'
    checkpoint
    send '\r'
    wait_log 'Destination exists'
    checkpoint
    send 'n'
    wait_log 'Copy incomplete: 1 skipped'
    has_log 'destination exists:'
    ! has_log 'Copy complete'
    cmp -s "$current/expected/first" "$current/dst/same.txt"
    [[ -f "$current/src/a/same.txt" && -f "$current/src/b/same.txt" ]]
    snapshot after-copy
    checkpoint
    send '\033[19~\r'
    wait_log 'Delete complete'
    [[ -f "$current/src/a/same.txt" && ! -e "$current/src/b/same.txt" ]]
    cmp -s "$current/expected/first" "$current/dst/same.txt"
    snapshot after-selected-delete
    quit_session
}

disk_full() {
    make_case disk-full
    [[ "$(stat -f -c %T /review/full)" == tmpfs ]]
    local destination=/review/full/disk-full
    mkdir "$destination"
    rm "$current/src/a.txt"
    truncate -s 16777216 "$current/src/a.bin"
    printf 'OLD-DESTINATION\n' >"$destination/a.bin"
    cp "$destination/a.bin" "$current/expected/old"
    chmod 640 "$destination/a.bin"
    snapshot before "$destination"
    start_session
    send '\033[B\033[15~'
    wait_log 'Source mask:'
    replace_destination "$current/dst" "$destination"
    send '\r'
    wait_log 'Destination exists'
    checkpoint
    send '\r'
    wait_log 'File operation error'
    cmp -s "$current/expected/old" "$destination/a.bin"
    [[ "$(stat -c %a "$destination/a.bin")" == 640 ]]
    [[ "$(stat -c %s "$current/src/a.bin")" == 16777216 ]]
    [[ -z "$(find "$destination" -name '.mcpp-*' -print)" ]]
    snapshot after "$destination"
    checkpoint
    send '\033'
    wait_log 'Operation cancelled'
    quit_session
}

move_directory_alias() {
    make_case "$1"
    local link_target="$2"
    rm "$current/src/a.txt"
    mkdir -p "$current/src/tree/child"
    printf 'ONLY-SOURCE-COPY\n' >"$current/src/tree/important.txt"
    printf 'CHILD-KEEP\n' >"$current/src/tree/child/KEEP"
    cp "$current/src/tree/important.txt" "$current/expected/important"
    cp "$current/src/tree/child/KEEP" "$current/expected/child"
    ln -s "$link_target" "$current/dst/tree"
    local file_before link_before
    file_before="$(stat -c '%d:%i:%a:%u:%g:%h:%y' "$current/src/tree/important.txt")"
    link_before="$(stat -c '%d:%i:%a:%u:%g:%h:%y' "$current/dst/tree")"
    snapshot before
    start_session
    send '\033[B\033[2~\033[17~'
    wait_log 'Source mask:'
    send '\r'
    wait_log 'Error:'
    cmp -s "$current/expected/important" "$current/src/tree/important.txt"
    cmp -s "$current/expected/child" "$current/src/tree/child/KEEP"
    [[ "$file_before" == "$(stat -c '%d:%i:%a:%u:%g:%h:%y' "$current/src/tree/important.txt")" ]]
    [[ "$link_before" == "$(stat -c '%d:%i:%a:%u:%g:%h:%y' "$current/dst/tree")" ]]
    [[ "$(readlink "$current/dst/tree")" == "$link_target" ]]
    checkpoint
    send '\033[H\033[19~'
    wait_log 'Delete 1 selected item(s)?'
    send '\t\r'
    snapshot after
    quit_session
}

move_hardlink() {
    make_case "$1"
    local exact="$2"
    ln "$current/src/a.txt" "$current/dst/a.txt"
    local before
    before="$(stat -c '%d:%i:%a:%u:%g:%h:%y' "$current/src/a.txt")"
    [[ "$(stat -c %h "$current/src/a.txt")" == 2 ]]
    snapshot before
    start_session
    send '\033[B\033[2~\033[17~'
    wait_log 'Source mask:'
    if [[ "$exact" == true ]]; then
        replace_destination "$current/dst" "$current/dst/a.txt"
    fi
    send '\r'
    wait_log 'Move incomplete: 1 skipped'
    has_log 'same file'
    ! has_log 'Move complete'
    cmp -s "$current/expected/source" "$current/src/a.txt"
    cmp -s "$current/expected/source" "$current/dst/a.txt"
    [[ "$before" == "$(stat -c '%d:%i:%a:%u:%g:%h:%y' "$current/src/a.txt")" ]]
    [[ "$before" == "$(stat -c '%d:%i:%a:%u:%g:%h:%y' "$current/dst/a.txt")" ]]
    checkpoint
    send '\033[H\033[19~'
    wait_log 'Delete 1 selected item(s)?'
    send '\t\r'
    snapshot after
    quit_session
}

if (($# == 0)); then
    set -- copy-exact copy-relative move-directory symlink-overwrite \
        background-editor background-notice background-viewer background-prompt background-swap \
        panelized-skip disk-full move-self-alias move-descendant-alias \
        move-hardlink move-exact-hardlink
fi
for name in "$@"; do
    if [[ "${MCPP_UI_GROUP:-all}" == foreground && "$name" == background-* ]] ||
       [[ "${MCPP_UI_GROUP:-all}" == background && "$name" != background-* ]]; then
        continue
    fi
    case "$name" in
        copy-exact) copy_exact ;;
        copy-relative) copy_relative ;;
        move-directory) move_directory ;;
        symlink-overwrite) symlink_overwrite ;;
        background-editor) background_editor ;;
        background-notice) background_notice ;;
        background-viewer) background_viewer ;;
        background-prompt) background_prompt ;;
        background-swap) background_swap ;;
        panelized-skip) panelized_skip ;;
        disk-full) disk_full ;;
        move-self-alias) move_directory_alias move-self-alias ../src/tree ;;
        move-descendant-alias) move_directory_alias move-descendant-alias ../src/tree/child ;;
        move-hardlink) move_hardlink move-hardlink false ;;
        move-exact-hardlink) move_hardlink move-exact-hardlink true ;;
        *) printf 'Unknown regression: %s\n' "$name" >&2; exit 2 ;;
    esac
    printf '[PASS] %s\n' "$name"
done
