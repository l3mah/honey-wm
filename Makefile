# honey — a tiling Wayland compositor on wlroots 0.20.
#
# The dev toolchain is not in the profile on this Nix host; build inside an
# ephemeral shell:
#
#   nix-shell -p wlroots wayland wayland-protocols wayland-scanner libxkbcommon \
#     pixman libinput libdrm libxcb xcbutilwm pkg-config gcc gnumake --run make
#
# wlroots is the 0.20.1 nix attr (pkg-config name wlroots-0.20).
# xcb + xcbutilwm are needed for the XWayland headers; the Xwayland binary is a
# runtime dependency (nix `xwayland`), started lazily on the first X11 client.

VERSION   = 0.20.15

CC       ?= cc
SCANNER  ?= wayland-scanner
PKGS      = wlroots-0.20 wayland-server xkbcommon pixman-1 libinput libdrm \
            xcb xcb-ewmh xcb-icccm
PKG_CFLAGS = $(shell pkg-config --cflags $(PKGS))
PKG_LIBS   = $(shell pkg-config --libs $(PKGS))

# Distro builds pass CFLAGS on the make command line, which overrides any
# assignment here; required flags live in HONEY_CFLAGS so they survive.
CFLAGS   ?= -g -Og
HONEY_CFLAGS = $(CFLAGS) -std=c11 -D_GNU_SOURCE -DWLR_USE_UNSTABLE \
            -DHONEY_VERSION=\"$(VERSION)\" \
            -Wall -Wextra -Wpedantic -Wno-unused-parameter \
            -Wno-missing-field-initializers \
            -MMD -MP \
            -Isrc -Ibuild $(PKG_CFLAGS)
LDLIBS    = $(PKG_LIBS)

# Server-side headers for protocols wlroots implements but doesn't ship
# generated (its headers #include these by name).
PROTOCOLS = wlr-layer-shell-unstable-v1 xdg-output-unstable-v1
PROTO_HDRS = $(PROTOCOLS:%=build/%-protocol.h)

# Protocols honey implements itself also need the interface code (the symbols
# are private inside wlroots).
IMPL_PROTOCOLS = xdg-output-unstable-v1
IMPL_OBJS = $(IMPL_PROTOCOLS:%=build/%-protocol.o)

SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:src/%.c=build/%.o) $(IMPL_OBJS)

all: honey honeyctl

build/%-protocol.h: protocol/%.xml | build
	$(SCANNER) server-header $< $@

build/%-protocol.c: protocol/%.xml | build
	$(SCANNER) private-code $< $@

build/%-protocol.o: build/%-protocol.c
	$(CC) $(HONEY_CFLAGS) -c $< -o $@

# Generated protocol headers must exist before any object is compiled.
$(OBJS): | $(PROTO_HDRS)

build/%.o: src/%.c | build
	$(CC) $(HONEY_CFLAGS) -c $< -o $@

# -rdynamic exports honey's symbols so the crash backtrace names its own
# functions even when the packaged binary is stripped (.dynsym survives strip).
honey: $(OBJS)
	$(CC) $(HONEY_CFLAGS) $(LDFLAGS) -rdynamic -o $@ $^ $(LDLIBS)

# honeyctl is a standalone unix-socket client; no wlroots/wayland linkage.
honeyctl: honeyctl.c
	$(CC) $(CFLAGS) $(LDFLAGS) -std=c11 -D_GNU_SOURCE \
		-DHONEY_VERSION=\"$(VERSION)\" -Wall -Wextra -o $@ $<

# Header dependencies (-MMD) so editing a header recompiles every affected .o.
-include $(wildcard build/*.d)

build:
	mkdir -p build

PREFIX ?= $(HOME)/.local
install: all
	install -Dm755 honey    $(DESTDIR)$(PREFIX)/bin/honey
	install -Dm755 honeyctl $(DESTDIR)$(PREFIX)/bin/honeyctl

clean:
	rm -rf build honey honeyctl

.PHONY: all install clean
