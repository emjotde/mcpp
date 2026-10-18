#!/usr/bin/env bash
set -euo pipefail

binary="$1"
script_command="$2"
timeout_command="$3"
temporary="$PWD/mcpp-editor-ui-$BASHPID"
mkdir -m 700 -- "$temporary"
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
printf '\033[14~beta\rBETA\ro\033[12~\r' >&3
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

printf 'a b\tc  \n' >"$temporary/options.txt"
mkfifo "$temporary/options-input"
printf -v options_command 'XDG_CONFIG_HOME=%q %q -e %q' \
    "$temporary/options-config" \
    "$binary" "$temporary/options.txt"
"$timeout_command" 12s "$script_command" \
    -qefc "$options_command" /dev/null \
    <"$temporary/options-input" \
    >"$temporary/options-output" 2>"$temporary/options-error" &
options_runner=$!
exec 5>"$temporary/options-input"
sleep 1
printf '\033[20~\033[C\033[C\033[C\033[C\033[C\033[C\r\r' >&5
sleep 0.5
printf '8\t2\t\t \t\r' >&5
sleep 0.5
printf '\033[20~\033[C\033[C\033[C\033[C\033[C\033[C\033[F\r' >&5
sleep 0.5
printf '\033[B\tX\033[A\033[F\tY\033[12~\r' >&5
sleep 0.5
printf '\033[21~' >&5
set +e
wait "$options_runner"
options_status=$?
set -e
exec 5>&-

if ((options_status != 0)); then
    printf 'editor options workflow failed with status %d\n' \
        "$options_status" >&2
    cat "$temporary/options-error" >&2
    exit 1
fi

if [[ "$(cat "$temporary/options.txt")" != \
      $'a b\tc  \tY\n  X' ]]; then
    printf 'configured editor indentation was not applied\n' >&2
    exit 1
fi

if ! grep -aFq 'Editor options' "$temporary/options-output" ||
   ! grep -aFq '<--->' "$temporary/options-output" ||
   ! grep -aFq 'c..' "$temporary/options-output"; then
    printf 'editor options or whitespace markers were not rendered\n' >&2
    exit 1
fi

options_config="$temporary/options-config/mcpp/config"
for expected in \
    'editor_tab_width=8' \
    'editor_indent_width=2' \
    'editor_auto_indent=true' \
    'editor_show_whitespace=true'; do
    if ! grep -Fq "$expected" "$options_config"; then
        printf 'saved editor option missing: %s\n' "$expected" >&2
        exit 1
    fi
done

printf 'buffer one\n' >"$temporary/buffer-one.txt"
mkfifo "$temporary/buffer-input"
printf -v buffer_command 'XDG_CONFIG_HOME=%q %q -e %q' \
    "$temporary/buffer-config" \
    "$binary" "$temporary/buffer-one.txt"
"$timeout_command" 15s "$script_command" \
    -qefc "$buffer_command" /dev/null \
    <"$temporary/buffer-input" \
    >"$temporary/buffer-output" 2>"$temporary/buffer-error" &
buffer_runner=$!
exec 6>"$temporary/buffer-input"
sleep 1
printf '\016\rbuffer-two\033[12~\r' >&6
sleep 0.5
printf '\033[20~\033[C\033[C\033[C\033[C\033[C\r\r' >&6
sleep 0.5
printf '\033[20~\033[C\033[C\033[C\033[C\033[C\r\033[B\033[B\r' >&6
sleep 0.5
printf '\033' >&6
sleep 0.3
printf '\033[21~' >&6
set +e
wait "$buffer_runner"
buffer_status=$?
set -e
exec 6>&-

if ((buffer_status != 0)); then
    printf 'editor multi-buffer workflow failed with status %d\n' \
        "$buffer_status" >&2
    cat "$temporary/buffer-error" >&2
    exit 1
fi

if [[ "$(cat "$temporary/new-file")" != "buffer-two" ]]; then
    printf 'editor new buffer did not save independently\n' >&2
    exit 1
fi

if ! grep -aFq 'Buffer list' "$temporary/buffer-output" ||
   ! grep -aFq 'buffer-one.txt' "$temporary/buffer-output" ||
   ! grep -aFq 'new-file' "$temporary/buffer-output"; then
    printf 'editor buffer list did not show both buffers\n' >&2
    exit 1
fi
