CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
PKGS     = gio-unix-2.0 gio-2.0
GTK_CFLAGS := $(shell pkg-config --cflags $(PKGS))
GTK_LIBS   := $(shell pkg-config --libs $(PKGS))

all: winvm-viewer qemu-dbus-listener

winvm-viewer: winvm-viewer.c
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -o $@ $< -lX11 -lXext $(GTK_LIBS)

qemu-dbus-listener: qemu-dbus-listener.c
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -o $@ $< $(GTK_LIBS)

clean:
	rm -f winvm-viewer qemu-dbus-listener

install: winvm-viewer
	install -D -m 0755 winvm-viewer $(DESTDIR)/usr/local/bin/winvm-viewer

.PHONY: all clean install
