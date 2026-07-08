# w3ld-wm

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

- `w3ld` — the compositor. `w3ld -c <path>` runs an alternate config file
  instead of the default `~/.config/w3ld/init`; the path is remembered, so
  `w3ldctl reload` re-runs the same file.
- `w3ldctl` — control client (config is `~/.config/w3ld/init`, a shell script of
  `w3ldctl` calls; the same commands reconfigure a running w3ld live).
- [w3ld-waybar](../w3ld-waybar) — waybar integration (succeeds lakewm-waybar).