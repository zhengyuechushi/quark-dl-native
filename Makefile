# Build shape is copied from owendswang/ps5-web-file-manager (GPL-3.0-or-later):
# include prospero.mk, compile a handful of .c files, embed the web assets, strip.
# Keeping the same toolchain invocation is the point of this stage — it is known
# to produce an ELF the console loads.
#
# `make linux` builds the same sources against the host so CI can start the server
# and exercise the endpoints. Everything console-specific is behind __SCE__, which
# the SDK defines, so the Linux build simply skips it.

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

APP_VERSION := 0.1.0
BUILD_ID    ?= dev
PYTHON      ?= python3
STRIP       ?= $(PS5_PAYLOAD_SDK)/bin/prospero-strip
PKG_CONFIG  ?= $(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config
HOST_CC     ?= cc
HOST_PKG_CONFIG ?= pkg-config

BIN       := quark-dl.elf
LINUX_BIN := quark-dl-linux
SRCS      := src/main.c src/websrv.c src/api.c src/kern.c src/asset.c src/notify.c
HEADERS   := $(wildcard src/*.h)

# icon0.png is the home-screen tile. It is served at /favicon.ico from the assets
# below, but the file itself is registered by an installer, not by this build.
ASSETS   := $(filter-out assets/icon0.png,$(wildcard assets/*))
GEN_SRCS := $(patsubst assets/%,gen/%, $(ASSETS:=.c))

CFLAGS := -Oz -flto=thin -fno-asynchronous-unwind-tables -fno-unwind-tables \
          -Wall -Werror -ffunction-sections -fdata-sections -Isrc \
          -DAPP_VERSION=\"$(APP_VERSION)\" -DBUILD_ID=\"$(BUILD_ID)\"
CFLAGS += `$(PKG_CONFIG) libmicrohttpd --cflags`
LDFLAGS := -Wl,--gc-sections
LDADD   := `$(PKG_CONFIG) libmicrohttpd --libs`

LINUX_CFLAGS := -O1 -g -Wall -Werror -Wno-format-truncation -Isrc \
                -DAPP_VERSION=\"$(APP_VERSION)\" -DBUILD_ID=\"$(BUILD_ID)\"
LINUX_CFLAGS += `$(HOST_PKG_CONFIG) libmicrohttpd --cflags`
LINUX_LDADD  := `$(HOST_PKG_CONFIG) libmicrohttpd --libs` -pthread

.PHONY: all deps gen linux linux-deps clean

all: deps $(BIN)

linux: linux-deps $(LINUX_BIN)

deps:
	@$(PKG_CONFIG) --exists libmicrohttpd || ./install-libmicrohttpd.sh

linux-deps:
	@$(HOST_PKG_CONFIG) --exists libmicrohttpd || \
	  (echo "libmicrohttpd development package is required for make linux" >&2; exit 1)

gen:
	mkdir -p gen

gen/%.c: assets/% gen-asset-module.py | gen
	$(PYTHON) gen-asset-module.py --path $* $< > $@

$(BIN): $(SRCS) $(HEADERS) $(GEN_SRCS) Makefile
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDADD)
	$(STRIP) $@
	@echo "built $(BIN) $$(wc -c < $(BIN)) bytes, build $(BUILD_ID)"

$(LINUX_BIN): $(SRCS) $(HEADERS) $(GEN_SRCS) Makefile
	$(HOST_CC) $(LINUX_CFLAGS) -o $@ $(filter %.c,$^) $(LINUX_LDADD)

clean:
	rm -rf $(BIN) $(LINUX_BIN) gen
