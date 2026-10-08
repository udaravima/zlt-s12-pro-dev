# Build: make CC=mipsel-openwrt-linux-musl-gcc   (or plain `make` for a host syntax check)
CC ?= gcc
CFLAGS ?= -O2
override CFLAGS += -std=gnu11 -Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function \
	  -Iarch -Ivpapi/includes -Ivpapi/vp886_api

API_SRC := $(wildcard vpapi/common/*.c) $(wildcard vpapi/vp886_api/*.c)
SRC := slicctl.c stock_profiles.c arch/vp_hal.c arch/sys_service.c $(API_SRC)

slicctl: $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f slicctl
