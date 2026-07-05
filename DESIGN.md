# w3ld-wm — design & plan

**w3ld** (weld / "wwm") is a tiling Wayland **compositor** built directly on
**wlroots**, succeeding [lakewm](https://github.com/l3mah/lakewm). lakewm was a
*client* of the river compositor (it spoke `river-window-management-v1`); w3ld
**is** the compositor. Same window-management philosophy (Hyprland-style
master-stack, unlimited per-monitor workspaces, dynamic config), but with no
dependency on river and no ceiling on what the WM can control.

lakewm stays intact and frozen as a working river WM. w3ld is a fresh repo that
reuses lakewm's policy code, borrows dwl's compositor plumbing, and adds modern
wlroots managers river couldn't give us.

## Why leave river

lakewm was a privileged client of river. river did rendering, input, DRM/KMS,
XWayland, and all protocol implementations; lakewm decided policy via the
`river-window-management-v1` family. That split has a hard ceiling — things
**structurally impossible** for a river client that become trivial for a
compositor:

- **Decorations:** river only advertises `xdg-decoration`; GTK/older-Qt speak
  only `org_kde_kwin_server_decoration` (verified: GTK3/4 link only the KDE
  protocol; alacritty links xdg-decoration). A river client cannot advertise a
  Wayland global, so GTK apps always draw CSD headerbars. A compositor advertises
  **both** decoration managers — one line — and it's solved.
- **Standard bars:** a river client cannot serve `ext-workspace` /
  `wlr-foreign-toplevel`, so waybar's native workspace/taskbar modules never
  worked (needed the custom `lakectl subscribe` stream + lakewm-waybar). A
  compositor implements these → standard bars just work.
- **Native output config:** lakewm shelled out to `wlr-randr`. A compositor does
  `wlr-output-management` natively.
- **Effects & input reach:** rounding/opacity/blur/shadow, resize-on-border,
  scroll-to-switch, gamma control, tearing — all reachable only by owning
  rendering and input.

The LOC math also favors it: lakewm is ~6k LOC, of which a large slice is pure
"talk to river" overhead (input config reimplemented as a protocol client, the
double-buffered manage/render dance, shadow state). dwl is a *full* tiling
compositor in ~2.5k. Sitting on river is **not** free — the river tax ≈ the
compositor plumbing dwl writes. We pay a comparable amount either way; as a
compositor we get control, at the cost of owning DRM/session/XWayland.

## Architecture

- **wlroots (target 0.19/0.20 — pin via nix; river links 0.20.1).** Build in a
  nix-shell like lakewm.
- **`wlr_scene` for all rendering.** No hand-rolled render loop / damage
  tracking. Windows are scene trees; borders are `wlr_scene_rect`; z-order is a
  fixed array of scene-tree layers (dwl pattern: background / bottom / tiled /
  floating / top / overlay / fullscreen / lock).
- **Single source of truth, event-driven.** The `wlr_xdg_toplevel` /
  `wlr_output` *is* the state — no shadow copies, no resync. `wl_listener`
  signals (`new_xdg_toplevel`, `map`, `commit`, `new_output`, `new_input`,
  cursor motion, …) call policy synchronously. The `manage_start`/`render_start`
  double-buffer orchestration is gone.
- **Clean module split preserved** (lakewm's is good): policy modules port; new
  thin wlroots-glue modules added.

## Module map — port / donate / new

**Port from lakewm (the crown jewels — reuse in spirit, output into scene nodes
instead of `propose_dimensions`):**
- `layout.c` — pluggable stateless layouts (master / spiral / grid). Best-in-
  class; keep the registry + `arrange(ctx)` interface.
- `workspace.c` — per-output, unlimited, `output:N`-addressable workspaces. Do
  NOT adopt dwl's dwm-tags model.
- `action.c` — action handlers (focus/swap/workspace/dir/move/…).
- `binding.c` — keysym parsing (drop the per-seat river-object reconciliation;
  use `wlr_keyboard` directly).
- `config.c` — defaults + effective-value resolution.
- `ipc.c` + `w3ldctl.c` + `status.c` — runtime control socket, live reconfig,
  JSON status stream (the big advantage over dwl's compile-time `config.h`).
  Keep the `subscribe` schema (`v:1`) so w3ld-waybar reuses it.
- window rules (`apply_rules`).

**Donate from dwl (read + borrow; don't fork wholesale — its suckless/tags/
compile-time philosophy clashes with ours):**
- The `setup()` bootstrap sequence (backend → renderer → allocator → compositor
  → scene → managers → loop).
- The scene-layer array for z-ordering.
- **XWayland `xwm` glue** — the #1 donation; do not reinvent.
- Hard-won correctness fixes: focus-stealing, unmanaged/override-redirect X
  surfaces, activation/urgency, cursor-warp quirks, output hotplug races.

**Net-new thin wlroots glue:**
- `main.c` — wlroots init (backend/renderer/allocator/scene) + event loop.
- `output.c` — `wlr_output` + `wlr_output_layout` + `wlr_scene_output`.
- `window.c` — `wlr_xdg_toplevel` (+ popups) lifecycle.
- `seat.c` — `wlr_seat` + `wlr_cursor` + focus.
- `input.c` — `new_input`; configure libinput **directly**
  (`libinput_device_config_*`) — deletes lakewm's ~700-line protocol-client
  layer. Build xkb keymaps directly (already do this).
- `decoration.c` — advertise `xdg-decoration` **and**
  `wlr_server_decoration_manager` (default server mode) → GTK problem solved.
- `layer.c` — implement `wlr_layer_shell_v1` (scene layers + exclusive zones);
  cleaner than the river-layer-shell client dance.
- `xwayland.c` — `wlr_xwayland` + xwm.

## Protocol / manager global checklist

Most are one-liner `*_create()` calls in `setup()`. Being a good-citizen
compositor is where w3ld beats both lakewm and vanilla dwl.

Essential: `xdg_shell`, `layer_shell`, `xdg_decoration` + `kde_server_decoration`,
`xdg_output`, `output_manager` + `output_configuration`, `data_device` +
`primary_selection` + `data_control` (clipboard managers), `xdg_activation`,
`presentation_time`, `viewporter`, `fractional_scale`, `cursor_shape`,
`relative_pointer` + `pointer_constraints`, `virtual_pointer` + `virtual_keyboard`.

Ecosystem wins (impossible under river): `wlr_foreign_toplevel_management` +
`ext_workspace` (standard bars/taskbars), `wlr_gamma_control` (night light — drop
hyprsunset), `session_lock` (swaylock), `screencopy` + `export_dmabuf` /
`ext_image_copy_capture` (screenshots/screenshare), `tearing_control` (native
allow-tearing), `input_method` + `text_input` (fcitx/IME), `security_context`,
`content_type`, `alpha_modifier`, `single_pixel_buffer`, `drm_lease` (VR).

Optional later: effects (rounded corners / opacity / blur / shadow) via custom
scene buffers + shaders — SwayFX is the reference. **Ship without effects
first.**

## Milestones

- **M0** — repo scaffold: Makefile (nix-shell wlroots pkg-config), `main.c`
  bootstrap, empty module stubs, compiles + runs an empty compositor (nested +
  headless).
- **M1** — one output, `xdg_shell`, `wlr_scene`, master layout, keybinds,
  cursor. Smoke-test: spawn a terminal, it tiles; Super+Return works.
- **M2** — workspaces + multi-monitor + `wlr_output_layout` + native output
  config (kill wlr-randr). Port `workspace.c`/`action.c`.
- **M3** — layer-shell (bars/wallpaper) + decorations (both protocols) + input
  config (libinput direct) + IPC/`w3ldctl`/status + config `init`.
- **M4** — XWayland + `foreign-toplevel` + `ext-workspace` + the manager long-
  tail (gamma, session-lock, screencopy, activation, clipboard).
- **M5** — polish; port rules, floating/fullscreen/maximize, pointer drag,
  drop-at-cursor; optional effects.

## Conventions

- **Code style = lakewm's (carry over exactly):** factual comments (no
  first-person, no phase tags), explicit names, function defs `name (` space
  before paren, INLINE for 0–1 params, MULTI-LINE (one param per line, no
  trailing comma) for 2+, `) {` on its own line; section-banner comments; DRY.
- **Build:** nix-shell (wlroots, wayland, wayland-protocols, libxkbcommon,
  pixman, libinput, libdrm, pkg-config, gcc, gnumake, + xcb/xwayland for XWM).
  Plain Makefile; `wayland-scanner` for any custom protocol (probably none).
- **Headless test harness** (from lakewm): `WLR_BACKENDS=headless
  WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 WLR_HEADLESS_OUTPUTS=1`; short
  `XDG_RUNTIME_DIR` (<108-byte sun_path); kill test compositor by EXACT PID,
  never `pkill -f`.
- **Names:** compositor binary `w3ld`; control client `w3ldctl`; config
  `~/.config/w3ld/init` (shell script of `w3ldctl` calls); socket
  `$XDG_RUNTIME_DIR/w3ld-$WAYLAND_DISPLAY.sock`; bar consumer `w3ld-waybar`.
- **Commits:** no Claude co-author trailer.

## Non-goals (initially)

Effects (defer), a config-file format (runtime IPC is the config, like lakewm),
dwm-tags (use workspaces), Xorg support (Wayland + XWayland only).

## References

- lakewm (`../../lakewm` / github.com/l3mah/lakewm) — policy code + the
  `subscribe` schema + lakewm-waybar (rename → w3ld-waybar).
- dwl (codeberg.org/dwl/dwl) — compositor skeleton, scene layers, XWayland.
- tinywl (wlroots reference) — minimal `wlr_scene` bootstrap.
- sway / river — protocol wiring references.
- wlroots 0.20 headers — the API of record.