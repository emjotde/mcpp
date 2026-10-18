#!/usr/bin/env bash
set -euo pipefail

binary="$1"
script_command="$2"
timeout_command="$3"
temporary="$PWD/mcpp-terminal-test-$BASHPID"
mkdir -m 700 -- "$temporary"
trap 'rm -rf -- "$temporary"' EXIT
mkdir -p "$temporary/left" "$temporary/right"
printf 'alpha\n' >"$temporary/left/a.txt"
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
printf '\033[12~\033\033[20~\033[C\033[B\033' >&3
sleep 1
printf '\033[B\033[13~' >&3
sleep 1
printf '\033[18~missing-value\r' >&3
sleep 1
printf '\r\033[13~\033[14~\033[FZ\033[12~\r\033[21~' >&3
sleep 1
printf '\033[18~created\r' >&3
sleep 1
printf '\033[15~\r' >&3
sleep 1
printf 'touch cmd-created.txt\r' >&3
sleep 1
printf 'x\177\017' >&3
sleep 1
printf 'printf "C_O_SHELL\\n"\rexit\r' >&3
sleep 1
printf ' \t\033[21~' >&3
set +e
wait "$runner"
status=$?
set -e
exec 3>&-

if ((status != 0)); then
    printf 'mcpp terminal smoke test failed with status %d\n' \
        "$status" >&2
    cat "$temporary/error" >&2
    exit 1
fi

tr -d '\000' <"$temporary/output" >"$temporary/output-clean"
if ! grep -aFq $'\033[?1049l\r\033[0m\r' \
    "$temporary/output-clean"; then
    printf 'mcpp did not restore a clean terminal line on exit\n' >&2
    exit 1
fi

if ! grep -aFq 'UnWrap' "$temporary/output-clean" ||
   ! grep -aFq 'Goto' "$temporary/output-clean" ||
   ! grep -aFq 'Search' "$temporary/output-clean" ||
   ! grep -aFq 'Raw' "$temporary/output-clean" ||
   ! grep -aFq 'Format' "$temporary/output-clean"; then
    printf 'mcpp viewer did not render the MC-style keybar\n' >&2
    exit 1
fi

if ! grep -aFq 'Search string not found' \
    "$temporary/output-clean"; then
    printf 'mcpp viewer did not report a failed search\n' >&2
    exit 1
fi

if [[ ! -d "$temporary/left/created" ]]; then
    printf 'mcpp F7 did not create the requested directory\n' >&2
    exit 1
fi

if [[ "$(cat "$temporary/right/a.txt")" != "alphaZ" ]]; then
    printf 'mcpp F5 did not copy the selected file\n' >&2
    exit 1
fi

if [[ ! -f "$temporary/left/cmd-created.txt" ]]; then
    printf 'mcpp command line did not run in the active directory\n' >&2
    exit 1
fi

if ! grep -aFq 'C_O_SHELL' "$temporary/output-clean"; then
    printf 'mcpp Ctrl-O did not open an interactive shell\n' >&2
    exit 1
fi

mkfifo "$temporary/default-input"
printf -v default_command 'cd %q && XDG_CONFIG_HOME=%q %q --printwd %q' \
    "$temporary/left" "$temporary/default-config" "$binary" \
    "$temporary/default-cwd"
"$timeout_command" 10s "$script_command" \
    -qefc "$default_command" /dev/null \
    <"$temporary/default-input" \
    >"$temporary/default-output" 2>"$temporary/default-error" &
default_runner=$!
exec 4>"$temporary/default-input"
sleep 1
printf '\033[21~' >&4
set +e
wait "$default_runner"
default_status=$?
set -e
exec 4>&-

if ((default_status != 0)); then
    printf 'mcpp default-pane test failed with status %d\n' \
        "$default_status" >&2
    exit 1
fi

if [[ ! -f "$temporary/default-cwd" ]] ||
   [[ "$(cat "$temporary/default-cwd")" != "$temporary/left" ]]; then
    printf 'mcpp without paths did not open cwd\n' >&2
    exit 1
fi
