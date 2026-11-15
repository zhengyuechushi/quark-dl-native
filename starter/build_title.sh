#!/usr/bin/env bash
# 把 starter.c 编成签名的原生 PS5 标题 eboot.bin（Linux / CI 容器里跑）。
#
# 三步（与 ps5-native-app-boilerplate 的 tools/build.sh 同一套做法）：
#   1. starter.c 用目标 clang 编成 freestanding 目标文件；
#   2. prospero-lld 链接成中间 PIE，只对 SDK 里的公共导入 stub 求符号；
#   3. ps5-native-tool 把 PIE 变成 PS5 标题布局，装成 eboot.elf，签名成 eboot.bin。
#
# 用法：PS5_PAYLOAD_SDK=<sdk> PS5_NATIVE_TOOL=<tool> starter/build_title.sh
# 前置：starter/payload.elf 已存在（由 CI 把刚构建的 quark-dl ELF 复制过来）。
set -euo pipefail

: "${PS5_PAYLOAD_SDK:?set PS5_PAYLOAD_SDK}"
TOOL=${PS5_NATIVE_TOOL:?set PS5_NATIVE_TOOL to the built ps5-native-tool}
CLANG=${PS5_CLANG:-$(command -v clang-18 || command -v clang || true)}
[[ -n $CLANG && -x $CLANG ]] || { echo "clang missing (clang-18 or clang)" >&2; exit 2; }
[[ -x $TOOL ]] || { echo "ps5-native-tool missing: $TOOL" >&2; exit 2; }

# ps5-native-app-boilerplate 在 FW 6.02 / 12.70 上验过的值
MODULE_SDK=0x02000009
COMPANION_SDK=0x08050001
FSELF_MAGIC=0x1D3D154F

cd "$(dirname "$0")"
B=build
mkdir -p "$B"

flags=(-target x86_64-sie-ps5 -fvisibility-nodllstorageclass=default
       -isysroot "$PS5_PAYLOAD_SDK" -isystem "$PS5_PAYLOAD_SDK/target/include"
       -fno-stack-protector -fno-plt -femulated-tls -fPIC
       -ffreestanding -fno-builtin -std=c11 -O2 -Wall -Wextra -Werror -I.
       -funwind-tables -fasynchronous-unwind-tables)   # ps5-native-tool 要 .eh_frame

[[ -f payload.elf ]] || { echo "starter/payload.elf missing (CI copies the built ELF here)" >&2; exit 2; }
"$CLANG" "${flags[@]}" -I. -c title/starter.c -o "$B/starter.o"

"$PS5_PAYLOAD_SDK/bin/prospero-lld" -T title/title.ld --eh-frame-hdr \
    --version-script title/symbols.map -e _start -o "$B/pie.elf" "$B/starter.o" \
    --as-needed "$PS5_PAYLOAD_SDK"/target/lib/*.so

# 只该有这些导入（libkernel + libSceSystemService）。多出来的一律说明链进了
# 运行时不存在的符号，宁可在构建时失败，也不要在主机上黑屏。
allowed="close connect recv sceKernelDebugOutText sceKernelSendNotificationRequest sceKernelUsleep sceSystemServiceLoadExec send setsockopt socket"
imports=$("$PS5_PAYLOAD_SDK/bin/llvm-nm" -D --undefined-only "$B/pie.elf" | awk '{ sub(/@.*/, "", $2); print $2 }' | sort -u)
for sym in $imports; do
    [[ " $allowed " == *" $sym "* ]] || { echo "unexpected import: $sym" >&2; exit 1; }
done
echo "imports: $(echo $imports)"

"$TOOL" link --in "$B/pie.elf" --out "$B/eboot.elf" --stub-dir "$PS5_PAYLOAD_SDK/target/lib" \
    --module-sdk "$MODULE_SDK" --companion-sdk "$COMPANION_SDK" --file-name eboot.elf
"$TOOL" self --sign --in "$B/eboot.elf" --out eboot.bin --magic "$FSELF_MAGIC"
"$TOOL" self --inspect --file eboot.bin
echo "built starter/eboot.bin $(wc -c < eboot.bin) bytes"
