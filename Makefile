# w3ld — a tiling Wayland compositor on wlroots 0.20.
#
# The dev toolchain is not in the profile on this Nix host; build inside an
# ephemeral shell:
#
#   nix-shell -p wlroots wayland wayland-protocols wayland-scanner libxkbcommon \
#     pixman libinput libdrm pkg-config gcc gnumake --run make
#
# wlroots is the 0.20.1 nix attr (pkg-config name wlroots-0.20), matching river.

CC       ?= cc
SCANNER  ?= wayland-scanner
PKGS      = wlroots-0.20 wayland-server xkbcommon pixman-1 libinput
PKG_CFLAGS = $(shell pkg-config --cflags $(PKGS))
PKG_LIBS   = $(shell pkg-config --libs $(PKGS))

CFLAGS   ?= -g -Og
CFLAGS   += -std=c11 -D_GNU_SOURCE -DWLR_USE_UNSTABLE \
            -Wall -Wextra -Wpedantic -Wno-unused-parameter \
            -Wno-missing-field-initializers \
            -MMD -MP \
            -Isrc -Ibuild $(PKG_CFLAGS)
LDLIBS    = $(PKG_LIBS)

# Server-side headers for protocols wlroots implements but doesn't ship
# generated (its headers #include these by name).
PROTOCOLS = wlr-layer-shell-unstable-v1
PROTO_HDRS = $(PROTOCOLS:%=build/%-protocol.h)

SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:src/%.c=build/%.o)

all: w3ld w3ldctl

build/%-protocol.h: protocol/%.xml | build
	$(SCANNER) server-header $< $@

# Generated protocol headers must exist before any object is compiled.
$(OBJS): | $(PROTO_HDRS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

w3ld: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# w3ldctl is a standalone unix-socket client — no wlroots/wayland linkage.
w3ldctl: w3ldctl.c
	$(CC) -std=c11 -D_GNU_SOURCE -Wall -Wextra -o $@ $<

# Header dependencies (-MMD) so editing a header recompiles every affected .o.
-include $(wildcard build/*.d)

build:
	mkdir -p build

PREFIX ?= $(HOME)/.local
install: all
	install -Dm755 w3ld    $(DESTDIR)$(PREFIX)/bin/w3ld
	install -Dm755 w3ldctl $(DESTDIR)$(PREFIX)/bin/w3ldctl

clean:
	rm -rf build w3ld w3ldctl

.PHONY: all install clean
