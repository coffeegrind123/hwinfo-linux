CC ?= gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE
LDFLAGS =
PREFIX ?= /usr/local

SRCDIR = src

# Native Linux collector
NATIVE_SRCS = $(SRCDIR)/main.c \
              $(SRCDIR)/json_builder.c \
              $(SRCDIR)/sysfs_utils.c \
              $(SRCDIR)/collect_memory.c \
              $(SRCDIR)/collect_cpu.c \
              $(SRCDIR)/collect_board.c \
              $(SRCDIR)/collect_storage.c \
              $(SRCDIR)/collect_gpu.c \
              $(SRCDIR)/collect_network.c \
              $(SRCDIR)/collect_errors.c
NATIVE_OBJS = $(NATIVE_SRCS:.c=.o)
NATIVE_TARGET = hwinfo-linux

# RemoteHWInfo bridge (Windows HWiNFO -> JSON)
BRIDGE_SRCS = $(SRCDIR)/bridge.c \
              $(SRCDIR)/json_builder.c
BRIDGE_OBJS = $(BRIDGE_SRCS:.c=.o)
BRIDGE_TARGET = hwinfo-bridge

all: $(NATIVE_TARGET) $(BRIDGE_TARGET)

$(NATIVE_TARGET): $(NATIVE_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BRIDGE_TARGET): $(SRCDIR)/bridge.o $(SRCDIR)/json_builder.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(NATIVE_OBJS) $(SRCDIR)/bridge.o $(NATIVE_TARGET) $(BRIDGE_TARGET)

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(NATIVE_TARGET) $(DESTDIR)$(PREFIX)/bin/
	install -m 755 $(BRIDGE_TARGET) $(DESTDIR)$(PREFIX)/bin/

.PHONY: all clean install
