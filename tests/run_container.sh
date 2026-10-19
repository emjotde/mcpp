#!/usr/bin/env bash
set -euo pipefail

repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image="mcpp-test:${USER:-local}"

docker build \
    --file "$repository/docker/Dockerfile.test" \
    --tag "$image" \
    "$repository"

docker run \
    --rm \
    --init \
    --network none \
    --cap-drop ALL \
    --security-opt no-new-privileges \
    "$image"
