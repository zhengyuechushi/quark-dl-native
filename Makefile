# Build shape is copied from owendswang/ps5-web-file-manager (GPL-3.0-or-later):
# include prospero.mk, compile a handful of .c files, embed the web assets, strip.
# Keeping the same toolchain invocation is the point — it is known to produce an
# ELF the console loads.
#
# `make linux` builds the same sources against the host so CI can start the server
# and exercise the endpoints, the download engine and the TLS path. Everything
# console-specific is behind __SCE__, which the SDK defines, so the Linux build
# simply skips it.

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk

# Only the PS5 targets need the cross toolchain; linux/clean must work without it.
ifeq ($(MAKECMDGOALS),)
NEED_PS5 := all
else
NEED_PS5 := $(filter-out linux clean,$(MAKECMDGOALS))
endif
ifneq ($(NEED_PS5),)
ifeq ($(wildcard $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk),)
$(error PS5_PAYLOAD_SDK is undefined or does not contain toolchain/prospero.mk)
endif
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

APP_VERSION := 0.10.0
BUILD_ID    ?= dev
PYTHON      ?= python3
STRIP       ?= $(PS5_PAYLOAD_SDK)/bin/prospero-strip
PKG_CONFIG  ?= $(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config
HOST_CC     ?= cc
HOST_PKG_CONFIG ?= pkg-config

# Static mbedTLS, built by install-mbedtls.sh. QDL_TLS is defined only when the
# library is actually there, so a build without it still works and the payload
# reports "未编译 TLS" instead of failing to link.
TLS_PREFIX ?= /opt/mbedtls
TLS_CFLAGS := -I$(TLS_PREFIX)/include -DQDL_TLS
TLS_LIBS   := -L$(TLS_PREFIX)/lib -lmbedtls -lmbedx509 -lmbedcrypto

# The version is part of the file name because that is the only place Payload
# Manager can read it from: for anything imported by hand (USB or upload) it
# takes the version out of the file name with the same regex its own cards use
# ("quark-dl_v0.6.0.elf" -> title "quark dl", version "v0.6.0"). A bare
# "quark-dl.elf" is listed with no version at all, which is what made this build
# the only entry on the console's launch list without one.
BIN       := quark-dl_v$(APP_VERSION).elf
LINUX_BIN := quark-dl-linux
SRCS      := src/main.c src/websrv.c src/api.c src/kern.c src/asset.c src/notify.c \
             src/net.c src/httpc.c src/dl.c src/json.c src/cookie.c src/quark.c \
             src/boot.c
HEADERS   := $(wildcard src/*.h)

# icon0.png is the home-screen tile, served at /favicon.ico but registered by an
# installer. ca.pem is compiled into the binary by gen-ca-module.py instead of
# being published as a static asset.
ASSETS   := $(filter-out assets/icon0.png assets/ca.pem,$(wildcard assets/*))
GEN_SRCS := $(patsubst assets/%,gen/%, $(ASSETS:=.c)) gen/ca_bundle.c

CFLAGS := -Oz -flto=thin -fno-asynchronous-unwind-tables -fno-unwind-tables \
          -Wall -Werror -ffunction-sections -fdata-sections -Isrc \
          -DAPP_VERSION=\"$(APP_VERSION)\" -DBUILD_ID=\"$(BUILD_ID)\"
CFLAGS += `$(PKG_CONFIG) libmicrohttpd --cflags`
CFLAGS += $(TLS_CFLAGS)
LDFLAGS := -Wl,--gc-sections
LDADD   := `$(PKG_CONFIG) libmicrohttpd --libs`
LDADD   += $(TLS_LIBS)

LINUX_CFLAGS := -O1 -g -Wall -Werror -Wno-format-truncation -Isrc \
                -DAPP_VERSION=\"$(APP_VERSION)\" -DBUILD_ID=\"$(BUILD_ID)\"
LINUX_CFLAGS += `$(HOST_PKG_CONFIG) libmicrohttpd --cflags`
LINUX_CFLAGS += $(TLS_CFLAGS)
LINUX_LDADD  := `$(HOST_PKG_CONFIG) libmicrohttpd --libs` -pthread $(TLS_LIBS)

.PHONY: all deps gen linux linux-deps check-tls clean

all: deps $(BIN)

linux: linux-deps $(LINUX_BIN)

deps:
	@$(PKG_CONFIG) --exists libmicrohttpd || ./install-libmicrohttpd.sh
	@test -f $(TLS_PREFIX)/lib/libmbedtls.a || \
	  (echo "mbedTLS not found in $(TLS_PREFIX); run ./install-mbedtls.sh" >&2; exit 1)

linux-deps:
	@$(HOST_PKG_CONFIG) --exists libmicrohttpd || \
	  (echo "libmicrohttpd development package is required for make linux" >&2; exit 1)
	@test -f $(TLS_PREFIX)/lib/libmbedtls.a || \
	  (echo "mbedTLS not found in $(TLS_PREFIX); run MBEDTLS_PREFIX=$(TLS_PREFIX) MBEDTLS_CC=cc ./install-mbedtls.sh" >&2; exit 1)

gen:
	mkdir -p gen

gen/%.c: assets/% gen-asset-module.py | gen
	$(PYTHON) gen-asset-module.py --path $* $< > $@

# The root store is data, not a page: one string literal per PEM line keeps the
# generated file small enough to compile quickly.
gen/ca_bundle.c: assets/ca.pem gen-ca-module.py | gen
	$(PYTHON) gen-ca-module.py $< -o $@

$(BIN): $(SRCS) $(HEADERS) $(GEN_SRCS) Makefile
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(GEN_SRCS) $(LDADD)
	$(STRIP) $@
	@echo "built $(BIN) $$(wc -c < $(BIN)) bytes, build $(BUILD_ID)"

$(LINUX_BIN): $(SRCS) $(HEADERS) $(GEN_SRCS) Makefile
	$(HOST_CC) $(LINUX_CFLAGS) -o $@ $(SRCS) $(GEN_SRCS) $(LINUX_LDADD)

clean:
	rm -rf $(BIN) $(LINUX_BIN) gen
