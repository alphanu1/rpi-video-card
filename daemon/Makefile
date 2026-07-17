# Author: Ben Templeman (alphanu1)
# Date:   2026-07-17
# daemon/Makefile -- builds on-device (Buildroot) or any Linux with libdrm
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += $(shell pkg-config --cflags libdrm 2>/dev/null || echo -I/usr/include/libdrm)
LDLIBS  += $(shell pkg-config --libs libdrm 2>/dev/null || echo -ldrm)

all: crtd crt_apply

crtd: crtd.o drm_apply.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

crt_apply: crt_apply.o drm_apply.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c protocol.h drm_apply.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o crtd crt_apply

.PHONY: all clean
