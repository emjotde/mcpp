#!/usr/bin/env bash
set -euo pipefail

binary="$1"
script_command="$2"
timeout_command="$3"
temporary="$(mktemp -d /tmp/mcpp-shortcut-test.XXXXXX)"
trap 'rm -rf -- "$temporary"' EXIT
mkdir -p "$temporary/left" "$temporary/right"
for ((index = 1; index <= 40; ++index)); do
    printf 'line%03d-abcdefghijklmnopqrstuvwxyz-ABCDEFGHIJKLMNOPQRSTUVWXYZ\n' \
        "$index"
done >"$temporary/left/a.txt"
chmod 0640 "$temporary/left/a.txt"
printf 'extra\n' >"$temporary/left/d.txt"
printf 'right\n' >"$temporary/right/b.txt"
printf 'second\n' >"$temporary/right/c.txt"
mkfifo "$temporary/input"

printf -v command 'XDG_CONFIG_HOME=%q %q %q %q' \
    "$temporary/config" \
    "$binary" "$temporary/left" "$temporary/right"
"$timeout_command" 30s "$script_command" \
    -qefc "$command" /dev/null \
    <"$temporary/input" \
    >"$temporary/output" 2>"$temporary/error" &
runner=$!

exec 3>"$temporary/input"
sleep 1
printf '\033[B\030q' >&3
sleep 0.4
printf '\t\033[6~' >&3
sleep 0.4
printf '\033[14~' >&3
sleep 0.4
printf '\033[14~\033[12~' >&3
sleep 0.4
printf '\033[12~' >&3
sleep 0.4
printf '\033[20~\r\033[A\r' >&3
sleep 0.4
printf '\030q\t\t' >&3
sleep 0.4
printf '\030q\t\030i' >&3
sleep 0.4
printf '\030i' >&3
sleep 0.4
printf '\030c' >&3
sleep 0.4
printf '\033' >&3
sleep 0.4
printf '\030l' >&3
sleep 0.4
printf '\033' >&3
sleep 0.4
printf '\030s' >&3
sleep 0.4
printf '\033' >&3
sleep 0.4
printf '\030v' >&3
sleep 0.4
printf '\033' >&3
sleep 0.4
printf '\030o' >&3
sleep 0.4
printf '\033' >&3
sleep 0.4
printf '\030d' >&3
sleep 0.4
printf '\034' >&3
sleep 0.4
printf '\033' >&3
sleep 0.4
printf '\033?' >&3
sleep 0.4
printf '\033' >&3
sleep 0.4
printf 'x\177+' >&3
sleep 0.4
printf '\r' >&3
sleep 0.4
printf '\033[19~' >&3
sleep 0.4
printf '\033' >&3
sleep 0.4
printf '-' >&3
sleep 0.4
printf '\r' >&3
sleep 0.4
printf '\033[19~' >&3
sleep 0.4
printf '\033' >&3
sleep 0.4
printf '*\033[19~' >&3
sleep 0.4
printf '\033' >&3
sleep 0.4
printf '\t\033[B\033[20~' >&3
sleep 0.4
printf '\033[B\033[B\r' >&3
sleep 0.4
printf '\t\t\033[B' >&3
sleep 0.4
printf '\033[20~\033[D\r\r' >&3
sleep 0.4
printf '\t\030q' >&3
sleep 0.4
printf '\030q' >&3
sleep 0.4
printf '\033[21~' >&3

set +e
wait "$runner"
status=$?
set -e
exec 3>&-

if ((status != 0)); then
    printf 'mcpp shortcut test failed with status %d\n' "$status" >&2
    cat "$temporary/error" >&2
    exit 1
fi

for expected in \
    'Quick view: a.txt' \
    'Quick view: b.txt' \
    'line009-' \
    'Ascii' \
    'HxSrch' \
    'Filesystem:' \
    'Octal permissions' \
    '> 0640' \
    'Hard link name' \
    'Symbolic link name' \
    'Relative symbolic link name' \
    'Owner:group' \
    'difference(s)' \
    'Directory hotlist' \
    'Recursive name pattern' \
    'Select group' \
    'Unselect group' \
    'Delete 2 selected item(s)?' \
    'Delete 1 selected item(s)?'; do
    if ! grep -aFq "$expected" "$temporary/output"; then
        printf 'shortcut did not open: %s\n' "$expected" >&2
        exit 1
    fi
done

if grep -aFq 'Quick view: c.txt' "$temporary/output"; then
    printf 'F9 opened the panel menu for the inactive pane\n' >&2
    exit 1
fi
