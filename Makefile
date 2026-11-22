# ps5-mcp - MCP server payload for the PS5.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   make                 build/ps5-mcp.elf with the PS5 payload SDK
#   make host            build/ps5-mcp-host, the same server on a mock backend
#   make test            build the host server and run the end-to-end tests
#   make deploy          send the payload to the console's ELF loader
#
# Variables: PS5_PAYLOAD_SDK (default /opt/ps5-payload-sdk), PS5_HOST,
# PS5_PORT (ELF loader port, default 9021), PORT (server port, default 7165),
# VERSION (default: the nearest vX.Y.Z git tag, else 0.0.0-dev).

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
PS5_HOST ?= ps5
PS5_PORT ?= 9021
PORT ?= 7165
# Release builds pass VERSION (semantic-release); otherwise the nearest git tag.
VERSION ?= $(shell git describe --tags --match 'v[0-9]*' 2>/dev/null | sed 's/^v//' || true)
ifeq ($(strip $(VERSION)),)
VERSION := 0.0.0-dev
endif

BUILD := build
CORE := src/util.c src/http.c src/mcp.c src/klog.c src/pad_klog.c src/pad.c src/image.c src/screen.c \
        src/tools_pad.c src/tools_system.c src/tools_process.c src/tools_klog.c \
        src/tools_fs.c src/tools_app.c src/tools_elf.c src/main.c third_party/cjson/cJSON.c
HEADERS := $(wildcard src/*.h)

WARN := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
DEFS := -DPS5MCP_DEFAULT_PORT=$(PORT) -DPS5MCP_VERSION=\"$(VERSION)\" -D_GNU_SOURCE

ELF := $(BUILD)/ps5-mcp.elf
HOST := $(BUILD)/ps5-mcp-host

all: $(ELF)

$(BUILD):
	mkdir -p $(BUILD)

# ---- PS5 payload --------------------------------------------------------
$(ELF): $(CORE) src/backend_ps5.c $(HEADERS) | $(BUILD)
	@test -x $(PS5_PAYLOAD_SDK)/bin/prospero-clang || \
	  { echo "no PS5 payload SDK at $(PS5_PAYLOAD_SDK): set PS5_PAYLOAD_SDK"; exit 1; }
	$(PS5_PAYLOAD_SDK)/bin/prospero-clang -std=gnu11 -O2 -g $(WARN) -Wno-unreachable-code-generic-assoc -Werror $(DEFS) -DPS5MCP_PS5 \
	  -o $@ $(CORE) src/backend_ps5.c \
	  -lkernel_sys -lScePad -lSceUserService -lSceSystemService -lpthread

# ---- host mock ----------------------------------------------------------
$(HOST): $(CORE) src/backend_host.c $(HEADERS) | $(BUILD)
	$(CC) -std=gnu11 -O1 -g $(WARN) $(DEFS) -fsanitize=address,undefined \
	  -o $@ $(CORE) src/backend_host.c -lpthread -lm

host: $(HOST)

test: $(HOST)
	python3 -I tests/test_mcp.py --binary $(HOST)

deploy: $(ELF)
	$(PS5_PAYLOAD_SDK)/bin/prospero-deploy -h $(PS5_HOST) -p $(PS5_PORT) $(ELF)

clean:
	rm -rf $(BUILD)

.PHONY: all host test deploy clean
