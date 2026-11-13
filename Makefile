# Build shape is copied from owendswang/ps5-web-file-manager (GPL-3.0-or-later):
# include prospero.mk, compile a handful of .c files, embed the web assets, strip.
# Keeping the same toolchain invocation is the point of this stage — it is known
# to produce an ELF the console loads.

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
ifeq ($(wildcard $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk),)
$(error PS5_PAYLOAD_SDK is undefined or does not contain toolchain/prospero.mk)
endif
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

APP_VERSION := 0.1.0
BUILD_ID    ?= dev
PYTHON      ?= python3
STRIP       ?= $(PS5_PAYLOAD_SDK)/bin/prospero-strip
PKG_CONFIG  ?= $(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config

BIN     := quark-dl.elf
SRCS    := src/main.c src/websrv.c src/api.c src/kern.c src/asset.c src/notify.c
HEADERS := $(wildcard src/*.h)

# icon0.png is the home-screen tile; it is served at /favicon.ico from the assets
# below, but the file itself is registered by the installer, not by this build.
ASSETS   := $(filter-out assets/icon0.png,$(wildcard assets/*))
GEN_SRCS := $(patsubst assets/%,gen/%, $(ASSETS:=.c))

CFLAGS := -Oz -flto=thin -fno-asynchronous-unwind-tables -fno-unwind-tables \
          -Wall -Werror -ffunction-sections -fdata-sections -Isrc \
          -DAPP_VERSION=\"$(APP_VERSION)\" -DBUILD_ID=\"$(BUILD_ID)\"
CFLAGS += `$(PKG_CONFIG) libmicrohttpd --cflags`
LDFLAGS := -Wl,--gc-sections
LDADD   := `$(PKG_CONFIG) libmicrohttpd --libs`

.PHONY: all deps gen clean

all: deps $(BIN)

deps:
	@$(PKG_CONFIG) --exists libmicrohttpd || ./install-libmicrohttpd.sh

gen:
	mkdir -p gen

gen/%.c: assets/% gen-asset-module.py | gen
	$(PYTHON) gen-asset-module.py --path $* $< > $@

$(BIN): $(SRCS) $(HEADERS) $(GEN_SRCS) Makefile
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDADD)
	$(STRIP) $@
	@echo "built $(BIN) $$(wc -c < $(BIN)) bytes, build $(BUILD_ID)"

clean:
	rm -rf $(BIN) gen
