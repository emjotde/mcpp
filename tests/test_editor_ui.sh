#!/usr/bin/env bash
set -euo pipefail

binary="$1"
script_command="$2"
timeout_command="$3"
temporary="$(mktemp -d /tmp/mcpp-editor-ui.XXXXXX)"
trap 'rm -rf -- "$temporary"' EXIT
mkdir -p "$temporary/left" "$temporary/right"
printf 'alpha beta\n' >"$temporary/left/a.txt"
mkfifo "$temporary/input"

printf -v command 'XDG_CONFIG_HOME=%q %q %q %q' \
    "$temporary/config" \
    "$binary" "$temporary/left" "$temporary/right"
"$timeout_command" 15s "$script_command" \
    -qefc "$command" /dev/null \
    <"$temporary/input" \
    >"$temporary/output" 2>"$temporary/error" &
runner=$!

exec 3>"$temporary/input"
sleep 1
printf '\033[B\033[14~' >&3
sleep 1
# Ctrl-Z is an editor word-motion key, not shell job control.
printf '\033[F\032\033[H' >&3
# Mark "alpha", use the clipboard once, copy it twice, then replace "beta".
printf '\033[1;2C\033[1;2C\033[1;2C\033[1;2C\033[1;2C\033[2;5~\033[F\033[2;2~\033[15~\033[15~' >&3
printf '\033[14~beta\rBETA\ro\033[12~' >&3
sleep 1
# Exercise the modified-file close dialog and discard only the last X.
printf 'X\033[21~' >&3
sleep 1
printf 'n\033[21~' >&3
set +e
wait "$runner"
status=$?
set -e
exec 3>&-

if ((status != 0)); then
    printf 'mcpp editor UI test failed with status %d\n' "$status" >&2
    cat "$temporary/error" >&2
    exit 1
fi

if [[ "$(cat "$temporary/left/a.txt")" != "alpha BETAalphaalphaalpha" ]]; then
    printf 'mcpp editor key workflow produced unexpected content\n' >&2
    exit 1
fi

if ! grep -Fq 'Save' "$temporary/output" ||
   ! grep -Fq 'Mark' "$temporary/output" ||
   ! grep -Fq 'Close file' "$temporary/output"; then
    printf 'mcpp editor chrome or close dialog was not rendered\n' >&2
    exit 1
fi

mkfifo "$temporary/direct-input"
printf -v direct_command 'XDG_CONFIG_HOME=%q %q -e %q' \
    "$temporary/direct-config" \
    "$binary" "$temporary/left/a.txt"
"$timeout_command" 8s "$script_command" \
    -qefc "$direct_command" /dev/null \
    <"$temporary/direct-input" \
    >"$temporary/direct-output" 2>"$temporary/direct-error" &
direct_runner=$!
exec 4>"$temporary/direct-input"
sleep 1
printf '\033' >&4
set +e
wait "$direct_runner"
direct_status=$?
set -e
exec 4>&-

if ((direct_status != 0)); then
    printf 'direct editor Escape did not exit cleanly\n' >&2
    cat "$temporary/direct-error" >&2
    exit 1
fi
