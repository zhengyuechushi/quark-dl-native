#!/usr/bin/env bash
# Builds quark-dl.elf in Docker. Run on a host with Docker, or let
# .github/workflows/build-elf.yml do it on a runner.
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ "${1:-}" != --inside ]]; then
  # The context is the repo root: the Dockerfile copies install-libmicrohttpd.sh from it.
  docker build -t quark-dl-native -f ps5/Dockerfile .
  exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
    -e BUILD_ID="${BUILD_ID:-local}" \
    -e QDL_BUILD_TILE="${QDL_BUILD_TILE:-0}" \
    -v "$PWD:/work" quark-dl-native bash ps5/build.sh --inside
fi

export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
export BUILD_ID="${BUILD_ID:-local}"

make BUILD_ID="$BUILD_ID"

# QDL_BUILD_TILE=1 时顺手把桌面磁贴也构出来。磁贴把刚构建的 ELF 内嵌进
# eboot.bin，所以必须跟在 make 后面、在同一个容器里做，用同一个工具链。
if [[ "${QDL_BUILD_TILE:-0}" == 1 ]]; then
  bash ps5/build-tile.sh --inside
fi
