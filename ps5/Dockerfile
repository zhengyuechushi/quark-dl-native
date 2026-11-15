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
    ca-certificates make curl unzip zip python3 pkg-config clang llvm lld \
    file binutils zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

ENV PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk

ARG SDK_URL=https://github.com/ps5-payload-dev/sdk/releases/download/v0.43/ps5-payload-sdk.zip
ARG SDK_SHA256=a9cc9929f21b2b2c5d5b309f3bab4997067c45281c0622cf4838b1aecba66fcb
RUN curl -fsSL --proto =https --retry 3 "$SDK_URL" -o /tmp/sdk.zip \
    && echo "$SDK_SHA256  /tmp/sdk.zip" | sha256sum -c - \
    && unzip -q /tmp/sdk.zip -d /opt && rm /tmp/sdk.zip \
    && test -x "$PS5_PAYLOAD_SDK/bin/prospero-clang"

# prospero-clang -> bin/clang -> prospero-llvm -> $(llvm-config --bindir)/clang.
# Without llvm-config that chain dead-ends with a bare exit 1 and no message, which is
# what "prospero-clang cannot link a trivial program" was reporting.
RUN printf 'int main(void){return 0;}\n' > /tmp/t.c \
    && llvm-config --version \
    && clang --version | head -1 \
    && "$PS5_PAYLOAD_SDK/bin/prospero-clang" /tmp/t.c -o /tmp/t \
    && file /tmp/t

COPY install-libmicrohttpd.sh /tmp/install-libmicrohttpd.sh
RUN bash /tmp/install-libmicrohttpd.sh \
    && "$PS5_PAYLOAD_SDK/bin/prospero-pkg-config" --exists libmicrohttpd \
    && "$PS5_PAYLOAD_SDK/bin/prospero-pkg-config" --cflags --libs libmicrohttpd

# TLS for the console. The payload carries its own HTTPS stack instead of calling
# libSceSsl, because a payload cannot know in advance which system modules its
# process has loaded. mbedTLS is built with its default configuration against the
# same prospero-clang, which keeps the dependency to one static library.
COPY install-mbedtls.sh /tmp/install-mbedtls.sh
RUN bash /tmp/install-mbedtls.sh \
    && ls -l /opt/mbedtls/lib \
    && test -f /opt/mbedtls/include/mbedtls/ssl.h

WORKDIR /work
