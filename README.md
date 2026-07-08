# honey-wm

A tiling Wayland **compositor** built on [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots),
succeeding [lakewm](https://github.com/l3mah/lakewm) (which was a *client* of the
river compositor). Same Hyprland-style master-stack workflow — unlimited
per-monitor workspaces, pluggable layouts, dynamic runtime config — with no river
dependency and full control of rendering, input, decorations, and protocols.

Status: **bootstrapping** (M0). See [DESIGN.md](DESIGN.md) for the full plan,
architecture, module map, and milestones.

## Building

Built in an ephemeral nix-shell (nothing installed permanently), like lakewm:

```sh
nix-shell -p wlroots wayland wayland-protocols wayland-scanner libxkbcommon \
  pixman libinput libdrm pkg-config gcc gnumake --run make
```

## Components

- `honey` — the compositor. `honey -c <path>` runs an alternate config file
  instead of the default `~/.config/honey/init`; the path is remembered, so
  `honeyctl reload` re-runs the same file.
- `honeyctl` — control client (config is `~/.config/honey/init`, a shell script of
  `honeyctl` calls; the same commands reconfigure a running honey live).
- [honey-waybar](../honey-waybar) — waybar integration (succeeds lakewm-waybar).