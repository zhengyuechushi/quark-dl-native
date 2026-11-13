# Toolchain image for the C payload: ps5-payload SDK (prospero-clang) plus a
# libmicrohttpd cross-built for it.
#
# ps5-payload-sdk ships wrappers, not a compiler: prospero-clang drives the host's
# clang with --target=x86_64-sie-ps5, and prospero-pkg-config drives the host's
# pkg-config. Without those two installed, configure reports "C compiler cannot
# create executables" (clang missing) or prospero-pkg-config dies on exec.
#
# Debian trixie matches the image the Rust build already used, so the clang
# version is one this SDK is known to work with.
FROM debian:trixie-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates make curl unzip python3 pkg-config clang lld \
    file binutils \
    && rm -rf /var/lib/apt/lists/*

ENV PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk

ARG SDK_URL=https://github.com/ps5-payload-dev/sdk/releases/download/v0.43/ps5-payload-sdk.zip
ARG SDK_SHA256=a9cc9929f21b2b2c5d5b309f3bab4997067c45281c0622cf4838b1aecba66fcb
RUN curl -fsSL --proto =https --retry 3 "$SDK_URL" -o /tmp/sdk.zip \
    && echo "$SDK_SHA256  /tmp/sdk.zip" | sha256sum -c - \
    && unzip -q /tmp/sdk.zip -d /opt && rm /tmp/sdk.zip \
    && test -x "$PS5_PAYLOAD_SDK/bin/prospero-clang"

# Fail here, with the compiler's own message, rather than inside configure.
RUN printf 'int main(void){return 0;}\n' > /tmp/t.c \
    && "$PS5_PAYLOAD_SDK/bin/prospero-clang" /tmp/t.c -o /tmp/t || \
       (echo "prospero-clang cannot link a trivial program" >&2; exit 1)

COPY install-libmicrohttpd.sh /tmp/install-libmicrohttpd.sh
RUN bash /tmp/install-libmicrohttpd.sh \
    && "$PS5_PAYLOAD_SDK/bin/prospero-pkg-config" --exists libmicrohttpd \
    && "$PS5_PAYLOAD_SDK/bin/prospero-pkg-config" --cflags --libs libmicrohttpd

WORKDIR /work
