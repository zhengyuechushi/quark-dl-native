# Toolchain image for the C payload: ps5-payload SDK (prospero-clang) plus a
# libmicrohttpd cross-built for it. Same SDK release and checksum as the build
# that was verified on hardware; the HTTP layer is the reference payload's, so
# the library has to be built the same way it builds there.
#
# libmicrohttpd is installed while the image is built (as root), because the
# container is later run as the host user and cannot write into /opt.
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates make curl unzip python3 gcc libc6-dev \
    && rm -rf /var/lib/apt/lists/*

ENV PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk

ARG SDK_URL=https://github.com/ps5-payload-dev/sdk/releases/download/v0.43/ps5-payload-sdk.zip
ARG SDK_SHA256=a9cc9929f21b2b2c5d5b309f3bab4997067c45281c0622cf4838b1aecba66fcb
RUN curl -fsSL --proto =https --retry 3 "$SDK_URL" -o /tmp/sdk.zip \
    && echo "$SDK_SHA256  /tmp/sdk.zip" | sha256sum -c - \
    && unzip -q /tmp/sdk.zip -d /opt && rm /tmp/sdk.zip \
    && test -x "$PS5_PAYLOAD_SDK/bin/prospero-clang"

COPY install-libmicrohttpd.sh /tmp/install-libmicrohttpd.sh
RUN bash /tmp/install-libmicrohttpd.sh \
    && "$PS5_PAYLOAD_SDK/bin/prospero-pkg-config" --exists libmicrohttpd

WORKDIR /work
