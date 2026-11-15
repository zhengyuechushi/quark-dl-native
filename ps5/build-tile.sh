#!/usr/bin/env bash
# 把已构建的 payload ELF 包成主屏幕磁贴（原生 PS5 标题）+ 可直接放到
# /data/homebrew 的目录 + 一个 zip。
#
# 产物（在仓库根）：
#   dist/QDLK85200/            eboot.bin + sce_module/ + sce_sys/
#   quark-dl-tile_v<版本>.zip  上面目录的压缩包，解压即得 QDLK85200/
#
# 用法：
#   本机（有 Docker）：bash ps5/build-tile.sh
#   CI / 容器内：      bash ps5/build-tile.sh --inside
#
# 前置：payload ELF 已构建（make all），因为磁贴把 payload.elf 内嵌进 eboot.bin。
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ "${1:-}" != --inside ]]; then
  # 与 ps5/build.sh 共用同一个镜像：payload 和磁贴必须用同一套工具链产出的
  # ELF，否则磁贴里嵌的是另一次构建的东西。
  docker build -t quark-dl-native -f ps5/Dockerfile .
  exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
    -e BUILD_ID="${BUILD_ID:-local}" \
    -v "$PWD:/work" quark-dl-native bash ps5/build-tile.sh --inside
fi

export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"

VERSION=$(sed -n 's/^APP_VERSION := //p' Makefile | tr -d ' ')
TITLE_ID=QDLK85200
PAYLOAD="quark-dl_v${VERSION}.elf"
DIST="dist/$TITLE_ID"

[[ -f $PAYLOAD ]] || {
  echo "payload missing: $PAYLOAD - build it first (make BUILD_ID=...)" >&2
  exit 2
}

# CRLF 会让 bash 报 "$'\r': command not found"：仓库在 Windows 上检出过。
sed -i 's/\r$//' tools/build-native-tool.sh starter/build_title.sh

echo "==> ps5-native-tool（host 端转换器）"
bash tools/build-native-tool.sh starter/build/host/ps5-native-tool

# 磁贴和它拉起的程序必须是同一版：payload.elf 就是刚刚构建的那份 ELF，
# .incbin 会把它整块贴进 eboot.bin。
echo "==> starter/payload.elf <- $PAYLOAD"
cp "$PAYLOAD" starter/payload.elf

echo "==> eboot.bin（签名原生标题）"
PS5_NATIVE_TOOL="$PWD/starter/build/host/ps5-native-tool" \
  bash starter/build_title.sh

echo "==> 组装 $DIST"
rm -rf "$DIST"
mkdir -p "$DIST/sce_sys" "$DIST/sce_module"
cp starter/eboot.bin "$DIST/eboot.bin"
cp starter/sce_module/libc.prx "$DIST/sce_module/"
cp starter/sce_module/NOTICE.txt "$DIST/sce_module/"
cp starter/sce_sys/param.json starter/sce_sys/icon0.png "$DIST/sce_sys/"

# 加载器要求 sce_module/libc.prx 在场（缺了报 PRX_SCE_MODULE_LOAD_ERROR
# 0xa0020102），所以这一项不能省；其余两个目录一个都不能多。
for need in eboot.bin sce_module/libc.prx sce_sys/param.json sce_sys/icon0.png; do
  [[ -s $DIST/$need ]] || { echo "missing $DIST/$need" >&2; exit 1; }
done

ZIP="quark-dl-tile_v${VERSION}.zip"
rm -f "$ZIP"
(cd dist && zip -qr "../$ZIP" "$TITLE_ID")

echo
echo "--- 产物 ---"
ls -l "$DIST/eboot.bin" "$ZIP"
echo "解压 $ZIP 得到 $TITLE_ID/，整个目录放到 PS5 的 /data/homebrew/ 下。"
