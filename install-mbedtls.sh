#!/usr/bin/env bash
# Build mbedTLS as a static library for whatever CC is given: Prospero in the
# container, the host in CI (which is how the TLS path gets tested before it
# ever reaches a console).
#
# mbedTLS is used with its default configuration on purpose. A trimmed config.h
# is smaller, but every missing define is a compile error inside the library
# that has nothing to do with this program, and the linker drops what is unused
# anyway.
set -euo pipefail

VERSION="${MBEDTLS_VERSION:-2.28.8}"
PREFIX="${MBEDTLS_PREFIX:-/opt/mbedtls}"
CC_BIN="${MBEDTLS_CC:-${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}/bin/prospero-clang}"
AR_BIN="${MBEDTLS_AR:-llvm-ar}"
RANLIB_BIN="${MBEDTLS_RANLIB:-llvm-ranlib}"
WORK="${MBEDTLS_WORK:-/tmp/mbedtls-build}"
URL="https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v${VERSION}.tar.gz"

if [[ -f "${PREFIX}/lib/libmbedtls.a" ]]; then
  echo "mbedTLS already present in ${PREFIX}"
  exit 0
fi

rm -rf "$WORK"
mkdir -p "$WORK"

echo "fetching mbedTLS ${VERSION}"
curl -fsSL --proto '=https' --retry 3 "$URL" -o "$WORK/mbedtls.tar.gz"
tar -xzf "$WORK/mbedtls.tar.gz" -C "$WORK"

SRC="$WORK/mbedtls-${VERSION}"
if [[ ! -d "$SRC/library" ]]; then
  echo "unexpected archive layout: $SRC/library missing" >&2
  exit 1
fi

JOBS="$(nproc 2>/dev/null || echo 4)"
echo "building with $CC_BIN ($JOBS jobs)"
make -C "$SRC" -j"$JOBS" lib \
  CC="$CC_BIN" AR="$AR_BIN" RANLIB="$RANLIB_BIN" ARFLAGS=rcs \
  CFLAGS="-O2 -fPIC -I$SRC/include -I$SRC/library"

# Copy the three archives and the headers instead of running `make install`:
# that target also builds and installs the example programs, which would happen
# with the host compiler and produce host binaries in a cross build.
mkdir -p "$PREFIX/lib" "$PREFIX/include"
cp "$SRC/library/libmbedcrypto.a" "$SRC/library/libmbedx509.a" \
   "$SRC/library/libmbedtls.a" "$PREFIX/lib/"
cp -R "$SRC/include/mbedtls" "$PREFIX/include/"

test -f "$PREFIX/lib/libmbedtls.a"
test -f "$PREFIX/include/mbedtls/ssl.h"
echo "mbedTLS ${VERSION} installed in ${PREFIX}"
