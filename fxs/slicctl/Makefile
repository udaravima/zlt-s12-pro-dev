# Build: make CC=mipsel-openwrt-linux-musl-gcc   (or plain `make` for a host syntax check)
CC ?= gcc
CFLAGS ?= -O2
override CFLAGS += -std=gnu11 -Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function \
	  -Iarch -Ivpapi/includes -Ivpapi/vp886_api

API_SRC := $(wildcard vpapi/common/*.c) $(wildcard vpapi/vp886_api/*.c)
SRC := slicctl.c cid.c dtmf.c asock.c board_profile.c arch/vp_hal.c arch/sys_service.c $(API_SRC)
# optional: Tozed's own DC-feed/ring/tone set (tools/extract_profiles.py on a
# stock dump), selectable at run time with SLIC_DC=B / SLIC_RING=B
ifneq ($(wildcard stock_profiles.c),)
SRC += stock_profiles.c
override CFLAGS += -DHAVE_STOCK_PROFILES
endif

slicctl: $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS) -lm

clean:
	rm -f slicctl
