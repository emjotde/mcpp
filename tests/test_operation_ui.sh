#!/usr/bin/env bash
set -euo pipefail

binary="$1"
script_command="$2"
timeout_command="$3"
temporary="$(mktemp -d /tmp/mcpp-operation-ui.XXXXXX)"
trap 'rm -rf -- "$temporary"' EXIT
mkdir -p "$temporary/left" "$temporary/right"
dd if=/dev/zero of="$temporary/left/a.bin" \
    bs=1M count=64 status=none
dd if=/dev/zero of="$temporary/left/b.bin" \
    bs=1M count=64 status=none
mkfifo "$temporary/input"

printf -v command 'XDG_CONFIG_HOME=%q %q %q %q' \
    "$temporary/config" \
    "$binary" "$temporary/left" "$temporary/right"
"$timeout_command" 20s "$script_command" \
    -qefc "$command" /dev/null \
    <"$temporary/input" \
    >"$temporary/output" 2>"$temporary/error" &
runner=$!

exec 3>"$temporary/input"
sleep 1
# Select both files, start copy, pause at the first progress checkpoint,
# skip that entry, and allow the next entry to complete.
printf '\033[B\033[2~\033[2~\033[15~\r ' >&3
sleep 1
printf 's' >&3
sleep 3
redraw_complete=false
if grep -aFq 'Copy complete' "$temporary/output"; then
    redraw_complete=true
fi
printf '\033[21~' >&3
set +e
wait "$runner"
status=$?
set -e
exec 3>&-

if ((status != 0)); then
    printf 'operation UI test failed with status %d\n' "$status" >&2
    cat "$temporary/error" >&2
    exit 1
fi

if [[ "$redraw_complete" != true ]]; then
    printf 'operation completion did not redraw without input\n' >&2
    exit 1
fi

if [[ -e "$temporary/right/a.bin" ]] ||
   [[ ! -f "$temporary/right/b.bin" ]]; then
    printf 'pause/skip did not skip only the current entry\n' >&2
    exit 1
fi
