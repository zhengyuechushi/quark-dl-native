#!/usr/bin/env bash
# Builds quark-dl.elf in Docker. Run on a host with Docker, or let
# .github/workflows/build-elf.yml do it on a runner.
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ "${1:-}" != --inside ]]; then
  docker build -t quark-dl-native ps5
  exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
    -e BUILD_ID="${BUILD_ID:-local}" \
    -v "$PWD:/work" quark-dl-native bash ps5/build.sh --inside
fi

export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
export BUILD_ID="${BUILD_ID:-local}"

make BUILD_ID="$BUILD_ID"
