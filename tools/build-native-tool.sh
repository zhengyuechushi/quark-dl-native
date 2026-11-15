#!/usr/bin/env bash
# 构建 host 端的 ps5-native-tool：把 PIE 变成 PS5 标题布局并签名成 eboot.bin。
#
# 源码来自 ps5-native-app-boilerplate（BlackBearReloaded, GPL-3.0-or-later）的
# tooling/native/，见同目录 ORIGIN.md。这里只编转换器，不编它那套应用运行时
# （libc_builder.cpp 不在其中：libc.prx 用的是既有产物，见 starter/sce_module）。
#
# 除 ps5-native-tool 外不安装任何东西：产物落在 starter/build/host/。
# 依赖：C++20 编译器（clang++ 或 g++）+ zlib 开发包。
#
# 用法：tools/build-native-tool.sh [输出路径]
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=${1:-starter/build/host/ps5-native-tool}
CXX=${CXX:-$(command -v clang++ || command -v g++ || true)}
[[ -n $CXX ]] || { echo "no C++ compiler (clang++ / g++)" >&2; exit 2; }

NATIVE=starter/tooling/native
for f in native_app_builder.cpp self_container.cpp elf_object.cpp sce_module_writer.cpp; do
  [[ -f $NATIVE/$f ]] || { echo "missing $NATIVE/$f" >&2; exit 2; }
done

mkdir -p "$(dirname "$OUT")"

# -Werror 故意不加：这个工具只在构建期跑，高版本编译器的新警告不该拖住建包。
# 真出错（缺失符号、类型不符）依然会硬失败。
"$CXX" -std=c++20 -O2 -Wall -Wextra \
  "$NATIVE/native_app_builder.cpp" "$NATIVE/self_container.cpp" \
  "$NATIVE/elf_object.cpp" "$NATIVE/sce_module_writer.cpp" \
  -lz -o "$OUT"

"$OUT" --help >/dev/null 2>&1 || true
echo "built $OUT"
