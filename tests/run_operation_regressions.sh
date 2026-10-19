#!/usr/bin/env bash
set -euo pipefail

binary="${1:?usage: run_operation_regressions.sh mcpp mcpp_tests evidence-directory [UI case ...]}"
unit_binary="${2:?missing mcpp_tests}"
evidence="${3:?missing evidence-directory}"
shift 3
[[ -x "$binary" && -x "$unit_binary" ]] || {
    printf 'Pass already-built binaries; this runner never builds.\n' >&2
    exit 2
}
image="${MCPP_TEST_IMAGE:-mcpp-test:${USER:-local}}"
docker image inspect "$image" >/dev/null
mkdir -p "$evidence"
evidence="$(cd "$evidence" && pwd)"
tests="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
container=
cleanup() {
    local status=$?
    trap - EXIT
    if [[ -n "$container" ]]; then
        local copy_status=0
        docker cp "$container:/review/evidence/." "$evidence/" || copy_status=$?
        if ((copy_status != 0)); then
            printf 'Evidence export failed (docker cp exit %d; original exit %d).\n' \
                "$copy_status" "$status" >&2
            printf 'Owned container %s retained for recovery.\n' "$container" >&2
            printf 'Recover with: docker cp %q %q\n' \
                "$container:/review/evidence/." "$evidence/" >&2
            if ((status == 0)); then
                status=$copy_status
            fi
            exit "$status"
        fi
        local remove_status=0
        docker rm -f "$container" >/dev/null || remove_status=$?
        if ((remove_status != 0)); then
            printf 'Could not remove owned container %s (exit %d).\n' \
                "$container" "$remove_status" >&2
            if ((status == 0)); then
                status=$remove_status
            fi
        fi
    fi
    exit "$status"
}
trap cleanup EXIT
container="$(docker create --pull=never \
    --init \
    --name "mcpp-operation-regressions-${UID}-$$" \
    --network none --cpus 1 --memory 512m --pids-limit 96 \
    -e ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    -e UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    --cap-drop ALL --security-opt no-new-privileges \
    --tmpfs /review/full:rw,size=8m,mode=0777 \
    --tmpfs /review/crossfs:rw,size=32m,mode=0777 \
    "$image" sleep infinity)"
[[ "$(docker inspect --format '{{json .Mounts}}' "$container")" == '[]' ]]
docker inspect --format \
    '{"id":"{{.Id}}","image":"{{.Image}}","user":"{{.Config.User}}","network":"{{.HostConfig.NetworkMode}}","mounts":{{json .Mounts}},"tmpfs":{{json .HostConfig.Tmpfs}}}' \
    "$container" >"$evidence/container.json"
docker start "$container" >/dev/null
docker exec --user 0 "$container" sh -c \
    'mkdir -p /review/bin /review/tests /review/runtime /review/cases /review/evidence /review/homes;
     chmod 777 /review/runtime /review/cases /review/evidence /review/homes;
     touch /review/.operation-regressions-disposable'
docker cp "$binary" "$container:/review/bin/mcpp"
docker cp "$unit_binary" "$container:/review/bin/mcpp_tests"
docker cp "$tests/test_operation_regressions.sh" \
    "$container:/review/tests/test_operation_regressions.sh"
docker cp "$tests/test_operation_ui.sh" \
    "$container:/review/tests/test_operation_ui.sh"
docker cp "$tests/test_main.cpp" "$container:/review/tests/test_main.cpp"
docker exec "$container" sha256sum /review/bin/mcpp /review/bin/mcpp_tests \
    >"$evidence/binaries.sha256"
docker exec "$container" mkdir -p /review/homes/unit/{config,cache,data}
docker exec \
    -e TMPDIR=/review/runtime \
    -e MCPP_DISPOSABLE_TESTS=1 \
    -e HOME=/review/homes/unit \
    -e XDG_CONFIG_HOME=/review/homes/unit/config \
    -e XDG_CACHE_HOME=/review/homes/unit/cache \
    -e XDG_DATA_HOME=/review/homes/unit/data \
    "$container" /review/bin/mcpp_tests 2>&1 | tee "$evidence/unit.log"
for group in 'operation destinations' 'atomic symbolic links' 'operation results'; do
    if ! grep -Fxq "[PASS] $group" "$evidence/unit.log"; then
        printf 'Missing regression group %s; rebuild mcpp_tests and use this runner.\n' \
            "$group" >&2
        exit 1
    fi
done
docker exec -e TMPDIR=/review/runtime "$container" \
    bash /review/tests/test_operation_ui.sh \
    /review/bin/mcpp /usr/bin/script /usr/bin/timeout \
    2>&1 | tee "$evidence/pause-skip.log"
docker exec \
    -e TMPDIR=/review/runtime \
    -e MCPP_DISPOSABLE_TESTS=1 \
    -e MCPP_UI_GROUP=foreground \
    -e MCPP_BG_BYTES="${MCPP_BG_BYTES:-1073741824}" \
    "$container" bash /review/tests/test_operation_regressions.sh \
    /review/bin/mcpp "$@" 2>&1 | tee "$evidence/ui.log"
docker update --cpus 0.1 "$container" >/dev/null
docker exec \
    -e TMPDIR=/review/runtime \
    -e MCPP_DISPOSABLE_TESTS=1 \
    -e MCPP_UI_GROUP=background \
    -e MCPP_BG_BYTES="${MCPP_BG_BYTES:-1073741824}" \
    "$container" bash /review/tests/test_operation_regressions.sh \
    /review/bin/mcpp "$@" 2>&1 | tee -a "$evidence/ui.log"
