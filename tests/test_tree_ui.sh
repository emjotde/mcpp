#!/usr/bin/env bash
set -euo pipefail

binary="$1"
script_command="$2"
timeout_command="$3"
temporary="$(mktemp -d /tmp/mcpp-tree-ui.XXXXXX)"
trap 'rm -rf -- "$temporary"' EXIT
mkdir -p \
    "$temporary/left/alpha/child" \
    "$temporary/left/beta" \
    "$temporary/right"
mkfifo "$temporary/input"

printf -v command 'XDG_CONFIG_HOME=%q %q %q %q' \
    "$temporary/config" \
    "$binary" "$temporary/left" "$temporary/right"
"$timeout_command" 12s "$script_command" \
    -qefc "$command" /dev/null \
    <"$temporary/input" \
    >"$temporary/output" 2>"$temporary/error" &
runner=$!

exec 3>"$temporary/input"
sleep 1
printf '\033[20~\r\033[B\033[B\033[B\r' >&3
sleep 0.5
printf '\t\033[C' >&3
sleep 0.5
printf '\033[C' >&3
sleep 0.5
printf '\033[D' >&3
sleep 0.5
printf '\r' >&3
sleep 0.5
printf '\033[20~\r\033[A\033[A\033[A\r' >&3
sleep 0.5
printf '\033[21~' >&3

set +e
wait "$runner"
status=$?
set -e
exec 3>&-

if ((status != 0)); then
    printf 'tree UI test failed with status %d\n' "$status" >&2
    cat "$temporary/error" >&2
    exit 1
fi

for expected in \
    'Directory tree' \
    'Rescan' \
    'Dynamc'; do
    if ! grep -aFq "$expected" "$temporary/output"; then
        printf 'tree UI did not render: %s\n' "$expected" >&2
        exit 1
    fi
done
